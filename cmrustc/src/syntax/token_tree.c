#include "cm/syntax/token_tree.h"

#include "cm/alloc.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cm_tt_node_alignment_probe {
    char prefix;
    struct cm_tt_node node;
};

struct cm_tt_builder {
    struct cm_token_tree *tree;
    cm_tt_id current_group;
    struct cm_tt_span eof_span;
};

static struct cm_tt_span cm_tt_span_absent(void)
{
    struct cm_tt_span span;

    span.start = SIZE_MAX;
    span.length = 0;
    span.line = 0;
    span.column = 0;
    return span;
}

static struct cm_tt_span cm_tt_span_from_token(const struct cm_token *token)
{
    struct cm_tt_span span;

    span.start = token->start;
    span.length = token->length;
    span.line = token->line;
    span.column = token->column;
    return span;
}

static struct cm_tt_node *cm_token_tree_node_mut(
    struct cm_token_tree *tree,
    cm_tt_id id
)
{
    struct cm_tt_node **slot;

    if (id == CM_TT_ID_NONE) {
        return NULL;
    }
    slot = (struct cm_tt_node **)cm_vec_at(&tree->nodes, (size_t)id - 1);
    return slot == NULL ? NULL : *slot;
}

const struct cm_tt_node *cm_token_tree_node(
    const struct cm_token_tree *tree,
    cm_tt_id id
)
{
    struct cm_tt_node *const *slot;

    if (id == CM_TT_ID_NONE) {
        return NULL;
    }
    slot = (struct cm_tt_node *const *)cm_vec_at_const(
        &tree->nodes,
        (size_t)id - 1
    );
    return slot == NULL ? NULL : *slot;
}

const struct cm_tt_error *cm_token_tree_error(
    const struct cm_token_tree *tree,
    size_t index
)
{
    return (const struct cm_tt_error *)cm_vec_at_const(&tree->errors, index);
}

static struct cm_tt_node *cm_tt_new_node(
    struct cm_token_tree *tree,
    enum cm_tt_node_kind kind,
    cm_tt_id parent
)
{
    struct cm_tt_node *node;
    struct cm_tt_node **index_entry;
    size_t alignment;
    size_t next_id;

    if (!cm_size_add(tree->nodes.len, 1, &next_id)
        || next_id > (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    alignment = offsetof(struct cm_tt_node_alignment_probe, node);
    node = (struct cm_tt_node *)cm_arena_alloc_zeroed(
        &tree->node_arena,
        1,
        sizeof(*node),
        alignment
    );
    node->id = (cm_tt_id)next_id;
    node->kind = kind;
    node->parent = parent;
    index_entry = (struct cm_tt_node **)cm_vec_push_uninit(&tree->nodes);
    *index_entry = node;
    return node;
}

static void cm_tt_link_child(
    struct cm_token_tree *tree,
    cm_tt_id parent_id,
    struct cm_tt_node *child
)
{
    struct cm_tt_node *parent;
    struct cm_tt_node *previous;

    parent = cm_token_tree_node_mut(tree, parent_id);
    if (parent == NULL) {
        abort();
    }
    if (parent->last_child == CM_TT_ID_NONE) {
        parent->first_child = child->id;
    } else {
        previous = cm_token_tree_node_mut(tree, parent->last_child);
        if (previous == NULL) {
            abort();
        }
        previous->next_sibling = child->id;
    }
    parent->last_child = child->id;
}

static enum cm_tt_delimiter cm_tt_open_delimiter(enum cm_token_kind kind)
{
    switch (kind) {
    case CM_TOKEN_LPAREN:
        return CM_TT_DELIMITER_PAREN;
    case CM_TOKEN_LBRACKET:
        return CM_TT_DELIMITER_BRACKET;
    case CM_TOKEN_LBRACE:
        return CM_TT_DELIMITER_BRACE;
    default:
        return CM_TT_DELIMITER_NONE;
    }
}

static enum cm_tt_delimiter cm_tt_close_delimiter(enum cm_token_kind kind)
{
    switch (kind) {
    case CM_TOKEN_RPAREN:
        return CM_TT_DELIMITER_PAREN;
    case CM_TOKEN_RBRACKET:
        return CM_TT_DELIMITER_BRACKET;
    case CM_TOKEN_RBRACE:
        return CM_TT_DELIMITER_BRACE;
    default:
        return CM_TT_DELIMITER_NONE;
    }
}

static void cm_tt_add_error(
    struct cm_token_tree *tree,
    enum cm_tt_error_kind kind,
    struct cm_tt_span span,
    struct cm_tt_span opener_span,
    enum cm_tt_delimiter expected,
    enum cm_tt_delimiter actual
)
{
    struct cm_tt_error *error;

    error = (struct cm_tt_error *)cm_vec_push_uninit(&tree->errors);
    error->kind = kind;
    error->span = span;
    error->opener_span = opener_span;
    error->expected = expected;
    error->actual = actual;
}

static void cm_tt_add_token(
    struct cm_tt_builder *builder,
    const struct cm_token *token
)
{
    struct cm_tt_node *node;

    node = cm_tt_new_node(
        builder->tree,
        CM_TT_NODE_TOKEN,
        builder->current_group
    );
    node->data.token = *token;
    cm_tt_link_child(builder->tree, builder->current_group, node);
}

static int cm_tt_accept_token(void *user, const struct cm_token *token)
{
    struct cm_tt_builder *builder;
    struct cm_tt_node *node;
    struct cm_tt_node *current;
    enum cm_tt_delimiter open_delimiter;
    enum cm_tt_delimiter close_delimiter;

    builder = (struct cm_tt_builder *)user;
    if (token->kind == CM_TOKEN_EOF) {
        builder->eof_span = cm_tt_span_from_token(token);
        return 0;
    }

    open_delimiter = cm_tt_open_delimiter(token->kind);
    if (open_delimiter != CM_TT_DELIMITER_NONE) {
        node = cm_tt_new_node(
            builder->tree,
            CM_TT_NODE_GROUP,
            builder->current_group
        );
        node->data.group.delimiter = open_delimiter;
        node->data.group.open_span = cm_tt_span_from_token(token);
        node->data.group.close_span = cm_tt_span_absent();
        cm_tt_link_child(builder->tree, builder->current_group, node);
        builder->current_group = node->id;
        return 0;
    }

    close_delimiter = cm_tt_close_delimiter(token->kind);
    if (close_delimiter != CM_TT_DELIMITER_NONE) {
        current = cm_token_tree_node_mut(
            builder->tree,
            builder->current_group
        );
        if (current == NULL) {
            abort();
        }
        if (current->kind == CM_TT_NODE_ROOT) {
            cm_tt_add_error(
                builder->tree,
                CM_TT_ERROR_UNEXPECTED_CLOSE,
                cm_tt_span_from_token(token),
                cm_tt_span_absent(),
                CM_TT_DELIMITER_NONE,
                close_delimiter
            );
            cm_tt_add_token(builder, token);
        } else if (current->data.group.delimiter != close_delimiter) {
            cm_tt_add_error(
                builder->tree,
                CM_TT_ERROR_MISMATCHED_CLOSE,
                cm_tt_span_from_token(token),
                current->data.group.open_span,
                current->data.group.delimiter,
                close_delimiter
            );
            cm_tt_add_token(builder, token);
        } else {
            current->data.group.close_span = cm_tt_span_from_token(token);
            builder->current_group = current->parent;
        }
        return 0;
    }

    cm_tt_add_token(builder, token);
    return 0;
}

void cm_token_tree_init(struct cm_token_tree *tree)
{
    cm_arena_init(&tree->node_arena, 4096);
    cm_vec_init(&tree->nodes, sizeof(struct cm_tt_node *));
    cm_vec_init(&tree->errors, sizeof(struct cm_tt_error));
    tree->root = CM_TT_ID_NONE;
}

void cm_token_tree_destroy(struct cm_token_tree *tree)
{
    cm_vec_destroy(&tree->errors);
    cm_vec_destroy(&tree->nodes);
    cm_arena_destroy(&tree->node_arena);
    tree->root = CM_TT_ID_NONE;
}

struct cm_token_tree_result cm_token_tree_build(
    struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    const struct cm_lexer_options *lexer_options
)
{
    struct cm_token_tree_result result;
    struct cm_lexer_result lexer_result;
    struct cm_tt_builder builder;
    struct cm_tt_node *root;
    struct cm_tt_node *unclosed;

    if ((source == NULL && source_length != 0)
        || tree->nodes.len != 0
        || tree->errors.len != 0
        || tree->root != CM_TT_ID_NONE) {
        abort();
    }
    root = cm_tt_new_node(tree, CM_TT_NODE_ROOT, CM_TT_ID_NONE);
    tree->root = root->id;
    builder.tree = tree;
    builder.current_group = tree->root;
    builder.eof_span.start = source_length;
    builder.eof_span.length = 0;
    builder.eof_span.line = 1;
    builder.eof_span.column = 1;

    lexer_result = cm_lex(
        source,
        source_length,
        lexer_options,
        cm_tt_accept_token,
        &builder
    );
    while (builder.current_group != tree->root) {
        unclosed = cm_token_tree_node_mut(tree, builder.current_group);
        if (unclosed == NULL || unclosed->kind != CM_TT_NODE_GROUP) {
            abort();
        }
        cm_tt_add_error(
            tree,
            CM_TT_ERROR_UNCLOSED_GROUP,
            builder.eof_span,
            unclosed->data.group.open_span,
            unclosed->data.group.delimiter,
            CM_TT_DELIMITER_NONE
        );
        builder.current_group = unclosed->parent;
    }

    result.token_count = lexer_result.token_count;
    result.lexer_error_count = lexer_result.error_count;
    result.delimiter_error_count = tree->errors.len;
    result.node_count = tree->nodes.len;
    return result;
}

const char *cm_tt_delimiter_name(enum cm_tt_delimiter delimiter)
{
    switch (delimiter) {
    case CM_TT_DELIMITER_NONE:
        return "none";
    case CM_TT_DELIMITER_PAREN:
        return "paren";
    case CM_TT_DELIMITER_BRACKET:
        return "bracket";
    case CM_TT_DELIMITER_BRACE:
        return "brace";
    }
    return "unknown";
}

const char *cm_tt_error_kind_name(enum cm_tt_error_kind kind)
{
    switch (kind) {
    case CM_TT_ERROR_UNEXPECTED_CLOSE:
        return "unexpected-close";
    case CM_TT_ERROR_MISMATCHED_CLOSE:
        return "mismatched-close";
    case CM_TT_ERROR_UNCLOSED_GROUP:
        return "unclosed-group";
    }
    return "unknown";
}

static void cm_tt_dump_size(CmStrBuf *output, size_t value)
{
    char buffer[3 * sizeof(size_t) + 1];

    (void)snprintf(buffer, sizeof(buffer), "%lu", (unsigned long)value);
    cm_str_buf_append(output, buffer);
}

static void cm_tt_dump_id(CmStrBuf *output, cm_tt_id id)
{
    cm_tt_dump_size(output, (size_t)id);
}

static void cm_tt_dump_span(CmStrBuf *output, struct cm_tt_span span)
{
    if (span.start == SIZE_MAX) {
        cm_str_buf_push(output, '-');
        return;
    }
    cm_tt_dump_size(output, span.start);
    cm_str_buf_push(output, ':');
    cm_tt_dump_size(output, span.length);
    cm_str_buf_push(output, '@');
    cm_tt_dump_size(output, span.line);
    cm_str_buf_push(output, ':');
    cm_tt_dump_size(output, span.column);
}

static char cm_tt_hex_digit(unsigned value)
{
    return (char)(value < 10 ? ('0' + value) : ('a' + value - 10));
}

static void cm_tt_dump_text(
    CmStrBuf *output,
    const char *source,
    size_t source_length,
    const struct cm_token *token
)
{
    size_t index;

    cm_str_buf_push(output, '"');
    if (token->start <= source_length
        && token->length <= source_length - token->start) {
        for (index = 0; index < token->length; index += 1) {
            unsigned char byte;

            byte = (unsigned char)source[token->start + index];
            if (byte == (unsigned char)'\\' || byte == (unsigned char)'"') {
                cm_str_buf_push(output, '\\');
                cm_str_buf_push(output, (char)byte);
            } else if (byte == (unsigned char)'\n') {
                cm_str_buf_append(output, "\\n");
            } else if (byte == (unsigned char)'\r') {
                cm_str_buf_append(output, "\\r");
            } else if (byte == (unsigned char)'\t') {
                cm_str_buf_append(output, "\\t");
            } else if (byte >= 0x20u && byte <= 0x7eu) {
                cm_str_buf_push(output, (char)byte);
            } else {
                cm_str_buf_append(output, "\\x");
                cm_str_buf_push(output, cm_tt_hex_digit((unsigned)byte >> 4));
                cm_str_buf_push(output, cm_tt_hex_digit((unsigned)byte & 15u));
            }
        }
    }
    cm_str_buf_push(output, '"');
}

static void cm_tt_dump_node(
    const struct cm_token_tree *tree,
    const struct cm_tt_node *node,
    const char *source,
    size_t source_length,
    CmStrBuf *output
)
{
    (void)tree;
    cm_str_buf_append(output, "node ");
    cm_tt_dump_id(output, node->id);
    cm_str_buf_append(output, " parent=");
    cm_tt_dump_id(output, node->parent);
    cm_str_buf_append(output, " next=");
    cm_tt_dump_id(output, node->next_sibling);
    if (node->kind == CM_TT_NODE_ROOT) {
        cm_str_buf_append(output, " root\n");
    } else if (node->kind == CM_TT_NODE_GROUP) {
        cm_str_buf_append(output, " group=");
        cm_str_buf_append(output, cm_tt_delimiter_name(node->data.group.delimiter));
        cm_str_buf_append(output, " open=");
        cm_tt_dump_span(output, node->data.group.open_span);
        cm_str_buf_append(output, " close=");
        cm_tt_dump_span(output, node->data.group.close_span);
        cm_str_buf_push(output, '\n');
    } else {
        const struct cm_token *token;

        token = &node->data.token;
        cm_str_buf_append(output, " token=");
        cm_str_buf_append(output, cm_token_kind_name(token->kind));
        cm_str_buf_append(output, " span=");
        cm_tt_dump_span(output, cm_tt_span_from_token(token));
        cm_str_buf_append(output, " keyword=");
        cm_str_buf_append(output, cm_keyword_name(token->keyword));
        cm_str_buf_append(output, " flags=");
        cm_tt_dump_size(output, (size_t)token->flags);
        cm_str_buf_append(output, " detail=");
        cm_tt_dump_size(output, (size_t)token->detail);
        cm_str_buf_append(output, " text=");
        cm_tt_dump_text(output, source, source_length, token);
        cm_str_buf_push(output, '\n');
    }
}

void cm_token_tree_dump(
    const struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    CmStrBuf *output
)
{
    size_t index;

    if (source == NULL && source_length != 0) {
        abort();
    }
    cm_str_buf_append(output, "token-tree-v1\n");
    for (index = 0; index < tree->nodes.len; index += 1) {
        const struct cm_tt_node *node;

        node = cm_token_tree_node(tree, (cm_tt_id)(index + 1));
        if (node == NULL) {
            abort();
        }
        cm_tt_dump_node(tree, node, source, source_length, output);
    }
    cm_str_buf_append(output, "errors ");
    cm_tt_dump_size(output, tree->errors.len);
    cm_str_buf_push(output, '\n');
    for (index = 0; index < tree->errors.len; index += 1) {
        const struct cm_tt_error *error;

        error = cm_token_tree_error(tree, index);
        if (error == NULL) {
            abort();
        }
        cm_str_buf_append(output, "error ");
        cm_tt_dump_size(output, index + 1);
        cm_str_buf_append(output, " kind=");
        cm_str_buf_append(output, cm_tt_error_kind_name(error->kind));
        cm_str_buf_append(output, " span=");
        cm_tt_dump_span(output, error->span);
        cm_str_buf_append(output, " opener=");
        cm_tt_dump_span(output, error->opener_span);
        cm_str_buf_append(output, " expected=");
        cm_str_buf_append(output, cm_tt_delimiter_name(error->expected));
        cm_str_buf_append(output, " actual=");
        cm_str_buf_append(output, cm_tt_delimiter_name(error->actual));
        cm_str_buf_push(output, '\n');
    }
}
