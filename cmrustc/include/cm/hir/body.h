#ifndef CMRUSTC_CM_HIR_BODY_H
#define CMRUSTC_CM_HIR_BODY_H

#include "cm/hir/module_map.h"
#include "cm/resolve/imports.h"

typedef enum CmHirBodyLowerStatus {
    CM_HIR_BODY_LOWER_OK = 0,
    CM_HIR_BODY_LOWER_INVALID_ARGUMENT,
    CM_HIR_BODY_LOWER_INVALID_BODY,
    CM_HIR_BODY_LOWER_SOURCE_MISMATCH,
    CM_HIR_BODY_LOWER_UNSUPPORTED_BODY,
    CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE,
    CM_HIR_BODY_LOWER_UNRESOLVED_PATH,
    CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION,
    CM_HIR_BODY_LOWER_TYPE_MISMATCH,
    CM_HIR_BODY_LOWER_INVALID_LITERAL,
    CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE,
    CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR,
    CM_HIR_BODY_LOWER_HIR_FAILURE
} CmHirBodyLowerStatus;

typedef struct CmHirBodyLowerResult {
    CmHirBodyLowerStatus status;
    CmHirBodyId body;
    CmHirExprId root_expression;
    CmSpan span;
    CmHirStatus hir_status;
} CmHirBodyLowerResult;

/*
 * Append fully typed body-owned expressions.  These APIs perform no type
 * inference: the caller supplies the exact local/call result type, and the
 * model checks it against the stored local and instantiated callee signature.
 * Call arguments remain in source order and every referenced expression must
 * already belong to the same unlowered body.
 */
CmHirStatus cm_hir_body_add_local_expression(CmHirContext *context,
    CmHirBodyId body, uint32_t local_index, CmHirTypeId type, CmSpan span,
    CmHirExprId *out_expression);
CmHirStatus cm_hir_body_add_call_expression(CmHirContext *context,
    CmHirBodyId body, CmHirDefId callee,
    const CmHirTypeId *type_substitutions,
    uint32_t type_substitution_count, const CmHirExprId *arguments,
    uint32_t argument_count, CmHirTypeId result_type, CmSpan span,
    CmHirExprId *out_expression);
CmHirStatus cm_hir_body_add_binary_expression(CmHirContext *context,
    CmHirBodyId body, CmHirBinaryOperator operator_kind,
    CmHirExprId left, CmHirExprId right, CmHirTypeId result_type,
    CmSpan span, CmHirExprId *out_expression);
CmHirStatus cm_hir_body_add_if_expression(CmHirContext *context,
    CmHirBodyId body, CmHirExprId condition, CmHirExprId then_expression,
    CmHirExprId else_expression, CmHirTypeId result_type, CmSpan span,
    CmHirExprId *out_expression);

/*
 * Type one source-qualified body in the deliberately narrow executable HIR
 * slice. Accepted tails are decimal i32/u32/usize literals with either the
 * exact matching suffix or no suffix when this expected type is already fixed
 * by the surrounding return, explicit let, field, operand, branch, or call
 * position; no default literal type is inferred. Named parameter locals and
 * recursively typed u32 or usize expression trees combine
 * wrapping addition and subtraction with exact free-function calls,
 * value-producing `if u32 == u32 { u32 } else { u32 }` or
 * `if usize < usize { usize } else { usize }`, or complete construction of a
 * local nongeneric named struct. Conditions admit only the comparison coupled
 * to the exact result type, both branches must be safe statement-free blocks
 * with exact result-typed tails, and an absent else, else-if, if-let,
 * truthiness, mixed-width comparison, and every other comparison reject.
 * Calls, nested control flow, aggregate construction, and surrounding let
 * statements are outside this first control-flow slice. The if expression is
 * admitted only as the outer function body's tail, yielding one canonical
 * control-flow diamond for MIR.
 * Aggregate paths use the import resolver,
 * fields remain in
 * source order, and child expressions are recursively checked against each
 * declaration field type, including nested aggregates. Calls admit one or two
 * independently computed arguments to a body-bearing monomorphic callee. An
 * exact u32 result retains the existing u32-or-local-aggregate parameter
 * boundary; an exact usize result requires every parameter to be exact usize.
 * Aggregate expressions retain the same authenticated construction checks.
 * The exact one-type substitution identity callee remains u32-only. Grouping
 * parentheses are already erased by the AST. Aggregate
 * returns, exported aggregate ABI, inference, and fallback resolution remain
 * unsupported.
 *
 * The import resolver and module map are the exact snapshots used for graph
 * lowering. Together they authenticate the graph revision, path-resolution
 * environment, and HIR owner which produced the body's source-qualified AST
 * identity. Success changes the body from UNLOWERED to TYPED while retaining
 * that provenance. Call and aggregate payloads use one preflight-sized
 * transaction allocation owned by exactly one stored expression. Every
 * failure leaves the body, locals, interner, arena, and expression vector at
 * their exact pre-call semantic contents.
 */
CmHirBodyLowerResult cm_hir_lower_body(CmHirContext *context,
    CmHirBodyId body, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules);

const char *cm_hir_body_lower_status_name(CmHirBodyLowerStatus status);

#endif
