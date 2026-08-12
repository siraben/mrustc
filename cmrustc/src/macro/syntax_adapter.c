#include "cm/macro/syntax_adapter.h"

#include <string.h>

typedef struct CmMacroSyntaxTree {
    CmStrBuf source;
    struct cm_token_tree tree;
    cm_tt_id body;
} CmMacroSyntaxTree;

static CmMacroSyntaxResult cm_syntax_result_init(void)
{
    CmMacroSyntaxResult result;

    memset(&result, 0, sizeof(result));
    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.stage = CM_MACRO_SYNTAX_STAGE_VALIDATE;
    result.kind = CM_MACRO_SYNTAX_DIAG_INVALID_ARGUMENT;
    result.diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    result.diagnostic.message = "invalid AST macro_rules adapter argument";
    return result;
}

static void cm_syntax_error(CmMacroSyntaxResult *result,
    CmMacroStatus status, CmMacroSyntaxStage stage,
    CmMacroSyntaxDiagnosticKind kind, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    result->status = status;
    result->stage = stage;
    result->kind = kind;
    result->diagnostic.code = code;
    result->diagnostic.offset = offset;
    result->diagnostic.message = message;
}

void cm_macro_syntax_options_init(CmMacroSyntaxOptions *options)
{
    if (options == NULL) return;
    options->edition = CM_EDITION_2024;
    cm_macro_rules_limits_init(&options->limits);
    options->crate_identifier = "crate";
}

static int cm_syntax_edition_valid(enum cm_edition edition)
{
    return edition == CM_EDITION_2015 || edition == CM_EDITION_2018 ||
        edition == CM_EDITION_2021 || edition == CM_EDITION_2024;
}

static int cm_syntax_path_is(const CmAst *ast, CmAstPathId path_id,
    const char *expected)
{
    const CmAstPath *path;
    const CmInternedString *segment;
    size_t length;

    path = cm_ast_get_path(ast, path_id);
    if (path == NULL || path->absolute || path->segment_count != 1u)
        return 0;
    segment = cm_ast_get_string(ast, path->segments[0].name);
    length = strlen(expected);
    return segment != NULL && segment->len == length &&
        memcmp(segment->bytes, expected, length) == 0;
}

static int cm_syntax_invocation_valid(const CmAst *ast,
    const CmAstMacroInvocation *invocation)
{
    const CmAstPath *path;

    if (ast == NULL || invocation == NULL ||
        invocation->form != CM_AST_MACRO_INVOCATION ||
        cm_ast_get_string(ast, invocation->arguments) == NULL)
        return 0;
    path = cm_ast_get_path(ast, invocation->path);
    return path != NULL && path->segment_count != 0u &&
        invocation->delimiter >= CM_AST_DELIMITER_PAREN &&
        invocation->delimiter <= CM_AST_DELIMITER_BRACKET;
}

static int cm_syntax_rules_definition_valid(const CmAst *ast,
    const CmAstItem *item)
{
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO) return 0;
    if (item->data.macro_item.form == CM_AST_MACRO_RULES_DEFINITION) {
        return cm_syntax_path_is(ast, item->data.macro_item.path,
            "macro_rules");
    }
    return item->data.macro_item.form
            == CM_AST_MACRO_DECLARATIVE_DEFINITION
        && item->data.macro_item.path == CM_AST_PATH_NONE
        && item->data.macro_item.delimiter == CM_AST_DELIMITER_BRACE
        && !item->data.macro_item.has_semicolon;
}

static void cm_syntax_delimiters(CmAstDelimiter delimiter,
    char *opening, char *closing, enum cm_tt_delimiter *tree_delimiter)
{
    if (delimiter == CM_AST_DELIMITER_PAREN) {
        *opening = '(';
        *closing = ')';
        *tree_delimiter = CM_TT_DELIMITER_PAREN;
    } else if (delimiter == CM_AST_DELIMITER_BRACE) {
        *opening = '{';
        *closing = '}';
        *tree_delimiter = CM_TT_DELIMITER_BRACE;
    } else {
        *opening = '[';
        *closing = ']';
        *tree_delimiter = CM_TT_DELIMITER_BRACKET;
    }
}

static void cm_syntax_tree_init(CmMacroSyntaxTree *tree)
{
    cm_str_buf_init(&tree->source);
    cm_token_tree_init(&tree->tree);
    tree->body = CM_TT_ID_NONE;
}

static void cm_syntax_tree_destroy(CmMacroSyntaxTree *tree)
{
    cm_token_tree_destroy(&tree->tree);
    cm_str_buf_destroy(&tree->source);
    tree->body = CM_TT_ID_NONE;
}

static int cm_syntax_tree_parse(CmMacroSyntaxTree *tree,
    enum cm_tt_delimiter expected_delimiter, enum cm_edition edition,
    struct cm_token_tree_result *tree_result)
{
    struct cm_lexer_options lexer_options;
    const struct cm_tt_node *root;
    const struct cm_tt_node *body;

    cm_lexer_options_init(&lexer_options);
    lexer_options.edition = edition;
    *tree_result = cm_token_tree_build(&tree->tree,
        cm_str_buf_c_str(&tree->source), tree->source.len, &lexer_options);
    root = cm_token_tree_node(&tree->tree, tree->tree.root);
    tree->body = root == NULL ? CM_TT_ID_NONE : root->first_child;
    body = cm_token_tree_node(&tree->tree, tree->body);
    return tree_result->lexer_error_count == 0u &&
        tree_result->delimiter_error_count == 0u && body != NULL &&
        body->kind == CM_TT_NODE_GROUP &&
        body->data.group.delimiter == expected_delimiter &&
        body->next_sibling == CM_TT_ID_NONE;
}

static int cm_syntax_tree_build(CmMacroSyntaxTree *tree,
    const CmInternedString *interior, CmAstDelimiter delimiter,
    enum cm_edition edition, struct cm_token_tree_result *tree_result)
{
    enum cm_tt_delimiter expected_delimiter;
    char opening;
    char closing;

    cm_syntax_delimiters(delimiter, &opening, &closing,
        &expected_delimiter);
    cm_str_buf_push(&tree->source, opening);
    cm_str_buf_append_n(&tree->source, (const char *)interior->bytes,
        interior->len);
    cm_str_buf_push(&tree->source, '\n');
    cm_str_buf_push(&tree->source, closing);
    return cm_syntax_tree_parse(tree, expected_delimiter, edition,
        tree_result);
}

static int cm_syntax_tree_build_parameterized_definition(
    CmMacroSyntaxTree *tree, const CmInternedString *parameters,
    const CmInternedString *body, enum cm_edition edition,
    struct cm_token_tree_result *tree_result)
{
    cm_str_buf_append(&tree->source, "{(");
    cm_str_buf_append_n(&tree->source, (const char *)parameters->bytes,
        parameters->len);
    cm_str_buf_append(&tree->source, "\n)=>{");
    cm_str_buf_append_n(&tree->source, (const char *)body->bytes, body->len);
    cm_str_buf_append(&tree->source, "\n}\n}");
    return cm_syntax_tree_parse(tree, CM_TT_DELIMITER_BRACE, edition,
        tree_result);
}

static void cm_syntax_copy_rules_diagnostic(CmMacroSyntaxResult *result,
    CmMacroStatus status, CmMacroSyntaxStage stage,
    CmMacroDiagnostic diagnostic)
{
    result->status = status;
    result->stage = stage;
    result->kind = CM_MACRO_SYNTAX_DIAG_RULES;
    result->diagnostic = diagnostic;
}

static size_t cm_syntax_tree_error_offset(const CmMacroSyntaxTree *tree)
{
    const struct cm_tt_error *error;

    error = cm_token_tree_error(&tree->tree, 0u);
    return error == NULL ? 0u : error->span.start;
}

CmMacroSyntaxResult cm_macro_syntax_expand(
    const CmAst *definition_ast, CmAstItemId definition_item,
    const CmAst *invocation_ast, const CmAstMacroInvocation *invocation,
    const CmMacroSyntaxOptions *options, CmStrBuf *output)
{
    CmMacroSyntaxResult result;
    CmMacroSyntaxOptions effective_options;
    const CmAstItem *definition_item_node;
    const CmInternedString *definition_name;
    const CmInternedString *definition_parameters;
    const CmInternedString *definition_interior;
    const CmInternedString *invocation_interior;
    CmMacroSyntaxTree definition_tree;
    CmMacroSyntaxTree invocation_tree;
    struct cm_token_tree_result tree_result;
    CmMacroRulesDefinition definition;
    CmMacroRulesParseResult parse_result;
    CmMacroCaptureSet captures;
    CmMacroRulesMatchResult match_result;
    CmMacroRulesTranscribeResult transcribe_result;

    result = cm_syntax_result_init();
    if (output != NULL) cm_str_buf_clear(output);
    if (definition_ast == NULL || invocation_ast == NULL ||
        invocation == NULL || output == NULL) return result;
    if (options == NULL) {
        cm_macro_syntax_options_init(&effective_options);
    } else {
        effective_options = *options;
    }
    if (!cm_syntax_edition_valid(effective_options.edition)) return result;
    definition_item_node = cm_ast_get_item(definition_ast, definition_item);
    if (!cm_syntax_rules_definition_valid(definition_ast,
            definition_item_node)) {
        cm_syntax_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_SYNTAX_STAGE_VALIDATE,
            CM_MACRO_SYNTAX_DIAG_EXPECTED_NAMED_RULES,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "expected a rules-style macro item");
        return result;
    }
    definition_name = cm_ast_get_string(definition_ast,
        definition_item_node->name);
    definition_parameters = definition_item_node->data.macro_item.parameters
            == CM_INTERN_ID_NONE
        ? NULL : cm_ast_get_string(definition_ast,
            definition_item_node->data.macro_item.parameters);
    definition_interior = cm_ast_get_string(definition_ast,
        definition_item_node->data.macro_item.arguments);
    if (definition_name == NULL || definition_name->len == 0u ||
        definition_interior == NULL ||
        (definition_item_node->data.macro_item.parameters
                != CM_INTERN_ID_NONE
            && definition_parameters == NULL) ||
        definition_item_node->data.macro_item.delimiter <
            CM_AST_DELIMITER_PAREN ||
        definition_item_node->data.macro_item.delimiter >
            CM_AST_DELIMITER_BRACKET) {
        cm_syntax_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_SYNTAX_STAGE_VALIDATE,
            CM_MACRO_SYNTAX_DIAG_EXPECTED_NAMED_RULES,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "macro_rules definition must have a name and body");
        return result;
    }
    if (!cm_syntax_invocation_valid(invocation_ast, invocation)) {
        cm_syntax_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_SYNTAX_STAGE_VALIDATE,
            CM_MACRO_SYNTAX_DIAG_EXPECTED_INVOCATION,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "expected a valid AST macro invocation");
        return result;
    }
    invocation_interior = cm_ast_get_string(invocation_ast,
        invocation->arguments);

    cm_syntax_tree_init(&definition_tree);
    cm_syntax_tree_init(&invocation_tree);
    cm_macro_rules_definition_init(&definition);
    cm_macro_capture_set_init(&captures);
    if (!(definition_parameters == NULL
            ? cm_syntax_tree_build(&definition_tree, definition_interior,
                definition_item_node->data.macro_item.delimiter,
                effective_options.edition, &tree_result)
            : cm_syntax_tree_build_parameterized_definition(
                &definition_tree, definition_parameters,
                definition_interior, effective_options.edition,
                &tree_result))) {
        result.lexer_error_count = tree_result.lexer_error_count;
        result.delimiter_error_count = tree_result.delimiter_error_count;
        cm_syntax_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_SYNTAX_STAGE_DEFINITION_TREE,
            CM_MACRO_SYNTAX_DIAG_INVALID_DEFINITION_TREE,
            CM_MACRO_DIAG_RULES_INVALID_TREE,
            cm_syntax_tree_error_offset(&definition_tree),
            "macro_rules AST body did not form its recorded token tree");
        goto cleanup;
    }
    parse_result = cm_macro_rules_parse(&definition,
        &definition_tree.tree, cm_str_buf_c_str(&definition_tree.source),
        definition_tree.source.len, definition_tree.body,
        &effective_options.limits);
    if (parse_result.status != CM_MACRO_OK) {
        cm_syntax_copy_rules_diagnostic(&result, parse_result.status,
            CM_MACRO_SYNTAX_STAGE_RULES_PARSE, parse_result.diagnostic);
        goto cleanup;
    }
    if (!cm_syntax_tree_build(&invocation_tree, invocation_interior,
        invocation->delimiter, effective_options.edition, &tree_result)) {
        result.lexer_error_count = tree_result.lexer_error_count;
        result.delimiter_error_count = tree_result.delimiter_error_count;
        cm_syntax_error(&result, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_SYNTAX_STAGE_INVOCATION_TREE,
            CM_MACRO_SYNTAX_DIAG_INVALID_INVOCATION_TREE,
            CM_MACRO_DIAG_RULES_INVALID_TREE,
            cm_syntax_tree_error_offset(&invocation_tree),
            "macro invocation AST arguments did not form its recorded token tree");
        goto cleanup;
    }
    match_result = cm_macro_rules_match(&definition,
        &invocation_tree.tree, cm_str_buf_c_str(&invocation_tree.source),
        invocation_tree.source.len, invocation_tree.body, &captures);
    result.arm_index = match_result.arm_index;
    result.backtrack_steps = match_result.backtrack_steps;
    result.capture_count = captures.captures.len;
    if (match_result.status != CM_MACRO_OK) {
        cm_syntax_copy_rules_diagnostic(&result, match_result.status,
            CM_MACRO_SYNTAX_STAGE_RULES_MATCH, match_result.diagnostic);
        goto cleanup;
    }
    transcribe_result = cm_macro_rules_transcribe_with_crate(&definition,
        &captures, effective_options.crate_identifier, output);
    result.emitted_repetitions = transcribe_result.emitted_repetitions;
    if (transcribe_result.status != CM_MACRO_OK) {
        cm_syntax_copy_rules_diagnostic(&result, transcribe_result.status,
            CM_MACRO_SYNTAX_STAGE_RULES_TRANSCRIBE,
            transcribe_result.diagnostic);
        goto cleanup;
    }
    result.status = CM_MACRO_OK;
    result.stage = CM_MACRO_SYNTAX_STAGE_COMPLETE;
    result.kind = CM_MACRO_SYNTAX_DIAG_NONE;
    result.diagnostic.code = CM_MACRO_DIAG_NONE;
    result.diagnostic.offset = 0u;
    result.diagnostic.message = "";

cleanup:
    if (result.status != CM_MACRO_OK) cm_str_buf_clear(output);
    cm_macro_capture_set_destroy(&captures);
    cm_macro_rules_definition_destroy(&definition);
    cm_syntax_tree_destroy(&invocation_tree);
    cm_syntax_tree_destroy(&definition_tree);
    return result;
}

CmMacroSyntaxResult cm_macro_syntax_expand_item(
    const CmAst *definition_ast, CmAstItemId definition_item,
    const CmAst *invocation_ast, CmAstItemId invocation_item,
    const CmMacroSyntaxOptions *options, CmStrBuf *output)
{
    const CmAstItem *item;

    item = cm_ast_get_item(invocation_ast, invocation_item);
    if (item == NULL || item->kind != CM_AST_ITEM_MACRO ||
        item->data.macro_item.form != CM_AST_MACRO_INVOCATION ||
        item->name != CM_INTERN_ID_NONE) {
        CmMacroSyntaxResult result;

        result = cm_syntax_result_init();
        if (output != NULL) cm_str_buf_clear(output);
        cm_syntax_error(&result, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_SYNTAX_STAGE_VALIDATE,
            CM_MACRO_SYNTAX_DIAG_EXPECTED_INVOCATION,
            CM_MACRO_DIAG_INVALID_ARGUMENT, 0u,
            "expected an item-position macro invocation");
        return result;
    }
    return cm_macro_syntax_expand(definition_ast, definition_item,
        invocation_ast, &item->data.macro_item, options, output);
}

const char *cm_macro_syntax_stage_name(CmMacroSyntaxStage stage)
{
    switch (stage) {
    case CM_MACRO_SYNTAX_STAGE_VALIDATE: return "validate";
    case CM_MACRO_SYNTAX_STAGE_DEFINITION_TREE: return "definition tree";
    case CM_MACRO_SYNTAX_STAGE_RULES_PARSE: return "rules parse";
    case CM_MACRO_SYNTAX_STAGE_INVOCATION_TREE: return "invocation tree";
    case CM_MACRO_SYNTAX_STAGE_RULES_MATCH: return "rules match";
    case CM_MACRO_SYNTAX_STAGE_RULES_TRANSCRIBE: return "rules transcribe";
    case CM_MACRO_SYNTAX_STAGE_COMPLETE: return "complete";
    }
    return "unknown";
}

const char *cm_macro_syntax_diagnostic_kind_name(
    CmMacroSyntaxDiagnosticKind kind)
{
    switch (kind) {
    case CM_MACRO_SYNTAX_DIAG_NONE: return "none";
    case CM_MACRO_SYNTAX_DIAG_INVALID_ARGUMENT: return "invalid argument";
    case CM_MACRO_SYNTAX_DIAG_EXPECTED_NAMED_RULES:
        return "expected named macro_rules";
    case CM_MACRO_SYNTAX_DIAG_EXPECTED_INVOCATION:
        return "expected macro invocation";
    case CM_MACRO_SYNTAX_DIAG_INVALID_DEFINITION_TREE:
        return "invalid definition token tree";
    case CM_MACRO_SYNTAX_DIAG_INVALID_INVOCATION_TREE:
        return "invalid invocation token tree";
    case CM_MACRO_SYNTAX_DIAG_RULES: return "macro_rules diagnostic";
    }
    return "unknown";
}
