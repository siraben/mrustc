#ifndef CMRUSTC_CM_MIR_MODEL_H
#define CMRUSTC_CM_MIR_MODEL_H

#include <stdio.h>

#include "cm/hir/admission.h"
#include "cm/hir/model.h"
#include "cm/hir/semantic_results.h"
#include "cm/vec.h"

/* MIR body IDs are one-based. Local and basic-block indices are zero-based. */
typedef uint32_t CmMirBodyId;
typedef uint32_t CmMirLocalId;
typedef uint32_t CmMirBasicBlockId;

#define CM_MIR_BODY_NONE ((CmMirBodyId)0u)
#define CM_MIR_RETURN_LOCAL ((CmMirLocalId)0u)
#define CM_MIR_ENTRY_BLOCK ((CmMirBasicBlockId)0u)
#define CM_MIR_MAX_PLACE_PROJECTIONS ((uint32_t)16u)
#define CM_MIR_MAX_AGGREGATE_FIELDS ((uint32_t)64u)

typedef enum CmMirStatus {
    CM_MIR_OK = 0,
    CM_MIR_INVALID_ARGUMENT,
    CM_MIR_INVALID_ID,
    CM_MIR_ID_EXHAUSTED,
    CM_MIR_INVARIANT_VIOLATION,
    CM_MIR_INVALID_ADMISSION
} CmMirStatus;

typedef enum CmMirLocalKind {
    CM_MIR_LOCAL_RETURN = 0,
    CM_MIR_LOCAL_ARGUMENT,
    CM_MIR_LOCAL_USER,
    CM_MIR_LOCAL_TEMPORARY
} CmMirLocalKind;

typedef struct CmMirLocal {
    CmMirLocalKind kind;
    CmHirTypeId type;
} CmMirLocal;

typedef enum CmMirPlaceProjectionKind {
    /* Zero preserves the representation of existing zero-initialized fields. */
    CM_MIR_PROJECTION_FIELD = 0,
    CM_MIR_PROJECTION_DEREFERENCE
} CmMirPlaceProjectionKind;

/*
 * One source-independent place step.  The field payload must be zero for a
 * dereference, which gives every accepted projection one canonical form.
 */
typedef struct CmMirPlaceProjection {
    CmMirPlaceProjectionKind kind;
    CmHirDefId definition;
    uint32_t field_index;
} CmMirPlaceProjection;

/* Compatibility spelling for callers that only construct field steps. */
typedef CmMirPlaceProjection CmMirFieldProjection;

/* A flattened local/temporary place with a bounded, source-independent path. */
typedef struct CmMirPlace {
    CmMirLocalId base;
    CmHirTypeId type;
    CmMirPlaceProjection *projections;
    uint32_t projection_count;
    /* Full source span of this place expression. */
    CmSpan span;
} CmMirPlace;

typedef enum CmMirOperandKind {
    CM_MIR_CONSTANT_I32 = 0,
    CM_MIR_CONSTANT_U32,
    CM_MIR_CONSTANT_USIZE,
    CM_MIR_OPERAND_MOVE,
    CM_MIR_OPERAND_MOVE_PLACE,
    CM_MIR_OPERAND_COPY_PLACE
} CmMirOperandKind;

typedef struct CmMirOperand {
    CmMirOperandKind kind;
    CmHirTypeId type;
    union {
        int32_t i32_value;
        uint32_t u32_value;
        uint64_t usize_value;
        CmMirLocalId local;
        CmMirPlace place;
    } data;
} CmMirOperand;

/* One declaration-ordered aggregate field paired with its exact ordinal. */
typedef struct CmMirAggregateField {
    uint32_t field_index;
    CmMirOperand value;
} CmMirAggregateField;

/* Compatibility names retained for the original signed-literal slice. */
typedef CmMirOperandKind CmMirConstantKind;
typedef CmMirOperand CmMirConstant;

typedef enum CmMirBinaryOp {
    /* Unsigned arithmetic wraps at the exact u32 or target-usize width. */
    CM_MIR_BINARY_ADD = 0,
    CM_MIR_BINARY_SUBTRACT
} CmMirBinaryOp;

typedef enum CmMirRvalueKind {
    CM_MIR_RVALUE_USE = 0,
    CM_MIR_RVALUE_BINARY,
    CM_MIR_RVALUE_AGGREGATE,
    /* Exact u32 equality producing a bool temporary. */
    CM_MIR_RVALUE_EQUAL,
    /* Exact target-usize ordering producing a bool temporary. */
    CM_MIR_RVALUE_LESS,
    /* Address of one validated place with an explicit borrow capability. */
    CM_MIR_RVALUE_BORROW
} CmMirRvalueKind;

typedef enum CmMirBorrowKind {
    CM_MIR_BORROW_SHARED = 0,
    CM_MIR_BORROW_MUTABLE
} CmMirBorrowKind;

typedef struct CmMirRvalue {
    CmMirRvalueKind kind;
    CmHirTypeId type;
    /* Required for aggregate construction and borrowing. */
    CmSpan span;
    union {
        CmMirOperand use;
        struct {
            CmMirBinaryOp operator_kind;
            CmMirOperand left;
            CmMirOperand right;
        } binary;
        struct {
            CmMirOperand left;
            CmMirOperand right;
        } equal;
        struct {
            CmMirOperand left;
            CmMirOperand right;
        } less;
        struct {
            CmMirBorrowKind kind;
            CmMirPlace source;
        } borrow;
        struct {
            CmHirDefId definition;
            /* Declaration order: fields[index].field_index must equal index. */
            CmMirAggregateField *fields;
            uint32_t field_count;
        } aggregate;
    } data;
} CmMirRvalue;

typedef struct CmMirInstance {
    /* Dispatch/symbol identity selected for this concrete instance. */
    CmHirDefId definition;
    /* Definition whose signature and HIR body are executed. */
    CmHirDefId body_definition;
    /*
     * Transitional executable materialization for the original narrow
     * generic lowering path.  These process-local HIR IDs are not part of a
     * canonical instance's identity.
     */
    CmHirTypeId *substitutions;
    uint32_t substitution_count;
    /*
     * Durable structural identity.  An exact canonical instance has a
     * nonzero body and a nonempty byte string.  MIR owners deep-copy these
     * bytes; callers may pass borrowed storage to reserve/find operations.
     */
    CmHirBodyId body;
    unsigned char *identity_bytes;
    size_t identity_size;
} CmMirInstance;

typedef enum CmMirSemanticEvidenceKind {
    CM_MIR_SEMANTIC_EVIDENCE_NONE = 0,
    CM_MIR_SEMANTIC_EVIDENCE_BODY,
    CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
} CmMirSemanticEvidenceKind;

typedef enum CmMirStatementKind {
    CM_MIR_STATEMENT_ASSIGN = 0
} CmMirStatementKind;

typedef struct CmMirStatement {
    CmMirStatementKind kind;
    union {
        struct {
            /* Compatibility scalar base; equals destination_place.base when
             * the explicit place is present. */
            CmMirLocalId destination;
            CmMirPlace destination_place;
            CmMirRvalue value;
        } assign;
    } data;
} CmMirStatement;

typedef enum CmMirTerminatorKind {
    CM_MIR_TERMINATOR_RETURN = 0,
    CM_MIR_TERMINATOR_CALL,
    CM_MIR_TERMINATOR_GOTO,
    CM_MIR_TERMINATOR_SWITCH_BOOL
} CmMirTerminatorKind;

typedef struct CmMirTerminator {
    CmMirTerminatorKind kind;
    union {
        struct {
            /* Compatibility scalar base; equals destination_place.base when
             * the explicit place is present. */
            CmMirLocalId destination;
            CmMirPlace destination_place;
            CmMirOperand *arguments;
            uint32_t argument_count;
            /* An already-published body is one resolved exact instance. */
            CmMirBodyId callee_instance;
            /* The resolution key must exactly match that published body. */
            CmMirInstance callee;
            CmMirBasicBlockId target;
        } call;
        struct {
            CmMirBasicBlockId target;
        } goto_block;
        struct {
            CmMirOperand condition;
            CmMirBasicBlockId true_target;
            CmMirBasicBlockId false_target;
        } switch_bool;
    } data;
} CmMirTerminator;

typedef struct CmMirBasicBlock {
    CmMirStatement *statements;
    uint32_t statement_count;
    CmMirTerminator terminator;
} CmMirBasicBlock;

typedef struct CmMirBody {
    /* Empty only for the compatibility literal-body path. */
    CmMirInstance instance;
    CmHirDefId owner;
    CmHirBodyId source_body;
    /* Selects the sealed semantic-results namespace used for publication. */
    CmMirSemanticEvidenceKind semantic_evidence;
    CmMirLocal *locals;
    uint32_t local_count;
    CmMirBasicBlock *basic_blocks;
    uint32_t basic_block_count;
    /* Private deep-copy allocation; callers must initialize it to NULL. */
    void *owned_storage;
} CmMirBody;

typedef struct CmMirContext {
    CmVec bodies;
    /* Process-unique identity of this initialized context lifetime. */
    uint64_t lifetime_id;
    /* Identity only; exact bodies retain no borrowed HIR storage. */
    const CmHirContext *hir_owner;
    /* Nonzero only after a successful admission-gated publication. */
    CmHirCrateId admitted_crate;
    uint64_t admitted_storage_lifetime_id;
    uint64_t admitted_semantic_generation;
    uint64_t admitted_rewind_generation;
    /* Exact semantic authority which admitted every retained body. */
    uint64_t admitted_admission_capability_id;
    /* Exact REGIONS authority from which that admission was derived. */
    uint64_t admitted_barrier_capability_id;
    /* Whole-local REGIONS semantic authority used to discover the slice. */
    uint64_t admitted_parent_capability_id;
    /* Zero is the legacy target-neutral state; usize requires 32 or 64. */
    unsigned int pointer_bits;
} CmMirContext;

/* An isolated exact-body publication transaction.  Its implementation is
 * private; initialize before use and destroy after either commit or abort.
 * The context and admission objects must remain addressable until destroy;
 * destroying or reinitializing either invalidates the transaction. */
typedef struct CmMirPublication {
    void *implementation;
} CmMirPublication;

void cm_mir_context_init(CmMirContext *context);
void cm_mir_context_destroy(CmMirContext *context);

void cm_mir_publication_init(CmMirPublication *publication);
void cm_mir_publication_destroy(CmMirPublication *publication);
CmMirStatus cm_mir_publication_begin(CmMirPublication *publication,
    CmMirContext *context, const CmSemanticAdmission *admission);
/* Production boundary: additionally require an exact bound REGIONS authority. */
CmMirStatus cm_mir_publication_begin_regions(CmMirPublication *publication,
    CmMirContext *context, const CmSemanticAdmission *admission);
CmMirStatus cm_mir_publication_reserve(CmMirPublication *publication,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirBodyId source_body,
    CmMirBodyId *out_id);
/* Reserve by complete structural identity while retaining any borrowed flat
 * substitutions solely as executable materialization during migration. */
CmMirStatus cm_mir_publication_reserve_canonical(
    CmMirPublication *publication, const CmMirInstance *instance,
    CmHirBodyId source_body, CmMirBodyId *out_id);
CmMirStatus cm_mir_publication_find_instance(
    const CmMirPublication *publication, CmHirDefId definition,
    const CmHirTypeId *substitutions, uint32_t substitution_count,
    CmMirBodyId *out_id);
CmMirStatus cm_mir_publication_find_canonical(
    const CmMirPublication *publication, const CmMirInstance *instance,
    CmMirBodyId *out_id);
/* The returned instance storage is borrowed from the publication. */
CmMirStatus cm_mir_publication_get_instance(
    const CmMirPublication *publication, CmMirBodyId id,
    CmMirInstance *out_instance, CmHirBodyId *out_source_body);
const CmMirBody *cm_mir_publication_get_body(
    const CmMirPublication *publication, CmMirBodyId id);
CmMirStatus cm_mir_publication_define(CmMirPublication *publication,
    CmMirBodyId id, const CmMirBody *body);
CmMirStatus cm_mir_publication_validate(
    const CmMirPublication *publication);
CmMirStatus cm_mir_publication_commit(CmMirPublication *publication);

/* Qualify an empty context for target-usize MIR. Repeated equal settings are
 * harmless; changing a set width or changing any published context rejects. */
CmMirStatus cm_mir_context_set_pointer_bits(CmMirContext *context,
    unsigned int pointer_bits);
unsigned int cm_mir_context_pointer_bits(const CmMirContext *context);

/* Inputs are validated and deeply copied; rejection does not mutate context. */
CmMirStatus cm_mir_add_body(CmMirContext *context, const CmMirBody *body,
    CmMirBodyId *out_id);

/*
 * Add one exact monomorphized function instance.  The HIR argument validates
 * the DefId, ordered type substitutions, and fully substituted signature, but
 * all retained MIR storage is independently owned by the MIR context.  The
 * initial boundary accepts typed local/constant returns, wrapping u32
 * addition/subtraction trees, and resolved direct calls. Generic call
 * parameters remain exact u32; monomorphic internal calls may additionally
 * move checked same-crate named aggregates into one or two parameters, with
 * an exact u32 destination and result.
 */
CmMirStatus cm_mir_add_monomorphized_body(CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, CmMirBodyId *out_id);

/* Publish only under current, exact local-crate semantic evidence. */
CmMirStatus cm_mir_add_admitted_monomorphized_body(CmMirContext *context,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    CmMirBodyId *out_id);

/* Revalidate one already-published exact body without mutating either model. */
CmMirStatus cm_mir_validate_monomorphized_body(
    const CmMirContext *context, const CmHirContext *hir, CmMirBodyId id);

/* Revalidate only a context latched to this exact admitted generation. */
CmMirStatus cm_mir_validate_admitted_monomorphized_body(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id);

/*
 * Borrow the sealed signature selected for one admitted MIR body.  Exact
 * instance reconstruction remains private to MIR; stale or foreign admission
 * returns CM_MIR_INVALID_ADMISSION and clears the requested view.
 */
CmMirStatus cm_mir_admitted_signature(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id, CmSemanticFunctionSignatureView *out_view);
CmMirStatus cm_mir_admitted_signature_parameter(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id, uint32_t parameter, CmSemanticTypeView *out_view);

/* Resolve one already-published exact instance by its complete key. */
CmMirStatus cm_mir_find_instance(const CmMirContext *context,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmMirBodyId *out_id);
CmMirStatus cm_mir_find_canonical(const CmMirContext *context,
    const CmMirInstance *instance, CmMirBodyId *out_id);

/* Validate a local place, including zero or more flattened field projections,
 * against one exact MIR/HIR body. */
CmMirStatus cm_mir_validate_place(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place);

/* Validate one rvalue against an exact MIR/HIR body without publishing it. */
CmMirStatus cm_mir_validate_rvalue(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits);

const CmMirBody *cm_mir_get_body(const CmMirContext *context,
    CmMirBodyId id);
size_t cm_mir_body_count(const CmMirContext *context);
const char *cm_mir_status_name(CmMirStatus status);

/* Deterministic insertion-order form for schema and lowering regression tests. */
int cm_mir_dump(FILE *stream, const CmMirContext *context);

#endif
