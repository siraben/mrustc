#include "cm/macro/ast_builtin.h"

#include <string.h>

static CmBuiltinAstResult cm_builtin_ast_result_init(
    CmAstExprId invocation_expression)
{
    CmBuiltinAstResult result;

    memset(&result, 0, sizeof(result));
    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.stage = CM_BUILTIN_AST_STAGE_VALIDATE;
    result.kind = CM_BUILTIN_AST_DIAG_INVALID_ARGUMENT;
    result.builtin = CM_BUILTIN_MACRO_UNKNOWN;
    result.invocation_expression = invocation_expression;
    result.expanded_expression = CM_AST_EXPR_NONE;
    result.macro_diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    result.macro_diagnostic.message =
        "invalid builtin AST adapter argument";
    return result;
}

static void cm_builtin_ast_error(CmBuiltinAstResult *result,
    CmMacroStatus status, CmBuiltinAstStage stage,
    CmBuiltinAstDiagnosticKind kind, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    result->status = status;
    result->stage = stage;
    result->kind = kind;
    result->expanded_expression = CM_AST_EXPR_NONE;
    result->macro_diagnostic.code = code;
    result->macro_diagnostic.offset = offset;
    result->macro_diagnostic.message = message;
}

static int cm_builtin_ast_edition_valid(enum cm_edition edition)
{
    return edition == CM_EDITION_2015 || edition == CM_EDITION_2018 ||
        edition == CM_EDITION_2021 || edition == CM_EDITION_2024;
}

CmBuiltinAstResult cm_builtin_ast_expand_expression(CmAst *ast,
    CmAstExprId invocation_expression, const CmBuiltinContext *context,
    enum cm_edition edition)
{
    CmBuiltinAstResult result;
    const CmAstExpr *invocation;
    const CmAstPath *path;
    const CmAstPathSegment *last_segment;
    const CmInternedString *name;
    const CmInternedString *arguments;
    CmStrBuf generated;
    CmMacroExpansion expansion;
    CmExpressionFragment fragment;

    result = cm_builtin_ast_result_init(invocation_expression);
    if (ast == NULL) return result;
    invocation = cm_ast_get_expr(ast, invocation_expression);
    if (invocation == NULL) {
        cm_builtin_ast_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_BUILTIN_AST_STAGE_VALIDATE,
            CM_BUILTIN_AST_DIAG_EXPECTED_MACRO_EXPRESSION,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "expected an expression-position macro invocation");
        return result;
    }
    result.invocation_span = invocation->span;
    if (invocation->kind != CM_AST_EXPR_MACRO) {
        cm_builtin_ast_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_BUILTIN_AST_STAGE_VALIDATE,
            CM_BUILTIN_AST_DIAG_EXPECTED_MACRO_EXPRESSION,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "expected an expression-position macro invocation");
        return result;
    }
    result.invocation_path = invocation->data.macro_expr.path;
    if (context == NULL || !cm_builtin_ast_edition_valid(edition))
        return result;
    path = cm_ast_get_path(ast, invocation->data.macro_expr.path);
    if (path == NULL || path->segment_count == 0u) {
        cm_builtin_ast_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_BUILTIN_AST_STAGE_CLASSIFY,
            CM_BUILTIN_AST_DIAG_INVALID_MACRO_PATH,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "macro invocation has no path segment to classify");
        return result;
    }
    last_segment = &path->segments[path->segment_count - 1u];
    name = cm_ast_get_string(ast, last_segment->name);
    arguments = cm_ast_get_string(ast,
        invocation->data.macro_expr.arguments);
    if (name == NULL || name->len == 0u || arguments == NULL) {
        cm_builtin_ast_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_BUILTIN_AST_STAGE_CLASSIFY,
            CM_BUILTIN_AST_DIAG_INVALID_MACRO_PATH,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "macro invocation path or arguments are invalid");
        return result;
    }
    result.builtin = cm_builtin_macro_classify((const char *)name->bytes,
        name->len);
    if (result.builtin == CM_BUILTIN_MACRO_UNKNOWN) {
        cm_builtin_ast_error(&result, CM_MACRO_UNSUPPORTED,
            CM_BUILTIN_AST_STAGE_CLASSIFY,
            CM_BUILTIN_AST_DIAG_UNSUPPORTED_MACRO,
            CM_MACRO_DIAG_BUILTIN_UNKNOWN, 0u,
            "macro path does not name a supported builtin macro");
        return result;
    }

    cm_str_buf_init(&generated);
    expansion = cm_builtin_macro_expand(result.builtin,
        (const char *)arguments->bytes, arguments->len, context, &generated);
    result.generated_source_length = generated.len;
    result.macro_diagnostic = expansion.diagnostic;
    if (expansion.status != CM_MACRO_OK) {
        result.status = expansion.status;
        result.stage = CM_BUILTIN_AST_STAGE_EXPAND;
        result.kind = CM_BUILTIN_AST_DIAG_BUILTIN_EXPANSION;
        result.expanded_expression = CM_AST_EXPR_NONE;
        cm_str_buf_destroy(&generated);
        return result;
    }

    fragment = cm_parse_expression_fragment(ast,
        cm_str_buf_c_str(&generated), generated.len, edition);
    result.parse = fragment.parse;
    if (fragment.parse.error_count != 0u ||
        fragment.expression == CM_AST_EXPR_NONE ||
        cm_ast_get_expr(ast, fragment.expression) == NULL) {
        cm_builtin_ast_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_BUILTIN_AST_STAGE_REPARSE,
            CM_BUILTIN_AST_DIAG_GENERATED_EXPRESSION,
            CM_MACRO_DIAG_NONE, fragment.parse.first_error.offset,
            "builtin expansion did not parse as exactly one expression");
        cm_str_buf_destroy(&generated);
        return result;
    }
    result.status = CM_MACRO_OK;
    result.stage = CM_BUILTIN_AST_STAGE_COMPLETE;
    result.kind = CM_BUILTIN_AST_DIAG_NONE;
    result.expanded_expression = fragment.expression;
    result.macro_diagnostic.code = CM_MACRO_DIAG_NONE;
    result.macro_diagnostic.offset = 0u;
    result.macro_diagnostic.message = "";
    cm_str_buf_destroy(&generated);
    return result;
}

const char *cm_builtin_ast_stage_name(CmBuiltinAstStage stage)
{
    switch (stage) {
    case CM_BUILTIN_AST_STAGE_VALIDATE: return "validate";
    case CM_BUILTIN_AST_STAGE_CLASSIFY: return "classify";
    case CM_BUILTIN_AST_STAGE_EXPAND: return "expand";
    case CM_BUILTIN_AST_STAGE_REPARSE: return "reparse";
    case CM_BUILTIN_AST_STAGE_COMPLETE: return "complete";
    }
    return "unknown";
}

const char *cm_builtin_ast_diagnostic_kind_name(
    CmBuiltinAstDiagnosticKind kind)
{
    switch (kind) {
    case CM_BUILTIN_AST_DIAG_NONE: return "none";
    case CM_BUILTIN_AST_DIAG_INVALID_ARGUMENT: return "invalid argument";
    case CM_BUILTIN_AST_DIAG_EXPECTED_MACRO_EXPRESSION:
        return "expected macro expression";
    case CM_BUILTIN_AST_DIAG_INVALID_MACRO_PATH:
        return "invalid macro path";
    case CM_BUILTIN_AST_DIAG_UNSUPPORTED_MACRO:
        return "unsupported macro";
    case CM_BUILTIN_AST_DIAG_BUILTIN_EXPANSION:
        return "builtin expansion";
    case CM_BUILTIN_AST_DIAG_GENERATED_EXPRESSION:
        return "generated expression";
    }
    return "unknown";
}
