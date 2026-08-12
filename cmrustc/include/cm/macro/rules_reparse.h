#ifndef CMRUSTC_CM_MACRO_RULES_REPARSE_H
#define CMRUSTC_CM_MACRO_RULES_REPARSE_H

#include "cm/macro/syntax_adapter.h"
#include "cm/syntax/parser.h"

#define CM_MACRO_REPARSE_DEFAULT_MAX_OUTPUT_BYTES ((size_t)16777216u)
#define CM_MACRO_REPARSE_DEFAULT_MAX_ITEMS ((uint32_t)65536u)

typedef enum CmMacroReparseStage {
    CM_MACRO_REPARSE_STAGE_VALIDATE = 0,
    CM_MACRO_REPARSE_STAGE_EXPAND,
    CM_MACRO_REPARSE_STAGE_OUTPUT_LIMIT,
    CM_MACRO_REPARSE_STAGE_PARSE,
    CM_MACRO_REPARSE_STAGE_ITEM_LIMIT,
    CM_MACRO_REPARSE_STAGE_COMPLETE
} CmMacroReparseStage;

typedef enum CmMacroReparseDiagnosticKind {
    CM_MACRO_REPARSE_DIAG_NONE = 0,
    CM_MACRO_REPARSE_DIAG_INVALID_ARGUMENT,
    CM_MACRO_REPARSE_DIAG_EXPECTED_EXPRESSION_INVOCATION,
    CM_MACRO_REPARSE_DIAG_EXPECTED_ITEM_INVOCATION,
    CM_MACRO_REPARSE_DIAG_EXPANSION,
    CM_MACRO_REPARSE_DIAG_OUTPUT_LIMIT,
    CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX,
    CM_MACRO_REPARSE_DIAG_ITEM_LIMIT
} CmMacroReparseDiagnosticKind;

typedef struct CmMacroReparseOptions {
    CmMacroSyntaxOptions expansion;
    CmItemListFragmentContext item_context;
    size_t maximum_output_bytes;
    uint32_t maximum_items;
} CmMacroReparseOptions;

typedef struct CmMacroReparseResult {
    CmMacroStatus status;
    CmMacroReparseStage stage;
    CmMacroReparseDiagnosticKind kind;
    const char *message;
    int expansion_attempted;
    int reparse_attempted;
    CmMacroSyntaxResult expansion;
    CmParseResult reparse;
    size_t generated_length;
    CmAstExprId expression;
    const CmAstItemId *items;
    uint32_t item_count;
} CmMacroReparseResult;

void cm_macro_reparse_options_init(CmMacroReparseOptions *options);

/*
 * Expand an explicitly paired macro_rules item and expression-position macro
 * invocation, then parse the exact transcription as one expression.  No name
 * lookup or scope search is performed.  The returned ID is owned by
 * `destination` and remains stable until that AST is destroyed.
 */
CmMacroReparseResult cm_macro_rules_reparse_expression(
    const CmAst *definition_ast,
    CmAstItemId definition_item,
    const CmAst *invocation_ast,
    CmAstExprId invocation_expression,
    CmAst *destination,
    const CmMacroReparseOptions *options
);

/*
 * Expand an explicitly paired macro_rules item and item-position macro
 * invocation, then parse the exact transcription as an item list.  The
 * returned ID array is owned by `destination`, must not be freed, and is not
 * inserted into destination->root_items.  Failed reparsing can leave
 * unreachable recovery nodes in the destination AST, but public IDs/counts
 * are always cleared.
 */
CmMacroReparseResult cm_macro_rules_reparse_items(
    const CmAst *definition_ast,
    CmAstItemId definition_item,
    const CmAst *invocation_ast,
    CmAstItemId invocation_item,
    CmAst *destination,
    const CmMacroReparseOptions *options
);

/*
 * Expands the resolver-authenticated compiler builtin `cfg_select!` in item
 * position.  The first true cfg predicate wins; `_` is an optional final
 * fallback.  This entry point performs no name lookup or authentication.
 */
CmMacroReparseResult cm_cfg_select_reparse_items(
    const CmAst *invocation_ast,
    CmAstItemId invocation_item,
    const CmCfgEnvironment *environment,
    CmAst *destination,
    const CmMacroReparseOptions *options
);

const char *cm_macro_reparse_stage_name(CmMacroReparseStage stage);
const char *cm_macro_reparse_diagnostic_kind_name(
    CmMacroReparseDiagnosticKind kind);

#endif
