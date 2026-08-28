#ifndef CMRUSTC_CM_MIR_ULOWER_H
#define CMRUSTC_CM_MIR_ULOWER_H

#include "cm/hir/tyck.h"
#include "cm/vec.h"

/*
 * M9-05 ubody->MIR lowering, grown by census.  The v1 walker executes the
 * lowering's control-flow skeleton over every fully typed ubody and
 * classifies each body: lowered (every construct in the v1 vocabulary) or
 * first-blocked by a named construct class.  Real MIR construction lands
 * class-by-class behind the same walk.
 */

#define CM_MIR_ULOWER_CLASSES 40u

typedef struct CmMirULowerResult {
    size_t bodies;      /* mir-ready bodies attempted */
    size_t lowered;     /* full walk succeeded */
    size_t blocked;     /* walk hit an unsupported construct */
    size_t statements;  /* v1 construction: assigns the build would emit */
    size_t blocks;      /* v1 construction: extra basic blocks */
    struct {
        const char *reason;
        size_t count;
    } classes[CM_MIR_ULOWER_CLASSES];
    size_t class_count;
} CmMirULowerResult;

CmMirULowerResult cm_mir_ulower_all(const CmHirContext *hir,
    const CmUBodySet *bodies, const CmTyckSet *tyck);

/*
 * u-MIR: the arena-owned MIR built from typed ubodies.  Operands carry
 * their defining ubody expression and its tyck arena type; the C emitter
 * consumes these directly.  One statement is one assignment of an rvalue
 * to a temporary or named local; control flow is explicit blocks.
 */

typedef uint32_t CmUMirLocalId;
typedef uint32_t CmUMirBlockId;

typedef enum CmUMirRvalueKind {
    CM_UMIR_RVALUE_LITERAL = 0,   /* expr carries the literal */
    CM_UMIR_RVALUE_LOCAL,         /* copy/move of another local */
    CM_UMIR_RVALUE_BINARY,
    CM_UMIR_RVALUE_UNARY,
    CM_UMIR_RVALUE_CALL,          /* callee + args are prior locals */
    CM_UMIR_RVALUE_METHOD_CALL,   /* receiver + args are prior locals */
    CM_UMIR_RVALUE_REF,           /* borrow of a prior local */
    CM_UMIR_RVALUE_CAST,
    CM_UMIR_RVALUE_ASSIGN,        /* store value into target place */
    CM_UMIR_RVALUE_FIELD,         /* place read: field / tuple index */
    CM_UMIR_RVALUE_AGGREGATE,     /* tuple / array / struct literal */
    CM_UMIR_RVALUE_INDEX,         /* place read: base[index] */
    CM_UMIR_RVALUE_TRY_UNWRAP,    /* ok-value of a ? operand */
    CM_UMIR_RVALUE_ITER_NEXT,     /* next() element in a for loop */
    CM_UMIR_RVALUE_CLOSURE,       /* closure value; body lowered at use */
    CM_UMIR_RVALUE_ASM,           /* retained inline-asm text (S10) */
    CM_UMIR_RVALUE_OFFSET_OF,     /* retained offset_of text (S10) */
    CM_UMIR_RVALUE_VARIANT,       /* enum ctor: slot[0]=immediate, fields */
    CM_UMIR_RVALUE_SLOT,          /* payload read: operands[0][immediate] */
    CM_UMIR_RVALUE_STORE_FIELD,   /* operands[0].field(expr) = operands[1] */
    CM_UMIR_RVALUE_STORE_INDEX,   /* operands[0][operands[1]] = operands[2] */
    CM_UMIR_RVALUE_STORE_DEREF,   /* *operands[0] = operands[1] */
    CM_UMIR_RVALUE_OPAQUE         /* representable later; keeps type */
} CmUMirRvalueKind;

#define CM_UMIR_STATEMENT_OPERANDS 4u

typedef struct CmUMirStatement {
    CmUMirLocalId destination;
    CmUMirRvalueKind kind;
    CmUExprId expr;      /* originating ubody expression */
    CmTyId type;         /* tyck arena type of the destination */
    /* Operand locals in evaluation order; calls with more arguments
     * carry the overflow count so emission can recover them from the
     * preceding statements. */
    CmUMirLocalId operands[CM_UMIR_STATEMENT_OPERANDS];
    uint32_t operand_count;
    uint32_t operand_overflow;
    /* VARIANT: discriminant index; SLOT: slot index. */
    uint32_t immediate;
} CmUMirStatement;

typedef enum CmUMirTerminatorKind {
    CM_UMIR_TERMINATOR_RETURN = 0,
    CM_UMIR_TERMINATOR_NONE,      /* not yet sealed (builder-internal) */
    CM_UMIR_TERMINATOR_GOTO,
    CM_UMIR_TERMINATOR_SWITCH_BOOL,
    /* Match dispatch: condition local selects among arm blocks that all
     * rejoin at goto_target; arm targets live in the arm blocks chain
     * starting at true_target. */
    CM_UMIR_TERMINATOR_SWITCH
} CmUMirTerminatorKind;

typedef struct CmUMirBlock {
    CmVec statements;              /* CmUMirStatement */
    CmUMirTerminatorKind terminator;
    CmUMirBlockId goto_target;
    CmUMirBlockId true_target;
    CmUMirBlockId false_target;
    CmUMirLocalId condition;
    /* SWITCH: arm targets with their discriminants (-1 = default). */
    CmUMirBlockId *arm_targets;
    long *arm_discriminants;
    uint32_t arm_count;
} CmUMirBlock;

typedef struct CmUMirBody {
    CmHirBodyId source;
    CmVec locals;                  /* CmTyId per local (0 = return slot) */
    CmVec blocks;                  /* CmUMirBlock */
    int complete;                  /* every node emitted precisely */
} CmUMirBody;

typedef struct CmUMirSet {
    CmVec bodies;                  /* CmUMirBody, index = body id - 1 */
} CmUMirSet;

void cm_umir_set_init(CmUMirSet *set);
void cm_umir_set_destroy(CmUMirSet *set);

/* Build u-MIR for every walkable typed body; result counts constructed
 * bodies and classifies gaps. The set owns all storage. */
CmMirULowerResult cm_mir_ulower_build(CmUMirSet *out,
    const CmHirContext *hir, const CmUBodySet *bodies,
    const CmTyckSet *tyck);

#endif
