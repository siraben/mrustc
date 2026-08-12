#ifndef CMRUSTC_CM_MACRO_SYNTAX_ADAPTER_H
#define CMRUSTC_CM_MACRO_SYNTAX_ADAPTER_H

#include "cm/macro_rules.h"
#include "cm/syntax/ast.h"

typedef enum CmMacroSyntaxStage {
    CM_MACRO_SYNTAX_STAGE_VALIDATE = 0,
    CM_MACRO_SYNTAX_STAGE_DEFINITION_TREE,
    CM_MACRO_SYNTAX_STAGE_RULES_PARSE,
    CM_MACRO_SYNTAX_STAGE_INVOCATION_TREE,
    CM_MACRO_SYNTAX_STAGE_RULES_MATCH,
    CM_MACRO_SYNTAX_STAGE_RULES_TRANSCRIBE,
    CM_MACRO_SYNTAX_STAGE_COMPLETE
} CmMacroSyntaxStage;

typedef enum CmMacroSyntaxDiagnosticKind {
    CM_MACRO_SYNTAX_DIAG_NONE = 0,
    CM_MACRO_SYNTAX_DIAG_INVALID_ARGUMENT,
    CM_MACRO_SYNTAX_DIAG_EXPECTED_NAMED_RULES,
    CM_MACRO_SYNTAX_DIAG_EXPECTED_INVOCATION,
    CM_MACRO_SYNTAX_DIAG_INVALID_DEFINITION_TREE,
    CM_MACRO_SYNTAX_DIAG_INVALID_INVOCATION_TREE,
    CM_MACRO_SYNTAX_DIAG_RULES
} CmMacroSyntaxDiagnosticKind;

typedef struct CmMacroSyntaxOptions {
    enum cm_edition edition;
    CmMacroRulesLimits limits;
    /* Validated defining-crate identifier used only for `$crate`. */
    const char *crate_identifier;
} CmMacroSyntaxOptions;

typedef struct CmMacroSyntaxResult {
    CmMacroStatus status;
    CmMacroSyntaxStage stage;
    CmMacroSyntaxDiagnosticKind kind;
    CmMacroDiagnostic diagnostic;
    size_t lexer_error_count;
    size_t delimiter_error_count;
    size_t arm_index;
    size_t capture_count;
    size_t backtrack_steps;
    size_t emitted_repetitions;
} CmMacroSyntaxResult;

void cm_macro_syntax_options_init(CmMacroSyntaxOptions *options);

/*
 * Expands one explicitly paired macro_rules definition and invocation.
 * No name lookup, scope registry, hygiene, AST mutation, or output reparsing
 * is performed.  Exact AST interior text is wrapped in its recorded outer
 * delimiter before token-tree construction.  Output is cleared on failure.
 */
CmMacroSyntaxResult cm_macro_syntax_expand(
    const CmAst *definition_ast,
    CmAstItemId definition_item,
    const CmAst *invocation_ast,
    const CmAstMacroInvocation *invocation,
    const CmMacroSyntaxOptions *options,
    CmStrBuf *output
);

/* Convenience entry point for an item-position macro invocation. */
CmMacroSyntaxResult cm_macro_syntax_expand_item(
    const CmAst *definition_ast,
    CmAstItemId definition_item,
    const CmAst *invocation_ast,
    CmAstItemId invocation_item,
    const CmMacroSyntaxOptions *options,
    CmStrBuf *output
);

const char *cm_macro_syntax_stage_name(CmMacroSyntaxStage stage);
const char *cm_macro_syntax_diagnostic_kind_name(
    CmMacroSyntaxDiagnosticKind kind);

#endif
