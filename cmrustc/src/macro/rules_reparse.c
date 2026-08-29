#include <stdlib.h>
#include <stdio.h>
#include "cm/macro/rules_reparse.h"

#include "cm/syntax/token_tree.h"

#include <string.h>

static CmMacroReparseResult cm_reparse_result_init(void)
{
    CmMacroReparseResult result;

    memset(&result, 0, sizeof(result));
    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.stage = CM_MACRO_REPARSE_STAGE_VALIDATE;
    result.kind = CM_MACRO_REPARSE_DIAG_INVALID_ARGUMENT;
    result.message = "invalid macro expansion-and-reparse argument";
    return result;
}

void cm_macro_reparse_options_init(CmMacroReparseOptions *options)
{
    if (options == NULL) {
        return;
    }
    cm_macro_syntax_options_init(&options->expansion);
    options->item_context = CM_ITEM_LIST_FRAGMENT_ROOT;
    options->maximum_output_bytes =
        CM_MACRO_REPARSE_DEFAULT_MAX_OUTPUT_BYTES;
    options->maximum_items = CM_MACRO_REPARSE_DEFAULT_MAX_ITEMS;
}

static int cm_reparse_options_valid(const CmMacroReparseOptions *options)
{
    return (options->item_context == CM_ITEM_LIST_FRAGMENT_ROOT
            || options->item_context == CM_ITEM_LIST_FRAGMENT_IMPL
            || options->item_context == CM_ITEM_LIST_FRAGMENT_EXTERN)
        && options->maximum_output_bytes != 0u
        && options->maximum_items != 0u;
}

static void cm_reparse_error(CmMacroReparseResult *result,
    CmMacroStatus status, CmMacroReparseStage stage,
    CmMacroReparseDiagnosticKind kind, const char *message)
{
    result->status = status;
    result->stage = stage;
    result->kind = kind;
    result->message = message;
    result->expression = CM_AST_EXPR_NONE;
    result->items = NULL;
    result->item_count = 0u;
}

static int cm_reparse_expression_invocation(const CmAst *ast,
    CmAstExprId id, const CmAstMacroInvocation **invocation)
{
    const CmAstExpr *expression;

    expression = cm_ast_get_expr(ast, id);
    if (expression == NULL || expression->kind != CM_AST_EXPR_MACRO
        || expression->data.macro_expr.form != CM_AST_MACRO_INVOCATION
        || expression->data.macro_expr.has_semicolon) {
        return 0;
    }
    *invocation = &expression->data.macro_expr;
    return 1;
}

static int cm_reparse_item_invocation(const CmAst *ast,
    CmAstItemId id)
{
    const CmAstItem *item;

    item = cm_ast_get_item(ast, id);
    return item != NULL && item->kind == CM_AST_ITEM_MACRO
        && item->data.macro_item.form == CM_AST_MACRO_INVOCATION
        && item->name == CM_INTERN_ID_NONE;
}

static int cm_reparse_output_within_limit(CmMacroReparseResult *result,
    const CmMacroReparseOptions *options, const CmStrBuf *source)
{
    result->generated_length = source->len;
    if (source->len <= options->maximum_output_bytes) {
        return 1;
    }
    cm_reparse_error(result, CM_MACRO_LIMIT_EXCEEDED,
        CM_MACRO_REPARSE_STAGE_OUTPUT_LIMIT,
        CM_MACRO_REPARSE_DIAG_OUTPUT_LIMIT,
        "macro transcription exceeds reparse output limit");
    return 0;
}

static void cm_reparse_success(CmMacroReparseResult *result)
{
    result->status = CM_MACRO_OK;
    result->stage = CM_MACRO_REPARSE_STAGE_COMPLETE;
    result->kind = CM_MACRO_REPARSE_DIAG_NONE;
    result->message = "";
}

static int cm_cfg_select_is_token(const struct cm_tt_node *node,
    enum cm_token_kind kind)
{
    return node != NULL && node->kind == CM_TT_NODE_TOKEN
        && node->data.token.kind == kind;
}

static int cm_cfg_select_is_fallback(const struct cm_token_tree *tree,
    const char *source, const struct cm_tt_node *first,
    const struct cm_tt_node *arrow)
{
    const struct cm_tt_node *next;

    if (!cm_cfg_select_is_token(first, CM_TOKEN_IDENT)
        || first->data.token.length != 1u
        || source[first->data.token.start] != '_') return 0;
    next = cm_token_tree_node(tree, first->next_sibling);
    return next == arrow;
}

static void cm_cfg_select_append_node(const struct cm_token_tree *tree,
    const struct cm_tt_node *node, const char *source, CmStrBuf *output)
{
    const struct cm_tt_node *child;

    if (node->kind == CM_TT_NODE_TOKEN) {
        cm_str_buf_append_n(output, source + node->data.token.start,
            node->data.token.length);
        return;
    }
    cm_str_buf_append_n(output,
        source + node->data.group.open_span.start,
        node->data.group.open_span.length);
    child = cm_token_tree_node(tree, node->first_child);
    while (child != NULL) {
        cm_cfg_select_append_node(tree, child, source, output);
        cm_str_buf_push(output, ' ');
        child = cm_token_tree_node(tree, child->next_sibling);
    }
    cm_str_buf_append_n(output,
        source + node->data.group.close_span.start,
        node->data.group.close_span.length);
}

static CmMacroReparseResult cm_cfg_select_reparse_selected(
    CmMacroReparseResult result, const char *source, size_t start,
    size_t length, CmAst *destination,
    const CmMacroReparseOptions *options)
{
    CmItemListFragment fragment;

    result.generated_length = length;
    if (length > options->maximum_output_bytes) {
        cm_reparse_error(&result, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_REPARSE_STAGE_OUTPUT_LIMIT,
            CM_MACRO_REPARSE_DIAG_OUTPUT_LIMIT,
            "cfg_select branch exceeds reparse output limit");
        return result;
    }
    result.reparse_attempted = 1;
    fragment = cm_parse_item_list_fragment_in_context(destination,
        source + start, length, options->expansion.edition,
        options->item_context);
    result.reparse = fragment.parse;
    if (fragment.parse.error_count != 0u) {
        if (getenv("CM_MACRO_DEBUG") != NULL)
            fprintf(stderr, "MACRO reparse (cfg_select) failed: %s\n--- text ---\n%.*s\n--- end ---\n",
                fragment.parse.first_error.message, (int)length,
                source + start);
        cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_REPARSE_STAGE_PARSE,
            CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX,
            "selected cfg_select branch is not a complete Rust item list");
        return result;
    }
    if (fragment.item_count > options->maximum_items) {
        cm_reparse_error(&result, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_REPARSE_STAGE_ITEM_LIMIT,
            CM_MACRO_REPARSE_DIAG_ITEM_LIMIT,
            "selected cfg_select branch exceeds reparse item limit");
        return result;
    }
    result.items = fragment.items;
    result.item_count = fragment.item_count;
    cm_reparse_success(&result);
    return result;
}

CmMacroReparseResult cm_cfg_select_reparse_items(
    const CmAst *invocation_ast, CmAstItemId invocation_item,
    const CmCfgEnvironment *environment, CmAst *destination,
    const CmMacroReparseOptions *options)
{
    CmMacroReparseResult result;
    CmMacroReparseOptions effective_options;
    const CmAstItem *item;
    const CmInternedString *arguments;
    const char *argument_text;
    struct cm_lexer_options lexer_options;
    struct cm_token_tree tree;
    struct cm_token_tree_result tree_result;
    const struct cm_tt_node *root;
    const struct cm_tt_node *cursor;
    size_t selected_start;
    size_t selected_length;
    int selected;
    int saw_fallback;

    result = cm_reparse_result_init();
    if (options == NULL) cm_macro_reparse_options_init(&effective_options);
    else effective_options = *options;
    if (invocation_ast == NULL || environment == NULL
        || destination == NULL || !cm_reparse_options_valid(
            &effective_options)
        || !cm_reparse_item_invocation(invocation_ast, invocation_item)) {
        return result;
    }
    item = cm_ast_get_item(invocation_ast, invocation_item);
    arguments = item == NULL ? NULL : cm_ast_get_string(invocation_ast,
        item->data.macro_item.arguments);
    if (arguments == NULL) return result;
    argument_text = (const char *)arguments->bytes;

    cm_lexer_options_init(&lexer_options);
    lexer_options.edition = effective_options.expansion.edition;
    cm_token_tree_init(&tree);
    tree_result = cm_token_tree_build(&tree, argument_text,
        arguments->len, &lexer_options);
    root = cm_token_tree_node(&tree, tree.root);
    if (tree_result.lexer_error_count != 0u
        || tree_result.delimiter_error_count != 0u || root == NULL) {
        cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_REPARSE_STAGE_EXPAND,
            CM_MACRO_REPARSE_DIAG_EXPANSION,
            "cfg_select input contains an invalid token tree");
        cm_token_tree_destroy(&tree);
        return result;
    }

    cursor = cm_token_tree_node(&tree, root->first_child);
    selected_start = 0u;
    selected_length = 0u;
    selected = 0;
    saw_fallback = 0;
    result.expansion_attempted = 1;
    while (cursor != NULL) {
        const struct cm_tt_node *predicate;
        const struct cm_tt_node *arrow;
        const struct cm_tt_node *body;
        const struct cm_tt_node *next;
        CmCfgEvaluation evaluation;
        int fallback;
        int take;
        CmStrBuf predicate_text;

        predicate = cursor;
        arrow = predicate;
        while (arrow != NULL
            && !cm_cfg_select_is_token(arrow, CM_TOKEN_FAT_ARROW)) {
            arrow = cm_token_tree_node(&tree, arrow->next_sibling);
        }
        body = arrow == NULL ? NULL
            : cm_token_tree_node(&tree, arrow->next_sibling);
        if (arrow == NULL || body == NULL
            || body->kind != CM_TT_NODE_GROUP
            || body->data.group.delimiter != CM_TT_DELIMITER_BRACE) {
            cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_REPARSE_STAGE_EXPAND,
                CM_MACRO_REPARSE_DIAG_EXPANSION,
                "expected `cfg predicate => { items }` branch");
            cm_token_tree_destroy(&tree);
            return result;
        }
        fallback = cm_cfg_select_is_fallback(&tree, argument_text,
            predicate, arrow);
        if (saw_fallback || (fallback && body->next_sibling != CM_TT_ID_NONE)) {
            cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_REPARSE_STAGE_EXPAND,
                CM_MACRO_REPARSE_DIAG_EXPANSION,
                "cfg_select fallback must be the final branch");
            cm_token_tree_destroy(&tree);
            return result;
        }
        take = fallback;
        if (fallback) {
            saw_fallback = 1;
        } else {
            const struct cm_tt_node *predicate_node;

            cm_str_buf_init(&predicate_text);
            predicate_node = predicate;
            while (predicate_node != arrow) {
                cm_cfg_select_append_node(&tree, predicate_node,
                    argument_text, &predicate_text);
                cm_str_buf_push(&predicate_text, ' ');
                predicate_node = cm_token_tree_node(&tree,
                    predicate_node->next_sibling);
            }
            evaluation = cm_cfg_evaluate(environment,
                cm_str_buf_c_str(&predicate_text), predicate_text.len);
            cm_str_buf_destroy(&predicate_text);
            if (evaluation.status != CM_MACRO_OK) {
                cm_reparse_error(&result, evaluation.status,
                    CM_MACRO_REPARSE_STAGE_EXPAND,
                    CM_MACRO_REPARSE_DIAG_EXPANSION,
                    evaluation.diagnostic.message);
                cm_token_tree_destroy(&tree);
                return result;
            }
            take = evaluation.value;
        }
        if (!selected && take) {
            selected_start = body->data.group.open_span.start
                + body->data.group.open_span.length;
            selected_length = body->data.group.close_span.start
                - selected_start;
            selected = 1;
        }
        next = cm_token_tree_node(&tree, body->next_sibling);
        if (cm_cfg_select_is_token(next, CM_TOKEN_COMMA)) {
            next = cm_token_tree_node(&tree, next->next_sibling);
        }
        cursor = next;
    }
    if (!selected) {
        cm_reparse_error(&result, CM_MACRO_NO_MATCH,
            CM_MACRO_REPARSE_STAGE_EXPAND,
            CM_MACRO_REPARSE_DIAG_EXPANSION,
            "no cfg_select predicate matched and no fallback was provided");
        cm_token_tree_destroy(&tree);
        return result;
    }
    result = cm_cfg_select_reparse_selected(result, argument_text,
        selected_start, selected_length, destination, &effective_options);
    cm_token_tree_destroy(&tree);
    return result;
}

CmMacroReparseResult cm_macro_rules_reparse_expression(
    const CmAst *definition_ast, CmAstItemId definition_item,
    const CmAst *invocation_ast, CmAstExprId invocation_expression,
    CmAst *destination, const CmMacroReparseOptions *options)
{
    CmMacroReparseResult result;
    CmMacroReparseOptions effective_options;
    const CmAstMacroInvocation *invocation;
    CmStrBuf source;
    CmExpressionFragment fragment;

    result = cm_reparse_result_init();
    if (options == NULL) {
        cm_macro_reparse_options_init(&effective_options);
    } else {
        effective_options = *options;
    }
    if (definition_ast == NULL || invocation_ast == NULL
        || destination == NULL
        || !cm_reparse_options_valid(&effective_options)) {
        return result;
    }
    if (!cm_reparse_expression_invocation(invocation_ast,
        invocation_expression, &invocation)) {
        cm_reparse_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_REPARSE_STAGE_VALIDATE,
            CM_MACRO_REPARSE_DIAG_EXPECTED_EXPRESSION_INVOCATION,
            "expected an expression-position macro invocation");
        return result;
    }

    cm_str_buf_init(&source);
    result.expansion_attempted = 1;
    result.expansion = cm_macro_syntax_expand(definition_ast,
        definition_item, invocation_ast, invocation,
        &effective_options.expansion, &source);
    if (result.expansion.status != CM_MACRO_OK) {
        cm_reparse_error(&result, result.expansion.status,
            CM_MACRO_REPARSE_STAGE_EXPAND,
            CM_MACRO_REPARSE_DIAG_EXPANSION,
            result.expansion.diagnostic.message);
        cm_str_buf_destroy(&source);
        return result;
    }
    if (!cm_reparse_output_within_limit(&result, &effective_options,
        &source)) {
        cm_str_buf_destroy(&source);
        return result;
    }

    result.reparse_attempted = 1;
    fragment = cm_parse_expression_fragment(destination,
        cm_str_buf_c_str(&source), source.len,
        effective_options.expansion.edition);
    result.reparse = fragment.parse;
    if (fragment.parse.error_count != 0u
        || fragment.expression == CM_AST_EXPR_NONE) {
        cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_REPARSE_STAGE_PARSE,
            CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX,
            "macro transcription is not one complete Rust expression");
        cm_str_buf_destroy(&source);
        return result;
    }
    result.expression = fragment.expression;
    cm_reparse_success(&result);
    cm_str_buf_destroy(&source);
    return result;
}

CmMacroReparseResult cm_macro_rules_reparse_items(
    const CmAst *definition_ast, CmAstItemId definition_item,
    const CmAst *invocation_ast, CmAstItemId invocation_item,
    CmAst *destination, const CmMacroReparseOptions *options)
{
    CmMacroReparseResult result;
    CmMacroReparseOptions effective_options;
    CmStrBuf source;
    CmItemListFragment fragment;

    result = cm_reparse_result_init();
    if (options == NULL) {
        cm_macro_reparse_options_init(&effective_options);
    } else {
        effective_options = *options;
    }
    if (definition_ast == NULL || invocation_ast == NULL
        || destination == NULL
        || !cm_reparse_options_valid(&effective_options)) {
        return result;
    }
    if (!cm_reparse_item_invocation(invocation_ast, invocation_item)) {
        cm_reparse_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_REPARSE_STAGE_VALIDATE,
            CM_MACRO_REPARSE_DIAG_EXPECTED_ITEM_INVOCATION,
            "expected an item-position macro invocation");
        return result;
    }

    cm_str_buf_init(&source);
    result.expansion_attempted = 1;
    result.expansion = cm_macro_syntax_expand_item(definition_ast,
        definition_item, invocation_ast, invocation_item,
        &effective_options.expansion, &source);
    if (result.expansion.status != CM_MACRO_OK) {
        cm_reparse_error(&result, result.expansion.status,
            CM_MACRO_REPARSE_STAGE_EXPAND,
            CM_MACRO_REPARSE_DIAG_EXPANSION,
            result.expansion.diagnostic.message);
        cm_str_buf_destroy(&source);
        return result;
    }
    if (!cm_reparse_output_within_limit(&result, &effective_options,
        &source)) {
        cm_str_buf_destroy(&source);
        return result;
    }

    result.reparse_attempted = 1;
    fragment = cm_parse_item_list_fragment_in_context(destination,
        cm_str_buf_c_str(&source), source.len,
        effective_options.expansion.edition,
        effective_options.item_context);
    result.reparse = fragment.parse;
    if (fragment.parse.error_count != 0u) {
        if (getenv("CM_MACRO_DEBUG") != NULL)
            fprintf(stderr, "MACRO reparse (items) failed: %s\n--- text ---\n%.*s\n--- end ---\n",
                fragment.parse.first_error.message, (int)source.len,
                cm_str_buf_c_str(&source));
        cm_reparse_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_REPARSE_STAGE_PARSE,
            CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX,
            "macro transcription is not a complete Rust item list");
        cm_str_buf_destroy(&source);
        return result;
    }
    if (fragment.item_count > effective_options.maximum_items) {
        cm_reparse_error(&result, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_REPARSE_STAGE_ITEM_LIMIT,
            CM_MACRO_REPARSE_DIAG_ITEM_LIMIT,
            "macro transcription exceeds reparse item limit");
        cm_str_buf_destroy(&source);
        return result;
    }
    result.items = fragment.items;
    result.item_count = fragment.item_count;
    cm_reparse_success(&result);
    cm_str_buf_destroy(&source);
    return result;
}

const char *cm_macro_reparse_stage_name(CmMacroReparseStage stage)
{
    switch (stage) {
    case CM_MACRO_REPARSE_STAGE_VALIDATE: return "validate";
    case CM_MACRO_REPARSE_STAGE_EXPAND: return "expand";
    case CM_MACRO_REPARSE_STAGE_OUTPUT_LIMIT: return "output limit";
    case CM_MACRO_REPARSE_STAGE_PARSE: return "parse";
    case CM_MACRO_REPARSE_STAGE_ITEM_LIMIT: return "item limit";
    case CM_MACRO_REPARSE_STAGE_COMPLETE: return "complete";
    }
    return "unknown";
}

const char *cm_macro_reparse_diagnostic_kind_name(
    CmMacroReparseDiagnosticKind kind)
{
    switch (kind) {
    case CM_MACRO_REPARSE_DIAG_NONE: return "none";
    case CM_MACRO_REPARSE_DIAG_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_MACRO_REPARSE_DIAG_EXPECTED_EXPRESSION_INVOCATION:
        return "expected expression invocation";
    case CM_MACRO_REPARSE_DIAG_EXPECTED_ITEM_INVOCATION:
        return "expected item invocation";
    case CM_MACRO_REPARSE_DIAG_EXPANSION: return "expansion";
    case CM_MACRO_REPARSE_DIAG_OUTPUT_LIMIT: return "output limit";
    case CM_MACRO_REPARSE_DIAG_GENERATED_SYNTAX:
        return "generated syntax";
    case CM_MACRO_REPARSE_DIAG_ITEM_LIMIT: return "item limit";
    }
    return "unknown";
}
