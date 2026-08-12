#include "cm/macro/syntax_adapter.h"
#include "cm/syntax/parser.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "syntax-adapter/%s: %s\n", test, message);
    failures += 1;
}

static int parse_ast(CmAst *ast, const char *source)
{
    CmParseResult result;

    cm_ast_init(ast);
    result = cm_parse_crate(ast, source, strlen(source), CM_EDITION_2024);
    if (result.error_count != 0u) {
        fprintf(stderr, "syntax-adapter/fixture: %lu:%lu: %s\n",
            (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message);
        cm_ast_destroy(ast);
        failures += 1;
        return 0;
    }
    return 1;
}

static CmAstItemId root_item(const CmAst *ast, size_t index)
{
    const CmAstItemId *item;

    item = (const CmAstItemId *)cm_vec_at_const(&ast->root_items, index);
    return item == NULL ? CM_AST_ITEM_NONE : *item;
}

static void test_simple_item(void)
{
    static const char source[] =
        "macro_rules! make_const {"
        "  () => { zero };"
        "  ($name:ident = $value:literal) => {"
        "    const $name: i32 = $value;"
        "  };"
        "}"
        "make_const!(ANSWER = 42);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_OK ||
        result.stage != CM_MACRO_SYNTAX_STAGE_COMPLETE ||
        result.kind != CM_MACRO_SYNTAX_DIAG_NONE ||
        result.diagnostic.code != CM_MACRO_DIAG_NONE) {
        fail("simple", "valid AST pair did not expand");
    } else if (strcmp(cm_str_buf_c_str(&output),
        "const ANSWER : i32 = 42 ;") != 0) {
        fprintf(stderr, "syntax-adapter/simple: unexpected output [%s]\n",
            cm_str_buf_c_str(&output));
        failures += 1;
    } else if (result.arm_index != 1u || result.capture_count != 2u ||
        result.backtrack_steps == 0u) {
        fail("simple", "matcher telemetry was not forwarded");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_explicit_pair_and_delimiters(void)
{
    static const char definition_source[] =
        "macro_rules! bracket {"
        "  [$name:ident] => { fn $name() {} };"
        "}";
    static const char invocation_source[] = "different_name![made];";
    CmAst definition_ast;
    CmAst invocation_ast;
    const CmAstItem *invocation_item;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&definition_ast, definition_source)) return;
    if (!parse_ast(&invocation_ast, invocation_source)) {
        cm_ast_destroy(&definition_ast);
        return;
    }
    invocation_item = cm_ast_get_item(&invocation_ast,
        root_item(&invocation_ast, 0u));
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand(&definition_ast,
        root_item(&definition_ast, 0u), &invocation_ast,
        invocation_item == NULL ? NULL : &invocation_item->data.macro_item,
        NULL, &output);
    if (result.status != CM_MACRO_OK ||
        strcmp(cm_str_buf_c_str(&output), "fn made () {}") != 0) {
        fail("delimiters", "recorded bracket delimiters were not preserved");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&invocation_ast);
    cm_ast_destroy(&definition_ast);
}

static void test_repetition(void)
{
    static const char source[] =
        "macro_rules! list {"
        "  ($( $x:ident ),*) => { [$( stringify!($x) ),*] };"
        "}"
        "list!(a, b, c);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_OK || result.capture_count != 3u ||
        result.emitted_repetitions != 3u ||
        strcmp(cm_str_buf_c_str(&output),
            "[stringify ! (a) , stringify ! (b) , stringify ! (c)]") != 0) {
        fail("repetition", "captures or transcription result differ");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_explicit_defining_crate(void)
{
    static const char source[] =
        "macro_rules! exported {"
        "  () => { $crate::module::Value };"
        "}"
        "exported!();";
    CmMacroSyntaxOptions options;
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_macro_syntax_options_init(&options);
    options.crate_identifier = "rust_core";
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), &options, &output);
    if (result.status != CM_MACRO_OK
        || strcmp(cm_str_buf_c_str(&output),
            "rust_core :: module :: Value") != 0) {
        fail("defining-crate", "$crate lost its explicit defining crate");
    }
    options.crate_identifier = "core::guessed";
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), &options, &output);
    if (result.status != CM_MACRO_INVALID_ARGUMENT || output.len != 0u) {
        fail("defining-crate-invalid",
            "invalid defining-crate identifier was accepted");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_rules_style_declarative_macro(void)
{
    static const char source[] =
        "macro modern {"
        "  ($name:ident) => { struct $name; },"
        "}"
        "modern!(Made);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_OK
        || strcmp(cm_str_buf_c_str(&output), "struct Made ;") != 0) {
        fail("rules-style-declarative",
            "rule-bodied declarative macro did not use the rules adapter");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_parameterized_declarative_macro(void)
{
    static const char source[] =
        "macro modern($name:ident $(<$T:ident>)?) {"
        "  struct $name $(<$T>)?;"
        "}"
        "modern!(Generic<T>);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_OK
        || strcmp(cm_str_buf_c_str(&output),
            "struct Generic < T > ;") != 0) {
        fail("parameterized-declarative",
            "single-arm declarative macro did not use its parameter matcher");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_trailing_line_comments(void)
{
    static const char source[] =
        "macro_rules! trailing {\n"
        "  ($name:ident) => { struct $name; };\n"
        "  // definition tail\n"
        "}\n"
        "trailing! { Thing // invocation tail\n"
        "}";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_OK ||
        strcmp(cm_str_buf_c_str(&output), "struct Thing ;") != 0) {
        fail("trailing-comments",
            "synthetic delimiters were captured by trailing line comments");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_no_match(void)
{
    static const char source[] =
        "macro_rules! only_ident { ($x:ident) => { $x }; }"
        "only_ident!(123);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "stale");
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_NO_MATCH ||
        result.stage != CM_MACRO_SYNTAX_STAGE_RULES_MATCH ||
        result.kind != CM_MACRO_SYNTAX_DIAG_RULES ||
        result.diagnostic.code != CM_MACRO_DIAG_RULES_NO_MATCH ||
        output.len != 0u) {
        fail("no-match", "structured no-match result or output differs");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_rules_parse_error(void)
{
    static const char source[] =
        "macro_rules! broken { ($x:unknown) => { $x }; }"
        "broken!(x);";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_SYNTAX_ERROR ||
        result.stage != CM_MACRO_SYNTAX_STAGE_RULES_PARSE ||
        result.kind != CM_MACRO_SYNTAX_DIAG_RULES ||
        result.diagnostic.code != CM_MACRO_DIAG_RULES_UNKNOWN_FRAGMENT ||
        result.diagnostic.message == NULL || output.len != 0u) {
        fail("parse-error", "rules parser diagnostic was not forwarded");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_limits(void)
{
    static const char source[] =
        "macro_rules! nested { ((($x:ident))) => { $x }; }"
        "nested!(((x)));";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxOptions options;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_macro_syntax_options_init(&options);
    options.limits.max_nesting = 1u;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), &options, &output);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED ||
        result.stage != CM_MACRO_SYNTAX_STAGE_RULES_PARSE ||
        result.diagnostic.code != CM_MACRO_DIAG_RULES_NESTING_LIMIT) {
        fail("limits", "adapter did not forward explicit rule limits");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_validation(void)
{
    static const char source[] =
        "macro_rules! valid { () => {}; }"
        "valid!();";
    CmAst ast;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    cm_str_buf_init(&output);
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 1u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_INVALID_ARGUMENT ||
        result.kind != CM_MACRO_SYNTAX_DIAG_EXPECTED_NAMED_RULES ||
        result.stage != CM_MACRO_SYNTAX_STAGE_VALIDATE) {
        fail("definition-validation", "ordinary invocation was accepted as rules");
    }
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 0u), NULL, &output);
    if (result.status != CM_MACRO_INVALID_ARGUMENT ||
        result.kind != CM_MACRO_SYNTAX_DIAG_EXPECTED_INVOCATION ||
        output.len != 0u) {
        fail("invocation-validation", "macro_rules item was accepted as invocation");
    }
    if (strcmp(cm_macro_syntax_stage_name(CM_MACRO_SYNTAX_STAGE_COMPLETE),
        "complete") != 0 || strcmp(cm_macro_syntax_diagnostic_kind_name(
        CM_MACRO_SYNTAX_DIAG_RULES), "macro_rules diagnostic") != 0) {
        fail("names", "adapter diagnostic names are unstable");
    }
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

static void test_tree_diagnostics(void)
{
    static const char source[] =
        "macro_rules! valid { () => {}; }"
        "valid!();";
    CmAst ast;
    CmAstItem *definition;
    CmAstItem *invocation;
    CmInternId definition_arguments;
    CmInternId invocation_arguments;
    CmStrBuf output;
    CmMacroSyntaxResult result;

    if (!parse_ast(&ast, source)) return;
    definition = (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)root_item(&ast, 0u) - 1u);
    invocation = (CmAstItem *)cm_vec_at(&ast.items,
        (size_t)root_item(&ast, 1u) - 1u);
    if (definition == NULL || invocation == NULL) {
        fail("tree-fixture", "could not access parsed macro items");
        cm_ast_destroy(&ast);
        return;
    }
    definition_arguments = definition->data.macro_item.arguments;
    invocation_arguments = invocation->data.macro_item.arguments;
    cm_str_buf_init(&output);
    invocation->data.macro_item.arguments = cm_interner_intern_c_str(
        &ast.strings, "]");
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_SYNTAX_ERROR ||
        result.stage != CM_MACRO_SYNTAX_STAGE_INVOCATION_TREE ||
        result.kind != CM_MACRO_SYNTAX_DIAG_INVALID_INVOCATION_TREE ||
        result.delimiter_error_count == 0u || output.len != 0u) {
        fail("invocation-tree", "invalid invocation tree was not diagnosed");
    }
    invocation->data.macro_item.arguments = invocation_arguments;
    definition->data.macro_item.arguments = cm_interner_intern_c_str(
        &ast.strings, ")");
    result = cm_macro_syntax_expand_item(&ast, root_item(&ast, 0u),
        &ast, root_item(&ast, 1u), NULL, &output);
    if (result.status != CM_MACRO_SYNTAX_ERROR ||
        result.stage != CM_MACRO_SYNTAX_STAGE_DEFINITION_TREE ||
        result.kind != CM_MACRO_SYNTAX_DIAG_INVALID_DEFINITION_TREE ||
        result.delimiter_error_count == 0u || output.len != 0u) {
        fail("definition-tree", "invalid definition tree was not diagnosed");
    }
    definition->data.macro_item.arguments = definition_arguments;
    cm_str_buf_destroy(&output);
    cm_ast_destroy(&ast);
}

int main(void)
{
    test_simple_item();
    test_explicit_pair_and_delimiters();
    test_repetition();
    test_explicit_defining_crate();
    test_rules_style_declarative_macro();
    test_parameterized_declarative_macro();
    test_trailing_line_comments();
    test_no_match();
    test_rules_parse_error();
    test_limits();
    test_validation();
    test_tree_diagnostics();
    if (failures == 0) puts("macro syntax adapter tests: ok");
    return failures == 0 ? 0 : 1;
}
