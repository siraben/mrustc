#include "cm/macro/rules_reparse.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "rules-reparse/%s: %s\n", test, message);
    failures += 1;
}

static int parse_crate(CmAst *ast, const char *source)
{
    CmParseResult result;

    cm_ast_init(ast);
    result = cm_parse_crate(ast, source, strlen(source), CM_EDITION_2024);
    if (result.error_count != 0u) {
        fprintf(stderr, "rules-reparse/fixture: %lu:%lu: %s\n",
            (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message);
        failures += 1;
        cm_ast_destroy(ast);
        return 0;
    }
    return 1;
}

static CmAstItemId root_item(const CmAst *ast, size_t index)
{
    const CmAstItemId *id;

    id = (const CmAstItemId *)cm_vec_at_const(&ast->root_items, index);
    return id == NULL ? CM_AST_ITEM_NONE : *id;
}

static int name_is(const CmAst *ast, CmInternId id, const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static CmAstExprId parse_invocation_expression(CmAst *ast,
    const char *source)
{
    CmExpressionFragment fragment;

    fragment = cm_parse_expression_fragment(ast, source, strlen(source),
        CM_EDITION_2024);
    if (fragment.parse.error_count != 0u) {
        fail("invocation-expression", "could not parse invocation fixture");
        return CM_AST_EXPR_NONE;
    }
    return fragment.expression;
}

static void test_expression_generation_and_delimiters(void)
{
    static const char definitions_source[] =
        "macro_rules! paren_expr {"
        "  ($x:literal) => { make($x) };"
        "}"
        "macro_rules! bracket_expr {"
        "  [$x:literal] => { [$x] };"
        "}"
        "macro_rules! brace_expr {"
        "  {$x:literal} => { {$x} };"
        "}";
    CmAst definitions;
    CmAst invocations;
    CmAst destination;
    CmAstExprId paren;
    CmAstExprId bracket;
    CmAstExprId brace;
    CmMacroReparseResult result;
    CmAstExprId first_output;
    const CmAstExpr *expression;
    size_t roots_before;

    if (!parse_crate(&definitions, definitions_source)) {
        return;
    }
    cm_ast_init(&invocations);
    cm_ast_init(&destination);
    paren = parse_invocation_expression(&invocations, "paren_expr!(7)");
    bracket = parse_invocation_expression(&invocations,
        "bracket_expr![8]");
    brace = parse_invocation_expression(&invocations, "brace_expr!{9}");
    roots_before = destination.root_items.len;

    result = cm_macro_rules_reparse_expression(&definitions,
        root_item(&definitions, 0u), &invocations, paren,
        &destination, NULL);
    expression = cm_ast_get_expr(&destination, result.expression);
    if (result.status != CM_MACRO_OK
        || result.stage != CM_MACRO_REPARSE_STAGE_COMPLETE
        || result.kind != CM_MACRO_REPARSE_DIAG_NONE
        || !result.expansion_attempted || !result.reparse_attempted
        || result.expansion.status != CM_MACRO_OK
        || result.reparse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_CALL
        || expression->data.call.argument_count != 1u
        || expression->span.start != 0u
        || (size_t)expression->span.end != result.generated_length) {
        fail("paren-expression", "paren invocation did not yield exact call AST");
    }
    first_output = result.expression;

    result = cm_macro_rules_reparse_expression(&definitions,
        root_item(&definitions, 1u), &invocations, bracket,
        &destination, NULL);
    expression = cm_ast_get_expr(&destination, result.expression);
    if (result.status != CM_MACRO_OK || expression == NULL
        || expression->kind != CM_AST_EXPR_ARRAY
        || expression->data.list.element_count != 1u) {
        fail("bracket-expression", "bracket matcher did not yield array AST");
    }

    result = cm_macro_rules_reparse_expression(&definitions,
        root_item(&definitions, 2u), &invocations, brace,
        &destination, NULL);
    expression = cm_ast_get_expr(&destination, result.expression);
    if (result.status != CM_MACRO_OK || expression == NULL
        || expression->kind != CM_AST_EXPR_BLOCK
        || expression->data.block.tail == CM_AST_EXPR_NONE) {
        fail("brace-expression", "brace matcher did not yield block AST");
    }
    if (cm_ast_get_expr(&destination, first_output) == NULL) {
        fail("expression-ownership", "later reparses invalidated AST-owned ID");
    }
    if (destination.root_items.len != roots_before) {
        fail("expression-root-isolation", "expression entered crate roots");
    }
    cm_ast_destroy(&destination);
    cm_ast_destroy(&invocations);
    cm_ast_destroy(&definitions);
}

static void test_item_generation_and_ownership(void)
{
    static const char definitions_source[] =
        "macro_rules! make_item {"
        "  ($name:ident) => { fn $name() -> u32 { 7 } };"
        "}"
        "macro_rules! make_pair {"
        "  [] => { struct First; struct Second; };"
        "}";
    static const char invocations_source[] =
        "make_item!(created);"
        "make_pair![];";
    CmAst definitions;
    CmAst invocations;
    CmAst destination;
    CmMacroReparseResult item_result;
    CmMacroReparseResult pair_result;
    CmParseResult destination_parse;
    const CmAstItem *item;
    CmAstItemId created;
    size_t roots_before;

    if (!parse_crate(&definitions, definitions_source)) {
        return;
    }
    if (!parse_crate(&invocations, invocations_source)) {
        cm_ast_destroy(&definitions);
        return;
    }
    cm_ast_init(&destination);
    destination_parse = cm_parse_crate(&destination, "struct Existing;",
        strlen("struct Existing;"), CM_EDITION_2024);
    if (destination_parse.error_count != 0u) {
        fail("destination", "could not seed destination AST");
    }
    roots_before = destination.root_items.len;

    item_result = cm_macro_rules_reparse_items(&definitions,
        root_item(&definitions, 0u), &invocations,
        root_item(&invocations, 0u), &destination, NULL);
    if (item_result.status != CM_MACRO_OK || item_result.item_count != 1u
        || item_result.items == NULL || !item_result.expansion_attempted
        || !item_result.reparse_attempted) {
        fail("single-item", "item invocation did not yield one stable ID");
        created = CM_AST_ITEM_NONE;
    } else {
        created = item_result.items[0];
        item = cm_ast_get_item(&destination, created);
        if (item == NULL || item->kind != CM_AST_ITEM_FUNCTION
            || !name_is(&destination, item->name, "created")
            || item->span.start != 0u
            || (size_t)item->span.end != item_result.generated_length) {
            fail("single-item-shape", "generated function shape or span differs");
        }
    }

    pair_result = cm_macro_rules_reparse_items(&definitions,
        root_item(&definitions, 1u), &invocations,
        root_item(&invocations, 1u), &destination, NULL);
    if (pair_result.status != CM_MACRO_OK || pair_result.item_count != 2u
        || pair_result.items == NULL) {
        fail("multi-item", "multi-item transcription did not reparse");
    } else {
        item = cm_ast_get_item(&destination, pair_result.items[0]);
        if (item == NULL || item->kind != CM_AST_ITEM_STRUCT
            || !name_is(&destination, item->name, "First")) {
            fail("multi-first", "first generated item lost order");
        }
        item = cm_ast_get_item(&destination, pair_result.items[1]);
        if (item == NULL || item->kind != CM_AST_ITEM_STRUCT
            || !name_is(&destination, item->name, "Second")) {
            fail("multi-second", "second generated item lost order");
        }
    }
    if (created != CM_AST_ITEM_NONE
        && cm_ast_get_item(&destination, created) == NULL) {
        fail("item-ownership", "later item reparse invalidated earlier ID");
    }
    if (destination.root_items.len != roots_before
        || root_item(&destination, 0u) == created) {
        fail("item-root-isolation", "generated items entered destination roots");
    }
    cm_ast_destroy(&destination);
    cm_ast_destroy(&invocations);
    cm_ast_destroy(&definitions);
}

static void test_expansion_and_parse_diagnostics(void)
{
    static const char definitions_source[] =
        "macro_rules! only_ident { ($x:ident) => { $x }; }"
        "macro_rules! bad_expr { () => { 1 + }; }"
        "macro_rules! bad_item { () => { fn }; }";
    static const char item_invocation_source[] = "bad_item!();";
    CmAst definitions;
    CmAst expression_invocations;
    CmAst item_invocations;
    CmAst destination;
    CmAstExprId no_match;
    CmAstExprId bad_expr;
    CmMacroReparseResult result;

    if (!parse_crate(&definitions, definitions_source)) {
        return;
    }
    cm_ast_init(&expression_invocations);
    no_match = parse_invocation_expression(&expression_invocations,
        "only_ident!(123)");
    bad_expr = parse_invocation_expression(&expression_invocations,
        "bad_expr!()");
    if (!parse_crate(&item_invocations, item_invocation_source)) {
        cm_ast_destroy(&expression_invocations);
        cm_ast_destroy(&definitions);
        return;
    }
    cm_ast_init(&destination);

    result = cm_macro_rules_reparse_expression(&definitions,
        root_item(&definitions, 0u), &expression_invocations, no_match,
        &destination, NULL);
    if (result.status != CM_MACRO_NO_MATCH
        || result.stage != CM_MACRO_REPARSE_STAGE_EXPAND
        || result.kind != CM_MACRO_REPARSE_DIAG_EXPANSION
        || !result.expansion_attempted || result.reparse_attempted
        || result.expansion.stage != CM_MACRO_SYNTAX_STAGE_RULES_MATCH
        || result.expansion.diagnostic.code
            != CM_MACRO_DIAG_RULES_NO_MATCH
        || result.expression != CM_AST_EXPR_NONE || result.items != NULL
        || result.item_count != 0u) {
        fail("no-match", "expansion-stage no-match was not preserved");
    }

    result = cm_macro_rules_reparse_expression(&definitions,
        root_item(&definitions, 1u), &expression_invocations, bad_expr,
        &destination, NULL);
    if (result.status != CM_MACRO_SYNTAX_ERROR
        || result.stage != CM_MACRO_REPARSE_STAGE_PARSE
        || result.kind != CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX
        || result.expansion.status != CM_MACRO_OK
        || result.expansion.stage != CM_MACRO_SYNTAX_STAGE_COMPLETE
        || !result.reparse_attempted || result.reparse.error_count == 0u
        || result.reparse.first_error.message[0] == '\0'
        || result.expression != CM_AST_EXPR_NONE || result.items != NULL) {
        fail("bad-expression", "generated-expression diagnostic stages collapsed");
    }

    result = cm_macro_rules_reparse_items(&definitions,
        root_item(&definitions, 2u), &item_invocations,
        root_item(&item_invocations, 0u), &destination, NULL);
    if (result.status != CM_MACRO_SYNTAX_ERROR
        || result.stage != CM_MACRO_REPARSE_STAGE_PARSE
        || result.expansion.status != CM_MACRO_OK
        || result.reparse.error_count == 0u || result.items != NULL
        || result.item_count != 0u
        || result.expression != CM_AST_EXPR_NONE) {
        fail("bad-item", "malformed generated item leaked public IDs");
    }
    cm_ast_destroy(&destination);
    cm_ast_destroy(&item_invocations);
    cm_ast_destroy(&expression_invocations);
    cm_ast_destroy(&definitions);
}

static void test_roles_and_limits(void)
{
    static const char definition_source[] =
        "macro_rules! make { ($x:literal) => { ($x) }; }"
        "macro_rules! two { () => { struct A; struct B; }; }";
    static const char invocation_source[] =
        "make!(1); struct Ordinary; two!();";
    CmAst definition;
    CmAst invocation;
    CmAst destination;
    CmAstExprId literal;
    CmMacroReparseOptions options;
    CmMacroReparseResult result;

    if (!parse_crate(&definition, definition_source)) {
        return;
    }
    if (!parse_crate(&invocation, invocation_source)) {
        cm_ast_destroy(&definition);
        return;
    }
    cm_ast_init(&destination);
    literal = parse_invocation_expression(&invocation, "1");

    result = cm_macro_rules_reparse_expression(&definition,
        root_item(&definition, 0u), &invocation, literal,
        &destination, NULL);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.kind
            != CM_MACRO_REPARSE_DIAG_EXPECTED_EXPRESSION_INVOCATION
        || result.expansion_attempted || result.reparse_attempted
        || result.expression != CM_AST_EXPR_NONE) {
        fail("expression-role", "non-macro expression role was accepted");
    }
    result = cm_macro_rules_reparse_items(&definition,
        root_item(&definition, 0u), &invocation,
        root_item(&invocation, 1u), &destination, NULL);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.kind != CM_MACRO_REPARSE_DIAG_EXPECTED_ITEM_INVOCATION
        || result.expansion_attempted || result.items != NULL) {
        fail("item-role", "ordinary item role was accepted");
    }

    cm_macro_reparse_options_init(&options);
    options.maximum_output_bytes = 1u;
    result = cm_macro_rules_reparse_items(&definition,
        root_item(&definition, 0u), &invocation,
        root_item(&invocation, 0u), &destination, &options);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.stage != CM_MACRO_REPARSE_STAGE_OUTPUT_LIMIT
        || !result.expansion_attempted || result.reparse_attempted
        || result.items != NULL || result.item_count != 0u) {
        fail("output-limit", "transcription limit did not stop before parse");
    }
    cm_macro_reparse_options_init(&options);
    options.maximum_items = 1u;
    result = cm_macro_rules_reparse_items(&definition,
        root_item(&definition, 1u), &invocation,
        root_item(&invocation, 2u), &destination, &options);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.stage != CM_MACRO_REPARSE_STAGE_ITEM_LIMIT
        || !result.expansion_attempted || !result.reparse_attempted
        || result.reparse.error_count != 0u || result.items != NULL
        || result.item_count != 0u) {
        fail("item-limit", "generated item limit leaked parsed IDs");
    }
    if (strcmp(cm_macro_reparse_stage_name(
        CM_MACRO_REPARSE_STAGE_COMPLETE), "complete") != 0
        || strcmp(cm_macro_reparse_diagnostic_kind_name(
            CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX),
            "generated syntax") != 0) {
        fail("names", "reparse diagnostic names are unstable");
    }
    cm_ast_destroy(&destination);
    cm_ast_destroy(&invocation);
    cm_ast_destroy(&definition);
}

int main(void)
{
    test_expression_generation_and_delimiters();
    test_item_generation_and_ownership();
    test_expansion_and_parse_diagnostics();
    test_roles_and_limits();
    if (failures != 0) {
        fprintf(stderr, "macro rules reparse tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("macro rules reparse tests: ok");
    return 0;
}
