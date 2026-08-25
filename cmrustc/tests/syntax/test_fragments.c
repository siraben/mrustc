#include "cm/syntax/parser.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "fragments/%s: %s\n", test, message);
    failures += 1;
}

static int string_is(const CmAst *ast, CmInternId id, const char *expected)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return string != NULL && string->len == length
        && memcmp(string->bytes, expected, length) == 0;
}

static void expect_expression_error(CmAst *ast, const char *test,
    const char *source)
{
    CmExpressionFragment fragment;

    fragment = cm_parse_expression_fragment(ast, source, strlen(source),
        CM_EDITION_2024);
    if (fragment.parse.error_count == 0u
        || fragment.expression != CM_AST_EXPR_NONE
        || fragment.parse.first_error.message[0] == '\0') {
        fail(test, "invalid expression fragment did not fail cleanly");
    }
}

static void test_expression_fragments(void)
{
    CmAst ast;
    CmExpressionFragment literal;
    CmExpressionFragment call;
    CmExpressionFragment turbofish;
    CmExpressionFragment block;
    CmExpressionFragment macro;
    CmExpressionFragment structure;
    CmExpressionFragment nested_structure;
    CmExpressionFragment update;
    CmExpressionFragment control;
    CmExpressionFragment unsafe_block;
    CmExpressionFragment projection;
    const CmAstExpr *expression;
    const CmAstExpr *argument;
    const CmAstExpr *callee;
    const CmAstExpr *condition;
    const CmAstExpr *field_value;
    const CmAstExpr *explicit_value;
    const CmAstExpr *nested_expression;
    const CmAstExpr *base;
    const CmAstExpr *projection_base;
    const CmAstPath *callee_path;
    const CmAstPath *field_path;
    const CmAstPath *path;
    const CmAstType *substitution;
    const CmAstPath *substitution_path;
    size_t roots_before;

    cm_ast_init(&ast);
    roots_before = ast.root_items.len;
    literal = cm_parse_expression_fragment(&ast, "1234u32", 7u,
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, literal.expression);
    if (literal.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_LITERAL
        || !string_is(&ast, expression->data.literal.text, "1234u32")) {
        fail("literal", "generated literal was not parsed structurally");
    }

    call = cm_parse_expression_fragment(&ast,
        "factory(7, { let value = 2; value })",
        strlen("factory(7, { let value = 2; value })"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, call.expression);
    if (call.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_CALL
        || expression->data.call.argument_count != 2u) {
        fail("call", "generated call fragment has the wrong AST shape");
    } else {
        argument = cm_ast_get_expr(&ast,
            expression->data.call.arguments[1]);
        if (argument == NULL || argument->kind != CM_AST_EXPR_BLOCK
            || argument->data.block.statement_count != 1u
            || argument->data.block.tail == CM_AST_EXPR_NONE) {
            fail("call-block", "call argument block was not preserved");
        }
    }

    turbofish = cm_parse_expression_fragment(&ast,
        "identity::<u32>(x)", strlen("identity::<u32>(x)"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, turbofish.expression);
    callee = expression == NULL || expression->kind != CM_AST_EXPR_CALL
        ? NULL : cm_ast_get_expr(&ast, expression->data.call.callee);
    callee_path = callee == NULL || callee->kind != CM_AST_EXPR_PATH
        ? NULL : cm_ast_get_path(&ast, callee->data.path.path);
    substitution = callee_path == NULL || callee_path->segment_count != 1u
        || callee_path->segments == NULL
        || callee_path->segments[0].argument_count != 1u
        || callee_path->segments[0].arguments == NULL
        || callee_path->segments[0].arguments[0].kind
            != CM_AST_GENERIC_TYPE
        ? NULL : cm_ast_get_type(&ast,
            callee_path->segments[0].arguments[0].type);
    substitution_path = substitution == NULL
        || substitution->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, substitution->path);
    if (turbofish.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_CALL
        || expression->data.call.argument_count != 1u
        || callee_path == NULL
        || !string_is(&ast, callee_path->segments[0].name, "identity")
        || substitution_path == NULL
        || substitution_path->segment_count != 1u
        || substitution_path->segments == NULL
        || !string_is(&ast, substitution_path->segments[0].name, "u32")) {
        fail("turbofish-call",
            "expression turbofish was not retained on the callee path");
    }

    projection = cm_parse_expression_fragment(&ast, "<T>::VALUE",
        strlen("<T>::VALUE"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, projection.expression);
    substitution = expression == NULL
            || expression->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_type(&ast,
            expression->data.qualified_path.self_type);
    substitution_path = substitution == NULL
            || substitution->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(&ast, substitution->path);
    path = expression == NULL
            || expression->kind != CM_AST_EXPR_QUALIFIED_PATH
        ? NULL : cm_ast_get_path(&ast,
            expression->data.qualified_path.associated_path);
    if (projection.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_QUALIFIED_PATH
        || expression->data.qualified_path.trait_path != CM_AST_PATH_NONE
        || expression->span.start != 0u || expression->span.end != 10u
        || expression->data.qualified_path.qualifier_span.start != 0u
        || expression->data.qualified_path.qualifier_span.end != 3u
        || substitution_path == NULL
        || substitution_path->segment_count != 1u
        || !string_is(&ast, substitution_path->segments[0].name, "T")
        || path == NULL || path->segment_count != 1u
        || !string_is(&ast, path->segments[0].name, "VALUE")) {
        fail("traitless-qualified-expression-path",
            "traitless qualified expression path was not structural");
    }

    projection = cm_parse_expression_fragment(&ast, "value.inner.leaf",
        strlen("value.inner.leaf"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, projection.expression);
    projection_base = expression == NULL
            || expression->kind != CM_AST_EXPR_FIELD
        ? NULL : cm_ast_get_expr(&ast, expression->data.field.base);
    base = projection_base == NULL
            || projection_base->kind != CM_AST_EXPR_FIELD
        ? NULL : cm_ast_get_expr(&ast, projection_base->data.field.base);
    if (projection.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_FIELD
        || expression->span.start != 0u || expression->span.end != 16u
        || expression->data.field.name_span.start != 12u
        || expression->data.field.name_span.end != 16u
        || !string_is(&ast, expression->data.field.name, "leaf")
        || projection_base == NULL
        || projection_base->kind != CM_AST_EXPR_FIELD
        || projection_base->span.start != 0u
        || projection_base->span.end != 11u
        || projection_base->data.field.name_span.start != 6u
        || projection_base->data.field.name_span.end != 11u
        || !string_is(&ast, projection_base->data.field.name, "inner")
        || base == NULL || base->kind != CM_AST_EXPR_PATH
        || base->span.start != 0u || base->span.end != 5u
        || projection_base->span.end
            > expression->data.field.name_span.start) {
        fail("named-field-projection-spans",
            "field projection did not retain exact base, name, and full spans");
    }

    projection = cm_parse_expression_fragment(&ast, "value.0",
        strlen("value.0"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, projection.expression);
    base = expression == NULL
            || expression->kind != CM_AST_EXPR_TUPLE_FIELD
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.tuple_field.base);
    if (projection.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_TUPLE_FIELD
        || expression->span.start != 0u || expression->span.end != 7u
        || expression->data.tuple_field.index != 0u
        || expression->data.tuple_field.index_span.start != 6u
        || expression->data.tuple_field.index_span.end != 7u
        || base == NULL || base->kind != CM_AST_EXPR_PATH
        || base->span.start != 0u || base->span.end != 5u) {
        fail("tuple-index-projection-spans",
            "tuple projection did not retain exact base, index, and spans");
    }

    projection = cm_parse_expression_fragment(&ast, "value.0.1",
        strlen("value.0.1"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, projection.expression);
    projection_base = expression == NULL
            || expression->kind != CM_AST_EXPR_TUPLE_FIELD
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.tuple_field.base);
    base = projection_base == NULL
            || projection_base->kind != CM_AST_EXPR_TUPLE_FIELD
        ? NULL : cm_ast_get_expr(&ast,
            projection_base->data.tuple_field.base);
    if (projection.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_TUPLE_FIELD
        || expression->span.start != 0u || expression->span.end != 9u
        || expression->data.tuple_field.index != 1u
        || expression->data.tuple_field.index_span.start != 8u
        || expression->data.tuple_field.index_span.end != 9u
        || projection_base == NULL
        || projection_base->kind != CM_AST_EXPR_TUPLE_FIELD
        || projection_base->span.start != 0u
        || projection_base->span.end != 7u
        || projection_base->data.tuple_field.index != 0u
        || base == NULL || base->kind != CM_AST_EXPR_PATH) {
        fail("chained-tuple-index-projection",
            "chained tuple projections lost an ordinal or span");
    }

    projection = cm_parse_expression_fragment(&ast, "value.0.field",
        strlen("value.0.field"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, projection.expression);
    projection_base = expression == NULL
            || expression->kind != CM_AST_EXPR_FIELD
        ? NULL : cm_ast_get_expr(&ast, expression->data.field.base);
    if (projection.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_FIELD
        || expression->span.start != 0u || expression->span.end != 13u
        || expression->data.field.name_span.start != 8u
        || expression->data.field.name_span.end != 13u
        || !string_is(&ast, expression->data.field.name, "field")
        || projection_base == NULL
        || projection_base->kind != CM_AST_EXPR_TUPLE_FIELD
        || projection_base->data.tuple_field.index != 0u
        || projection_base->span.end != 7u) {
        fail("tuple-index-named-field-chain",
            "tuple-to-named-field chaining was not retained");
    }

    structure = cm_parse_expression_fragment(&ast,
        "Pair { left: 1u32, right, }",
        strlen("Pair { left: 1u32, right, }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, structure.expression);
    path = expression == NULL || expression->kind != CM_AST_EXPR_STRUCT
        ? NULL : cm_ast_get_path(&ast, expression->data.struct_expr.path);
    explicit_value = expression == NULL || expression->kind
            != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 2u
        || expression->data.struct_expr.fields == NULL
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.struct_expr.fields[0].value);
    field_value = expression == NULL || expression->kind
            != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 2u
        || expression->data.struct_expr.fields == NULL
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.struct_expr.fields[1].value);
    field_path = field_value == NULL || field_value->kind
            != CM_AST_EXPR_PATH
        ? NULL : cm_ast_get_path(&ast, field_value->data.path.path);
    if (structure.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_STRUCT || path == NULL
        || path->segment_count != 1u || path->segments == NULL
        || !string_is(&ast, path->segments[0].name, "Pair")
        || expression->data.struct_expr.field_count != 2u
        || expression->data.struct_expr.fields == NULL
        || !string_is(&ast,
            expression->data.struct_expr.fields[0].name, "left")
        || expression->data.struct_expr.fields[0].is_shorthand
        || explicit_value == NULL
        || explicit_value->kind != CM_AST_EXPR_LITERAL
        || !string_is(&ast,
            expression->data.struct_expr.fields[1].name, "right")
        || !expression->data.struct_expr.fields[1].is_shorthand
        || field_value == NULL || field_value->kind != CM_AST_EXPR_PATH
        || field_path == NULL || field_path->segment_count != 1u
        || field_path->segments == NULL
        || !string_is(&ast, field_path->segments[0].name, "right")
        || expression->data.struct_expr.fields[0].span.start != 7u
        || expression->data.struct_expr.fields[0].span.end != 17u
        || expression->data.struct_expr.fields[1].span.start != 19u
        || expression->data.struct_expr.fields[1].span.end != 24u
        || expression->data.struct_expr.fields[0].span.end
            > expression->data.struct_expr.fields[1].span.start
        || expression->data.struct_expr.base != CM_AST_EXPR_NONE) {
        fail("struct-expression",
            "named and shorthand fields or their spans were not retained");
    }

    nested_structure = cm_parse_expression_fragment(&ast,
        "Outer { first: Inner { value, }, second: 2 }",
        strlen("Outer { first: Inner { value, }, second: 2 }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, nested_structure.expression);
    nested_expression = expression == NULL
            || expression->kind != CM_AST_EXPR_STRUCT
            || expression->data.struct_expr.field_count != 2u
            || expression->data.struct_expr.fields == NULL
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.struct_expr.fields[0].value);
    if (nested_structure.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 2u
        || expression->data.struct_expr.fields == NULL
        || expression->data.struct_expr.fields[0].span.start != 8u
        || expression->data.struct_expr.fields[0].span.end != 31u
        || expression->data.struct_expr.fields[1].span.start != 33u
        || expression->data.struct_expr.fields[1].span.end != 42u
        || expression->data.struct_expr.fields[0].span.end
            > expression->data.struct_expr.fields[1].span.start
        || nested_expression == NULL
        || nested_expression->kind != CM_AST_EXPR_STRUCT
        || nested_expression->span.start != 15u
        || nested_expression->span.end != 31u
        || nested_expression->data.struct_expr.field_count != 1u
        || nested_expression->data.struct_expr.fields == NULL
        || nested_expression->data.struct_expr.fields[0].span.start != 23u
        || nested_expression->data.struct_expr.fields[0].span.end != 28u
        || nested_expression->data.struct_expr.fields[0].span.start
            < nested_expression->span.start
        || nested_expression->data.struct_expr.fields[0].span.end
            > nested_expression->span.end) {
        fail("nested-struct-field-spans",
            "nested struct fields did not retain exact ordered spans");
    }

    update = cm_parse_expression_fragment(&ast,
        "module::Pair { left, ..fallback }",
        strlen("module::Pair { left, ..fallback }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, update.expression);
    base = expression == NULL || expression->kind != CM_AST_EXPR_STRUCT
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.struct_expr.base);
    path = base == NULL || base->kind != CM_AST_EXPR_PATH
        ? NULL : cm_ast_get_path(&ast, base->data.path.path);
    if (update.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 1u
        || base == NULL || base->kind != CM_AST_EXPR_PATH
        || path == NULL || path->segment_count != 1u
        || path->segments == NULL
        || !string_is(&ast, path->segments[0].name, "fallback")) {
        fail("struct-update", "struct update base was not retained");
    }

    update = cm_parse_expression_fragment(&ast, "Pair { ..fallback, }",
        strlen("Pair { ..fallback, }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, update.expression);
    base = expression == NULL || expression->kind != CM_AST_EXPR_STRUCT
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.struct_expr.base);
    if (update.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 0u
        || expression->data.struct_expr.fields != NULL || base == NULL
        || base->kind != CM_AST_EXPR_PATH) {
        fail("struct-base-only",
            "base-only struct update with a trailing comma was lost");
    }

    structure = cm_parse_expression_fragment(&ast, "Unit {}",
        strlen("Unit {}"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, structure.expression);
    if (structure.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_STRUCT
        || expression->data.struct_expr.field_count != 0u
        || expression->data.struct_expr.fields != NULL
        || expression->data.struct_expr.base != CM_AST_EXPR_NONE) {
        fail("empty-struct", "empty struct expression was not retained");
    }

    control = cm_parse_expression_fragment(&ast,
        "if ready { left } else { right }",
        strlen("if ready { left } else { right }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind != CM_AST_EXPR_IF
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.if_expr.condition);
    if (control.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_IF || condition == NULL
        || condition->kind != CM_AST_EXPR_PATH
        || cm_ast_get_expr(&ast, expression->data.if_expr.then_expr) == NULL
        || cm_ast_get_expr(&ast, expression->data.if_expr.else_expr)
            == NULL) {
        fail("if-struct-disambiguation",
            "if body brace was consumed as a struct expression");
    }

    control = cm_parse_expression_fragment(&ast,
        "while ready { tick(); }", strlen("while ready { tick(); }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind
            != CM_AST_EXPR_WHILE
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.while_expr.condition);
    if (control.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_WHILE || condition == NULL
        || condition->kind != CM_AST_EXPR_PATH
        || cm_ast_get_expr(&ast, expression->data.while_expr.body) == NULL) {
        fail("while-struct-disambiguation",
            "while body brace was consumed as a struct expression");
    }

    control = cm_parse_expression_fragment(&ast,
        "for item in ready { visit(item); }",
        strlen("for item in ready { visit(item); }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind != CM_AST_EXPR_FOR
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.for_expr.iterable);
    if (control.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_FOR || condition == NULL
        || condition->kind != CM_AST_EXPR_PATH
        || cm_ast_get_expr(&ast, expression->data.for_expr.body) == NULL) {
        fail("for-struct-disambiguation",
            "for body brace was consumed as a struct expression");
    }

    control = cm_parse_expression_fragment(&ast,
        "match ready { _ => left }", strlen("match ready { _ => left }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind
            != CM_AST_EXPR_MATCH
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.match_expr.scrutinee);
    if (control.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_MATCH || condition == NULL
        || condition->kind != CM_AST_EXPR_PATH
        || expression->data.match_expr.arm_count != 1u) {
        fail("match-struct-disambiguation",
            "match arm brace was consumed as a struct expression");
    }

    control = cm_parse_expression_fragment(&ast,
        "if (Pair { left, right }).left { yes }",
        strlen("if (Pair { left, right }).left { yes }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind != CM_AST_EXPR_IF
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.if_expr.condition);
    base = condition == NULL || condition->kind != CM_AST_EXPR_FIELD
        ? NULL : cm_ast_get_expr(&ast, condition->data.field.base);
    if (control.parse.error_count != 0u || condition == NULL
        || condition->kind != CM_AST_EXPR_FIELD || base == NULL
        || base->kind != CM_AST_EXPR_STRUCT) {
        fail("parenthesized-struct-condition",
            "parentheses did not re-enable a struct expression");
    }

    control = cm_parse_expression_fragment(&ast,
        "if predicate(Pair { left, right }) { yes }",
        strlen("if predicate(Pair { left, right }) { yes }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, control.expression);
    condition = expression == NULL || expression->kind != CM_AST_EXPR_IF
        ? NULL : cm_ast_get_expr(&ast,
            expression->data.if_expr.condition);
    argument = condition == NULL || condition->kind != CM_AST_EXPR_CALL
        || condition->data.call.argument_count != 1u
        ? NULL : cm_ast_get_expr(&ast, condition->data.call.arguments[0]);
    if (control.parse.error_count != 0u || condition == NULL
        || condition->kind != CM_AST_EXPR_CALL || argument == NULL
        || argument->kind != CM_AST_EXPR_STRUCT) {
        fail("delimited-struct-condition",
            "call arguments did not re-enable a struct expression");
    }

    unsafe_block = cm_parse_expression_fragment(&ast,
        "unsafe { call(); }", strlen("unsafe { call(); }"),
        CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, unsafe_block.expression);
    if (unsafe_block.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_BLOCK
        || !expression->data.block.is_unsafe
        || expression->data.block.statement_count != 1u) {
        fail("unsafe-block",
            "unsafe block was consumed as a struct expression");
    }

    call = cm_parse_expression_fragment(&ast, "left < right",
        strlen("left < right"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, call.expression);
    if (call.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_BINARY
        || !string_is(&ast, expression->data.binary.operator_name, "<")) {
        fail("comparison-not-turbofish",
            "bare '<' stopped parsing as a comparison expression");
    }

    block = cm_parse_expression_fragment(&ast,
        "{ let x: u32 = 3; x + 4 }",
        strlen("{ let x: u32 = 3; x + 4 }"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, block.expression);
    if (block.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_BLOCK
        || expression->data.block.statement_count != 1u
        || expression->data.block.tail == CM_AST_EXPR_NONE) {
        fail("block", "generated block was not parsed as one expression");
    }

    macro = cm_parse_expression_fragment(&ast, "generated!(a, [b, c])",
        strlen("generated!(a, [b, c])"), CM_EDITION_2024);
    expression = cm_ast_get_expr(&ast, macro.expression);
    if (macro.parse.error_count != 0u || expression == NULL
        || expression->kind != CM_AST_EXPR_MACRO
        || expression->data.macro_expr.delimiter
            != CM_AST_DELIMITER_PAREN
        || !string_is(&ast, expression->data.macro_expr.arguments,
            "a, [b, c]")) {
        fail("macro-expression", "macro expression token tree was lost");
    }

    expression = cm_ast_get_expr(&ast, literal.expression);
    if (expression == NULL || expression->kind != CM_AST_EXPR_LITERAL) {
        fail("append-stability", "later fragments invalidated an earlier ID");
    }
    if (ast.root_items.len != roots_before) {
        fail("expression-roots", "expression parsing changed crate roots");
    }

    expect_expression_error(&ast, "empty", "");
    expect_expression_error(&ast, "whitespace", " \t\n");
    expect_expression_error(&ast, "trailing-token", "7 ignored");
    expect_expression_error(&ast, "trailing-semicolon", "7;");
    expect_expression_error(&ast, "malformed-call", "factory(");
    expect_expression_error(&ast, "struct-missing-value",
        "Pair { left: }");
    expect_expression_error(&ast, "struct-missing-comma",
        "Pair { left right }");
    expect_expression_error(&ast, "struct-base-not-last",
        "Pair { ..base, right: 1 }");
    expect_expression_error(&ast, "struct-missing-base", "Pair { .. }");
    expect_expression_error(&ast, "struct-missing-close",
        "Pair { left: 1");
    expect_expression_error(&ast, "struct-keyword-path-segment",
        "module::unsafe {}");
    expect_expression_error(&ast, "struct-absolute-keyword-path",
        "::if {}");
    expect_expression_error(&ast, "struct-keyword-field",
        "Pair { unsafe: 1 }");
    expect_expression_error(&ast, "tuple-index-leading-zero", "value.00");
    expect_expression_error(&ast, "tuple-index-suffix", "value.0u32");
    expect_expression_error(&ast, "tuple-index-underscore", "value.1_0");
    expect_expression_error(&ast, "tuple-index-base", "value.0x0");
    expect_expression_error(&ast, "tuple-index-overflow",
        "value.4294967296");
    expect_expression_error(&ast, "lexical", "\"unterminated");

    call = cm_parse_expression_fragment(&ast, "factory() /* trailing */\n",
        strlen("factory() /* trailing */\n"), CM_EDITION_2024);
    if (call.parse.error_count != 0u
        || call.expression == CM_AST_EXPR_NONE) {
        fail("trailing-comment", "trivia was treated as trailing input");
    }
    cm_ast_destroy(&ast);
}

static void expect_item_error(CmAst *ast, const char *test,
    const char *source)
{
    CmItemListFragment fragment;

    fragment = cm_parse_item_list_fragment(ast, source, strlen(source),
        CM_EDITION_2024);
    if (fragment.parse.error_count == 0u || fragment.items != NULL
        || fragment.item_count != 0u
        || fragment.parse.first_error.message[0] == '\0') {
        fail(test, "invalid item fragment did not fail cleanly");
    }
}

static void test_item_fragments(void)
{
    static const char generated[] =
        "#[inline] fn generated() -> u32 { 7 }\n"
        "struct Pair(u8, u8);\n"
        "helper! { generated tokens }\n";
    CmAst ast;
    CmItemListFragment empty;
    CmItemListFragment items;
    CmItemListFragment trailing;
    CmItemListFragment impl_items;
    CmItemListFragment wrong_context;
    CmParseResult crate_result;
    const CmAstItem *item;
    CmAstItemId first_id;

    cm_ast_init(&ast);
    empty = cm_parse_item_list_fragment(&ast, NULL, 0u,
        CM_EDITION_2024);
    if (empty.parse.error_count != 0u || empty.items != NULL
        || empty.item_count != 0u) {
        fail("empty-items", "empty item output should be a valid empty list");
    }

    items = cm_parse_item_list_fragment(&ast, generated,
        sizeof(generated) - 1u, CM_EDITION_2024);
    if (items.parse.error_count != 0u || items.items == NULL
        || items.item_count != 3u) {
        fail("items", "generated item list was not parsed completely");
    } else {
        first_id = items.items[0];
        item = cm_ast_get_item(&ast, items.items[0]);
        if (item == NULL || item->kind != CM_AST_ITEM_FUNCTION
            || !string_is(&ast, item->name, "generated")
            || item->attribute_count != 1u) {
            fail("function-item", "generated function item is incomplete");
        }
        item = cm_ast_get_item(&ast, items.items[1]);
        if (item == NULL || item->kind != CM_AST_ITEM_STRUCT
            || !string_is(&ast, item->name, "Pair")) {
            fail("struct-item", "generated struct item is incomplete");
        }
        item = cm_ast_get_item(&ast, items.items[2]);
        if (item == NULL || item->kind != CM_AST_ITEM_MACRO
            || item->data.macro_item.delimiter != CM_AST_DELIMITER_BRACE
            || !string_is(&ast, item->data.macro_item.arguments,
                "generated tokens")) {
            fail("macro-item", "generated macro item is incomplete");
        }
        trailing = cm_parse_item_list_fragment(&ast,
            "const LAST: u8 = 9; // trailing trivia\n",
            strlen("const LAST: u8 = 9; // trailing trivia\n"),
            CM_EDITION_2024);
        if (trailing.parse.error_count != 0u
            || trailing.item_count != 1u
            || cm_ast_get_item(&ast, first_id) == NULL) {
            fail("item-ownership", "AST-owned item IDs were not stable");
        }
    }
    if (ast.root_items.len != 0u) {
        fail("item-roots", "item fragments were inserted as crate roots");
    }

    impl_items = cm_parse_item_list_fragment_in_context(&ast,
        "default fn specialized() {} default type Output = u8;",
        strlen("default fn specialized() {} default type Output = u8;"),
        CM_EDITION_2024, CM_ITEM_LIST_FRAGMENT_IMPL);
    if (impl_items.parse.error_count != 0u
        || impl_items.item_count != 2u
        || !cm_ast_get_item(&ast, impl_items.items[0])->is_default
        || !cm_ast_get_item(&ast, impl_items.items[1])->is_default) {
        fail("impl-items",
            "impl-context fragment did not preserve specialization items");
    }
    expect_item_error(&ast, "root-default-item",
        "default fn specialized() {}");
    wrong_context = cm_parse_item_list_fragment_in_context(&ast,
        "fn item() {}", strlen("fn item() {}"), CM_EDITION_2024,
        (CmItemListFragmentContext)99);
    if (wrong_context.parse.error_count == 0u
        || wrong_context.items != NULL || wrong_context.item_count != 0u) {
        fail("item-context", "invalid item fragment context was accepted");
    }

    expect_item_error(&ast, "malformed-item", "struct Missing");
    expect_item_error(&ast, "expression-as-item", "1 + 2");
    expect_item_error(&ast, "partial-list",
        "struct Good; fn broken(");
    expect_item_error(&ast, "stray-trailing", "struct Good; @");

    crate_result = cm_parse_crate(&ast, "struct Root;",
        strlen("struct Root;"), CM_EDITION_2024);
    if (crate_result.error_count != 0u || ast.root_items.len != 1u) {
        fail("crate-preserved", "normal crate parsing behavior changed");
    }
    cm_ast_destroy(&ast);
}

static void test_type_fragments(void)
{
    CmAst ast;
    CmTypeFragment generic;
    CmTypeFragment macro;
    CmTypeFragment trailing;
    CmTypeFragment empty;
    CmTypeFragment bound_function;
    CmTypeFragment implicit_function;
    CmTypeFragment duplicate_binder;
    const CmAstType *bound_function_type;
    const CmAstType *implicit_function_type;
    const CmAstType *macro_type;
    const CmAstPath *macro_path;

    cm_ast_init(&ast);
    generic = cm_parse_type_fragment(&ast,
        "Wrapping<Result<u8, E>>",
        strlen("Wrapping<Result<u8, E>>"), CM_EDITION_2024);
    macro = cm_parse_type_fragment(&ast, "wrap::in_ref!(x x)",
        strlen("wrap::in_ref!(x x)"), CM_EDITION_2024);
    trailing = cm_parse_type_fragment(&ast, "u8 u16",
        strlen("u8 u16"), CM_EDITION_2024);
    empty = cm_parse_type_fragment(&ast, NULL, 0u, CM_EDITION_2024);
    bound_function = cm_parse_type_fragment(&ast,
        "for<'a, 'b> unsafe fn(&'a u8, &'b u16) -> &'a u8",
        strlen("for<'a, 'b> unsafe fn(&'a u8, &'b u16) -> &'a u8"),
        CM_EDITION_2024);
    implicit_function = cm_parse_type_fragment(&ast, "fn(&u8) -> &u8",
        strlen("fn(&u8) -> &u8"), CM_EDITION_2024);
    duplicate_binder = cm_parse_type_fragment(&ast,
        "for<'a, 'a> fn(&'a u8)",
        strlen("for<'a, 'a> fn(&'a u8)"), CM_EDITION_2024);
    macro_type = cm_ast_get_type(&ast, macro.type);
    macro_path = macro_type == NULL
            || macro_type->kind != CM_AST_TYPE_MACRO
        ? NULL : cm_ast_get_path(&ast, macro_type->macro_type.path);
    bound_function_type = cm_ast_get_type(&ast, bound_function.type);
    implicit_function_type = cm_ast_get_type(&ast, implicit_function.type);
    if (generic.parse.error_count != 0u
        || generic.type == CM_AST_TYPE_NONE
        || macro.parse.error_count != 0u || macro_type == NULL
        || macro_type->kind != CM_AST_TYPE_MACRO
        || macro_type->macro_type.form != CM_AST_MACRO_INVOCATION
        || macro_type->macro_type.delimiter != CM_AST_DELIMITER_PAREN
        || macro_type->macro_type.has_semicolon
        || !string_is(&ast, macro_type->macro_type.arguments, "x x")
        || macro_path == NULL || macro_path->segment_count != 2u
        || !string_is(&ast, macro_path->segments[0].name, "wrap")
        || !string_is(&ast, macro_path->segments[1].name, "in_ref")
        || trailing.parse.error_count == 0u
        || trailing.type != CM_AST_TYPE_NONE
        || empty.parse.error_count == 0u
        || empty.type != CM_AST_TYPE_NONE
        || bound_function.parse.error_count != 0u
        || bound_function_type == NULL
        || bound_function_type->kind != CM_AST_TYPE_FUNCTION
        || !bound_function_type->is_unsafe
        || bound_function_type->binder.lifetime_count != 2u
        || bound_function_type->binder.lifetimes == NULL
        || !string_is(&ast, bound_function_type->binder.lifetimes[0], "'a")
        || !string_is(&ast, bound_function_type->binder.lifetimes[1], "'b")
        || implicit_function.parse.error_count != 0u
        || implicit_function_type == NULL
        || implicit_function_type->kind != CM_AST_TYPE_FUNCTION
        || implicit_function_type->binder.lifetime_count != 0u
        || implicit_function_type->binder.lifetimes != NULL
        || duplicate_binder.parse.error_count != 0u) {
        fail("type-fragment",
            "complete type fragment boundaries were not enforced");
    }
    cm_ast_destroy(&ast);
}

static void test_invalid_arguments(void)
{
    CmExpressionFragment expression;
    CmItemListFragment items;
    CmAst ast;

    cm_ast_init(&ast);
    expression = cm_parse_expression_fragment(NULL, "1", 1u,
        CM_EDITION_2024);
    items = cm_parse_item_list_fragment(&ast, NULL, 1u,
        CM_EDITION_2024);
    if (expression.parse.error_count == 0u
        || expression.expression != CM_AST_EXPR_NONE
        || items.parse.error_count == 0u || items.items != NULL
        || items.item_count != 0u) {
        fail("invalid-arguments", "invalid fragment arguments were accepted");
    }
    cm_ast_destroy(&ast);
}

static void test_inner_attribute_ownership(void)
{
    static const char source[] =
        "#![no_core]\n"
        "mod inner { #![allow(dead_code)] struct Child; }\n";
    CmAst ast;
    CmParseResult result;
    const CmAstItem *module;
    const CmAstAttribute *crate_attribute;
    const CmAstAttribute *module_attribute;

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast, source, sizeof(source) - 1u,
        CM_EDITION_2024);
    module = ast.root_items.len == 1u
        ? cm_ast_get_item(&ast,
            *(const CmAstItemId *)cm_vec_at_const(&ast.root_items, 0u))
        : NULL;
    crate_attribute = ast.crate_attributes.len == 1u
        ? cm_ast_get_attribute(&ast,
            *(const CmAstAttributeId *)cm_vec_at_const(
                &ast.crate_attributes, 0u))
        : NULL;
    module_attribute = module != NULL
        && module->data.module_item.inner_attribute_count == 1u
        ? cm_ast_get_attribute(&ast,
            module->data.module_item.inner_attributes[0])
        : NULL;
    if (result.error_count != 0u || module == NULL
        || module->kind != CM_AST_ITEM_MODULE
        || !module->data.module_item.is_inline
        || ast.crate_attributes.len != 1u
        || crate_attribute == NULL
        || crate_attribute->style != CM_AST_ATTR_INNER
        || module_attribute == NULL
        || module_attribute->style != CM_AST_ATTR_INNER
        || module->data.module_item.item_count != 1u) {
        fail("inner-ownership",
            "crate and inline-module inner attributes were conflated");
    }
    cm_ast_destroy(&ast);

    cm_ast_init(&ast);
    result = cm_parse_crate(&ast,
        "struct Before; #![allow(dead_code)] struct After;",
        strlen("struct Before; #![allow(dead_code)] struct After;"),
        CM_EDITION_2024);
    if (result.error_count == 0u) {
        fail("misplaced-inner",
            "inner attribute after an item was silently accepted");
    }
    cm_ast_destroy(&ast);
}

int main(void)
{
    test_expression_fragments();
    test_type_fragments();
    test_item_fragments();
    test_invalid_arguments();
    test_inner_attribute_ownership();
    if (failures != 0) {
        fprintf(stderr, "parser fragment tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("parser fragment tests: ok");
    return 0;
}
