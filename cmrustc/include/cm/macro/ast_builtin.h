#ifndef CMRUSTC_CM_MACRO_AST_BUILTIN_H
#define CMRUSTC_CM_MACRO_AST_BUILTIN_H

#include "cm/macro.h"
#include "cm/syntax/parser.h"

typedef enum CmBuiltinAstStage {
    CM_BUILTIN_AST_STAGE_VALIDATE = 0,
    CM_BUILTIN_AST_STAGE_CLASSIFY,
    CM_BUILTIN_AST_STAGE_EXPAND,
    CM_BUILTIN_AST_STAGE_REPARSE,
    CM_BUILTIN_AST_STAGE_COMPLETE
} CmBuiltinAstStage;

typedef enum CmBuiltinAstDiagnosticKind {
    CM_BUILTIN_AST_DIAG_NONE = 0,
    CM_BUILTIN_AST_DIAG_INVALID_ARGUMENT,
    CM_BUILTIN_AST_DIAG_EXPECTED_MACRO_EXPRESSION,
    CM_BUILTIN_AST_DIAG_INVALID_MACRO_PATH,
    CM_BUILTIN_AST_DIAG_UNSUPPORTED_MACRO,
    CM_BUILTIN_AST_DIAG_BUILTIN_EXPANSION,
    CM_BUILTIN_AST_DIAG_GENERATED_EXPRESSION
} CmBuiltinAstDiagnosticKind;

typedef struct CmBuiltinAstResult {
    CmMacroStatus status;
    CmBuiltinAstStage stage;
    CmBuiltinAstDiagnosticKind kind;
    CmBuiltinMacroKind builtin;

    /* Stable diagnostic context copied before the AST is appended to. */
    CmAstExprId invocation_expression;
    CmAstSpan invocation_span;
    CmAstPathId invocation_path;

    /* Always NONE on failure; owned by the caller-provided AST on success. */
    CmAstExprId expanded_expression;
    size_t generated_source_length;

    /* Builtin diagnostics use offsets relative to invocation arguments. */
    CmMacroDiagnostic macro_diagnostic;
    /* Reparse diagnostics use offsets in the generated Rust source. */
    CmParseResult parse;
} CmBuiltinAstResult;

/*
 * Expands one expression-position builtin macro and reparses its generated
 * Rust source as exactly one expression appended to `ast`.  The caller owns
 * `ast`; no invocation is replaced or otherwise mutated.  A context is
 * required explicitly even for builtins that do not inspect it.
 */
CmBuiltinAstResult cm_builtin_ast_expand_expression(
    CmAst *ast,
    CmAstExprId invocation_expression,
    const CmBuiltinContext *context,
    enum cm_edition edition
);

const char *cm_builtin_ast_stage_name(CmBuiltinAstStage stage);
const char *cm_builtin_ast_diagnostic_kind_name(
    CmBuiltinAstDiagnosticKind kind);

#endif
