#ifndef CMRUSTC_CM_MIR_MODEL_H
#define CMRUSTC_CM_MIR_MODEL_H

#include <stdio.h>

#include "cm/hir/admission.h"
#include "cm/hir/model.h"
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

/* One source-independent declaration-ordinal field step. */
typedef struct CmMirFieldProjection {
    CmHirDefId definition;
    uint32_t field_index;
} CmMirFieldProjection;

/* A flattened local/temporary place with a bounded, source-independent path. */
typedef struct CmMirPlace {
    CmMirLocalId base;
    CmHirTypeId type;
    CmMirFieldProjection *projections;
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
    CM_MIR_RVALUE_LESS
} CmMirRvalueKind;

typedef struct CmMirRvalue {
    CmMirRvalueKind kind;
    CmHirTypeId type;
    /* Required for aggregate construction; zero remains valid for scalars. */
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
            CmHirDefId definition;
            /* Declaration order: fields[index].field_index must equal index. */
            CmMirAggregateField *fields;
            uint32_t field_count;
        } aggregate;
    } data;
} CmMirRvalue;

typedef struct CmMirInstance {
    CmHirDefId definition;
    CmHirTypeId *substitutions;
    uint32_t substitution_count;
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
    /* Identity only; exact bodies retain no borrowed HIR storage. */
    const CmHirContext *hir_owner;
    /* Nonzero only after a successful admission-gated publication. */
    CmHirCrateId admitted_crate;
    uint64_t admitted_storage_lifetime_id;
    uint64_t admitted_semantic_generation;
    uint64_t admitted_rewind_generation;
    /* Zero is the legacy target-neutral state; usize requires 32 or 64. */
    unsigned int pointer_bits;
} CmMirContext;

void cm_mir_context_init(CmMirContext *context);
void cm_mir_context_destroy(CmMirContext *context);

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

/* Resolve one already-published exact instance by its complete key. */
CmMirStatus cm_mir_find_instance(const CmMirContext *context,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmMirBodyId *out_id);

/* Validate a local place, including zero or more flattened field projections,
 * against one exact MIR/HIR body. */
CmMirStatus cm_mir_validate_place(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place);

const CmMirBody *cm_mir_get_body(const CmMirContext *context,
    CmMirBodyId id);
size_t cm_mir_body_count(const CmMirContext *context);
const char *cm_mir_status_name(CmMirStatus status);

/* Deterministic insertion-order form for schema and lowering regression tests. */
int cm_mir_dump(FILE *stream, const CmMirContext *context);

#endif
