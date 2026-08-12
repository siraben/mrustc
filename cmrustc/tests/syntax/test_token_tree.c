#include "cm/syntax/token_tree.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "%s: %s\n", test, message);
    failures += 1;
}

static struct cm_token_tree_result build_tree(
    struct cm_token_tree *tree,
    const char *source
)
{
    struct cm_lexer_options options;

    cm_token_tree_init(tree);
    cm_lexer_options_init(&options);
    options.edition = CM_EDITION_2024;
    return cm_token_tree_build(tree, source, strlen(source), &options);
}

static void test_nested_groups(void)
{
    const char *source;
    struct cm_token_tree tree;
    struct cm_token_tree_result result;
    const struct cm_tt_node *root;
    const struct cm_tt_node *paren;
    const struct cm_tt_node *brace;
    const struct cm_tt_node *bracket;

    source = "fn main(a: [u8; 2]) { value[0] }";
    result = build_tree(&tree, source);
    if (result.lexer_error_count != 0 || result.delimiter_error_count != 0)
        fail("nested", "valid nesting produced an error");
    if (result.node_count < 10)
        fail("nested", "too few nodes were constructed");

    root = cm_token_tree_node(&tree, tree.root);
    if (root == NULL || root->kind != CM_TT_NODE_ROOT ||
        root->first_child == CM_TT_ID_NONE)
        fail("nested", "root links are invalid");

    /* IDs follow deterministic source preorder: fn, main, (...), {...). */
    paren = cm_token_tree_node(&tree, 4);
    if (paren == NULL || paren->kind != CM_TT_NODE_GROUP ||
        paren->data.group.delimiter != CM_TT_DELIMITER_PAREN ||
        paren->data.group.open_span.start != 7 ||
        paren->data.group.close_span.start != 18)
        fail("nested", "parenthesis group metadata is wrong");
    bracket = cm_token_tree_node(&tree, 7);
    if (bracket == NULL || bracket->parent != paren->id ||
        bracket->data.group.delimiter != CM_TT_DELIMITER_BRACKET)
        fail("nested", "bracket parent or delimiter is wrong");
    brace = cm_token_tree_node(&tree, 11);
    if (brace == NULL || brace->parent != tree.root ||
        brace->data.group.delimiter != CM_TT_DELIMITER_BRACE)
        fail("nested", "brace parent or delimiter is wrong");

    cm_token_tree_destroy(&tree);
}

static void test_delimiter_errors(void)
{
    struct cm_token_tree tree;
    struct cm_token_tree_result result;
    const struct cm_tt_error *error;
    const struct cm_tt_node *token;

    result = build_tree(&tree, "([)]");
    if (result.delimiter_error_count != 2)
        fail("mismatch", "expected mismatch plus unclosed error");
    error = cm_token_tree_error(&tree, 0);
    if (error == NULL || error->kind != CM_TT_ERROR_MISMATCHED_CLOSE ||
        error->span.start != 2 || error->opener_span.start != 1 ||
        error->expected != CM_TT_DELIMITER_BRACKET ||
        error->actual != CM_TT_DELIMITER_PAREN)
        fail("mismatch", "mismatch diagnostic is not deterministic");
    error = cm_token_tree_error(&tree, 1);
    if (error == NULL || error->kind != CM_TT_ERROR_UNCLOSED_GROUP ||
        error->opener_span.start != 0 || error->span.start != 4 ||
        error->expected != CM_TT_DELIMITER_PAREN)
        fail("mismatch", "unclosed outer diagnostic is wrong");
    token = cm_token_tree_node(&tree, 4);
    if (token == NULL || token->kind != CM_TT_NODE_TOKEN ||
        token->data.token.kind != CM_TOKEN_RPAREN)
        fail("mismatch", "mismatched closer was not retained");
    cm_token_tree_destroy(&tree);

    result = build_tree(&tree, "}");
    if (result.delimiter_error_count != 1)
        fail("unexpected", "unexpected closer was not diagnosed");
    error = cm_token_tree_error(&tree, 0);
    if (error == NULL || error->kind != CM_TT_ERROR_UNEXPECTED_CLOSE ||
        error->actual != CM_TT_DELIMITER_BRACE ||
        error->expected != CM_TT_DELIMITER_NONE)
        fail("unexpected", "unexpected closer metadata is wrong");
    cm_token_tree_destroy(&tree);

    result = build_tree(&tree, "{(");
    if (result.delimiter_error_count != 2)
        fail("unclosed", "both unclosed groups must be diagnosed");
    error = cm_token_tree_error(&tree, 0);
    if (error == NULL || error->expected != CM_TT_DELIMITER_PAREN ||
        error->opener_span.start != 1)
        fail("unclosed", "inner group must be diagnosed first");
    error = cm_token_tree_error(&tree, 1);
    if (error == NULL || error->expected != CM_TT_DELIMITER_BRACE ||
        error->opener_span.start != 0)
        fail("unclosed", "outer group diagnostic is wrong");
    cm_token_tree_destroy(&tree);
}

static void test_canonical_dump(void)
{
    const char *source;
    const char *expected;
    struct cm_token_tree first;
    struct cm_token_tree second;
    CmStrBuf first_dump;
    CmStrBuf second_dump;

    source = "(a)";
    expected =
        "token-tree-v1\n"
        "node 1 parent=0 next=0 root\n"
        "node 2 parent=1 next=0 group=paren open=0:1@1:1 close=2:1@1:3\n"
        "node 3 parent=2 next=0 token=IDENT span=1:1@1:2 keyword= flags=0 detail=0 text=\"a\"\n"
        "errors 0\n";

    (void)build_tree(&first, source);
    (void)build_tree(&second, source);
    cm_str_buf_init(&first_dump);
    cm_str_buf_init(&second_dump);
    cm_token_tree_dump(&first, source, strlen(source), &first_dump);
    cm_token_tree_dump(&second, source, strlen(source), &second_dump);
    if (strcmp(cm_str_buf_c_str(&first_dump), expected) != 0) {
        fprintf(stderr, "canonical: unexpected dump:\n%s",
            cm_str_buf_c_str(&first_dump));
        failures += 1;
    }
    if (strcmp(
        cm_str_buf_c_str(&first_dump),
        cm_str_buf_c_str(&second_dump)
    ) != 0)
        fail("canonical", "same source produced different dumps");

    cm_str_buf_destroy(&second_dump);
    cm_str_buf_destroy(&first_dump);
    cm_token_tree_destroy(&second);
    cm_token_tree_destroy(&first);
}

static void test_dump_escaping(void)
{
    const char source[] = { '"', 'a', '\n', '\t', '\\', '"' };
    struct cm_token_tree tree;
    struct cm_lexer_options options;
    CmStrBuf dump;

    cm_token_tree_init(&tree);
    cm_lexer_options_init(&options);
    (void)cm_token_tree_build(&tree, source, sizeof(source), &options);
    cm_str_buf_init(&dump);
    cm_token_tree_dump(&tree, source, sizeof(source), &dump);
    if (strstr(cm_str_buf_c_str(&dump), "text=\"\\\"a\\n\\t\\\\\\\"\"") == NULL)
        fail("escaping", "canonical text escaping is unstable");
    cm_str_buf_destroy(&dump);
    cm_token_tree_destroy(&tree);
}

static void test_macro_signature_lifetimes(void)
{
    const char *source;
    struct cm_token_tree tree;
    struct cm_token_tree_result result;
    size_t index;
    size_t lifetime_count;
    size_t char_count;
    size_t paren_count;

    source = "define_bignum! { pub fn add<'a>(&'a mut self, other: "
        "&$name) -> &'a mut $name {} }";
    result = build_tree(&tree, source);
    lifetime_count = 0u;
    char_count = 0u;
    paren_count = 0u;
    for (index = 1u; index <= result.node_count; ++index) {
        const struct cm_tt_node *node;

        node = cm_token_tree_node(&tree, (cm_tt_id)index);
        if (node != NULL && node->kind == CM_TT_NODE_TOKEN) {
            if (node->data.token.kind == CM_TOKEN_LIFETIME)
                lifetime_count += 1u;
            if (node->data.token.kind == CM_TOKEN_CHAR)
                char_count += 1u;
        } else if (node != NULL && node->kind == CM_TT_NODE_GROUP
                && node->data.group.delimiter == CM_TT_DELIMITER_PAREN) {
            paren_count += 1u;
        }
    }
    if (result.lexer_error_count != 0u
        || result.delimiter_error_count != 0u
        || lifetime_count != 3u || char_count != 0u || paren_count != 1u) {
        fail("macro-signature",
            "bignum lifetime signature produced an invalid token tree");
    }
    cm_token_tree_destroy(&tree);
}

int main(void)
{
    test_nested_groups();
    test_delimiter_errors();
    test_canonical_dump();
    test_dump_escaping();
    test_macro_signature_lifetimes();
    if (failures != 0) {
        fprintf(stderr, "token-tree tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("token-tree tests: ok");
    return 0;
}
