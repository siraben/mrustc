#include "cm/macro/ast_builtin.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "ast-builtin/%s: %s\n", test, message);
    failures += 1;
}

static int string_is(const CmAst *ast, CmInternId id, const char *expected)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return string != NULL && string->len == length &&
        memcmp(string->bytes, expected, length) == 0;
}

static CmAstExprId parse_expression(CmAst *ast, const char *source)
{
    CmExpressionFragment fragment;

    fragment = cm_parse_expression_fragment(ast, source, strlen(source),
        CM_EDITION_2024);
    if (fragment.parse.error_count != 0u) {
        fprintf(stderr, "ast-builtin/fixture: %lu:%lu: %s\n",
            (unsigned long)fragment.parse.first_error.line,
            (unsigned long)fragment.parse.first_error.column,
            fragment.parse.first_error.message);
        failures += 1;
        return CM_AST_EXPR_NONE;
    }
    return fragment.expression;
}

static const char *lookup_env(void *opaque, const char *name,
    size_t name_length, size_t *value_length)
{
    const char *expected;

    expected = (const char *)opaque;
    if (expected != NULL && strlen(expected) == name_length &&
        memcmp(expected, name, name_length) == 0) {
        *value_length = strlen("present-value");
        return "present-value";
    }
    return NULL;
}

static void init_context(CmBuiltinContext *context,
    CmCfgEnvironment *cfg)
{
    cm_builtin_context_init(context);
    context->file = "src/lib.rs";
    context->file_length = strlen(context->file);
    context->line = 7u;
    context->column = 3u;
    context->module_path = "crate::nested";
    context->module_path_length = strlen(context->module_path);
    cm_cfg_environment_init(cfg);
    cfg->target_family = "unix";
    cfg->target_os = "linux";
    context->cfg = cfg;
    context->env_lookup = lookup_env;
    context->env_context = (void *)"PRESENT";
}

static void expect_literal(const char *test, const char *invocation_source,
    CmBuiltinMacroKind expected_builtin, const char *expected_literal)
{
    CmAst ast;
    CmAstExprId invocation;
    CmBuiltinContext context;
    CmCfgEnvironment cfg;
    CmBuiltinAstResult result;
    const CmAstExpr *expanded;

    cm_ast_init(&ast);
    invocation = parse_expression(&ast, invocation_source);
    init_context(&context, &cfg);
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    expanded = cm_ast_get_expr(&ast, result.expanded_expression);
    if (result.status != CM_MACRO_OK ||
        result.stage != CM_BUILTIN_AST_STAGE_COMPLETE ||
        result.kind != CM_BUILTIN_AST_DIAG_NONE ||
        result.builtin != expected_builtin ||
        result.expanded_expression == CM_AST_EXPR_NONE ||
        result.expanded_expression == invocation || expanded == NULL ||
        expanded->kind != CM_AST_EXPR_LITERAL ||
        !string_is(&ast, expanded->data.literal.text, expected_literal)) {
        fail(test, "expansion did not append the expected literal expression");
    } else if (result.invocation_expression != invocation ||
        result.invocation_path == CM_AST_PATH_NONE ||
        result.invocation_span.end <= result.invocation_span.start ||
        result.parse.error_count != 0u ||
        result.generated_source_length != strlen(expected_literal)) {
        fail(test, "success metadata or invocation context differs");
    }
    cm_ast_destroy(&ast);
}

static int path_final_is(const CmAst *ast, CmAstPathId path_id,
    const char *expected)
{
    const CmAstPath *path;

    path = cm_ast_get_path(ast, path_id);
    return path != NULL && path->segment_count != 0u &&
        string_is(ast, path->segments[path->segment_count - 1u].name,
            expected);
}

static void test_required_builtins(void)
{
    expect_literal("cfg", "cfg!(all(unix, target_os = \"linux\"))",
        CM_BUILTIN_MACRO_CFG, "true");
    expect_literal("module-path", "module_path!()",
        CM_BUILTIN_MACRO_MODULE_PATH, "\"crate::nested\"");
    expect_literal("concat", "core::concat!(\"ab\", '-', 42u8, true)",
        CM_BUILTIN_MACRO_CONCAT, "\"ab-42u8true\"");
    expect_literal("stringify", "stringify!(value + call(1))",
        CM_BUILTIN_MACRO_STRINGIFY, "\"value + call(1)\"");
    expect_literal("env", "env!(\"PRESENT\")",
        CM_BUILTIN_MACRO_ENV, "\"present-value\"");
}

static void test_option_env_shapes(void)
{
    CmAst ast;
    CmBuiltinContext context;
    CmCfgEnvironment cfg;
    CmAstExprId invocation;
    CmBuiltinAstResult result;
    const CmAstExpr *expanded;
    const CmAstExpr *callee;

    cm_ast_init(&ast);
    init_context(&context, &cfg);
    invocation = parse_expression(&ast, "option_env!(\"PRESENT\")");
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    expanded = cm_ast_get_expr(&ast, result.expanded_expression);
    callee = expanded != NULL && expanded->kind == CM_AST_EXPR_CALL ?
        cm_ast_get_expr(&ast, expanded->data.call.callee) : NULL;
    if (result.status != CM_MACRO_OK || expanded == NULL ||
        expanded->kind != CM_AST_EXPR_CALL ||
        expanded->data.call.argument_count != 1u || callee == NULL ||
        callee->kind != CM_AST_EXPR_PATH ||
        !path_final_is(&ast, callee->data.path.path, "Some")) {
        fail("option-some", "Some expansion was not parsed structurally");
    }
    invocation = parse_expression(&ast, "option_env!(\"ABSENT\")");
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    expanded = cm_ast_get_expr(&ast, result.expanded_expression);
    if (result.status != CM_MACRO_OK || expanded == NULL ||
        expanded->kind != CM_AST_EXPR_PATH ||
        !path_final_is(&ast, expanded->data.path.path, "None")) {
        fail("option-none", "None expansion was not parsed structurally");
    }
    cm_ast_destroy(&ast);
}

static void test_validation_failures(void)
{
    CmAst ast;
    CmBuiltinContext context;
    CmCfgEnvironment cfg;
    CmAstExprId literal;
    CmAstExprId unknown;
    CmBuiltinAstResult result;
    const CmAstExpr *unknown_node;

    cm_ast_init(&ast);
    init_context(&context, &cfg);
    literal = parse_expression(&ast, "123");
    result = cm_builtin_ast_expand_expression(&ast, literal, &context,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_INVALID_ARGUMENT ||
        result.stage != CM_BUILTIN_AST_STAGE_VALIDATE ||
        result.kind != CM_BUILTIN_AST_DIAG_EXPECTED_MACRO_EXPRESSION ||
        result.expanded_expression != CM_AST_EXPR_NONE) {
        fail("non-macro", "ordinary expression was accepted as a macro");
    }
    unknown = parse_expression(&ast, "some::unknown_builtin!(x)");
    unknown_node = cm_ast_get_expr(&ast, unknown);
    result = cm_builtin_ast_expand_expression(&ast, unknown, &context,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_UNSUPPORTED ||
        result.stage != CM_BUILTIN_AST_STAGE_CLASSIFY ||
        result.kind != CM_BUILTIN_AST_DIAG_UNSUPPORTED_MACRO ||
        result.macro_diagnostic.code != CM_MACRO_DIAG_BUILTIN_UNKNOWN ||
        result.expanded_expression != CM_AST_EXPR_NONE ||
        unknown_node == NULL ||
        result.invocation_span.start != unknown_node->span.start ||
        result.invocation_span.end != unknown_node->span.end) {
        fail("unsupported", "unsupported macro did not retain context");
    }
    result = cm_builtin_ast_expand_expression(&ast, unknown, NULL,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_INVALID_ARGUMENT ||
        result.kind != CM_BUILTIN_AST_DIAG_INVALID_ARGUMENT ||
        result.invocation_path == CM_AST_PATH_NONE ||
        result.invocation_span.end <= result.invocation_span.start ||
        result.expanded_expression != CM_AST_EXPR_NONE) {
        fail("context", "missing explicit context was accepted");
    }
    result = cm_builtin_ast_expand_expression(&ast, CM_AST_EXPR_NONE,
        &context, CM_EDITION_2024);
    if (result.status != CM_MACRO_INVALID_ARGUMENT ||
        result.expanded_expression != CM_AST_EXPR_NONE) {
        fail("invalid-id", "invalid invocation ID was accepted");
    }
    cm_ast_destroy(&ast);
}

static void test_builtin_failures(void)
{
    CmAst ast;
    CmBuiltinContext context;
    CmCfgEnvironment cfg;
    CmAstExprId invocation;
    CmBuiltinAstResult result;

    cm_ast_init(&ast);
    init_context(&context, &cfg);
    invocation = parse_expression(&ast, "env!(\"ABSENT\")");
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_ENV_NOT_FOUND ||
        result.stage != CM_BUILTIN_AST_STAGE_EXPAND ||
        result.kind != CM_BUILTIN_AST_DIAG_BUILTIN_EXPANSION ||
        result.macro_diagnostic.code != CM_MACRO_DIAG_BUILTIN_ENV_MISSING ||
        result.expanded_expression != CM_AST_EXPR_NONE) {
        fail("env-missing", "builtin error was not forwarded");
    }
    invocation = parse_expression(&ast, "concat!(not_a_literal)");
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_SYNTAX_ERROR ||
        result.stage != CM_BUILTIN_AST_STAGE_EXPAND ||
        result.macro_diagnostic.code !=
            CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL ||
        result.expanded_expression != CM_AST_EXPR_NONE ||
        result.generated_source_length != 0u) {
        fail("malformed-arguments", "malformed builtin input was accepted");
    }
    invocation = parse_expression(&ast, "cfg!(not(unix, windows))");
    result = cm_builtin_ast_expand_expression(&ast, invocation, &context,
        CM_EDITION_2024);
    if (result.status != CM_MACRO_SYNTAX_ERROR ||
        result.stage != CM_BUILTIN_AST_STAGE_EXPAND ||
        result.macro_diagnostic.code != CM_MACRO_DIAG_CFG_NOT_ARITY ||
        result.expanded_expression != CM_AST_EXPR_NONE) {
        fail("cfg-error", "cfg diagnostic was not forwarded");
    }
    cm_ast_destroy(&ast);
}

static void test_names(void)
{
    if (strcmp(cm_builtin_ast_stage_name(CM_BUILTIN_AST_STAGE_REPARSE),
        "reparse") != 0 || strcmp(cm_builtin_ast_diagnostic_kind_name(
        CM_BUILTIN_AST_DIAG_GENERATED_EXPRESSION),
        "generated expression") != 0) {
        fail("names", "stage or diagnostic names are unstable");
    }
}

int main(void)
{
    test_required_builtins();
    test_option_env_shapes();
    test_validation_failures();
    test_builtin_failures();
    test_names();
    if (failures == 0) puts("builtin AST adapter tests: ok");
    return failures == 0 ? 0 : 1;
}
