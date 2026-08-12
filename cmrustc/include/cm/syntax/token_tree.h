#ifndef CM_SYNTAX_TOKEN_TREE_H
#define CM_SYNTAX_TOKEN_TREE_H

#include "cm/arena.h"
#include "cm/buf.h"
#include "cm/syntax/lexer.h"
#include "cm/vec.h"

typedef uint32_t cm_tt_id;

#define CM_TT_ID_NONE ((cm_tt_id)0)

enum cm_tt_node_kind {
    CM_TT_NODE_ROOT = 0,
    CM_TT_NODE_GROUP,
    CM_TT_NODE_TOKEN
};

enum cm_tt_delimiter {
    CM_TT_DELIMITER_NONE = 0,
    CM_TT_DELIMITER_PAREN,
    CM_TT_DELIMITER_BRACKET,
    CM_TT_DELIMITER_BRACE
};

struct cm_tt_span {
    size_t start;
    size_t length;
    size_t line;
    size_t column;
};

struct cm_tt_group {
    enum cm_tt_delimiter delimiter;
    struct cm_tt_span open_span;
    struct cm_tt_span close_span;
};

struct cm_tt_node {
    cm_tt_id id;
    enum cm_tt_node_kind kind;
    cm_tt_id parent;
    cm_tt_id first_child;
    cm_tt_id last_child;
    cm_tt_id next_sibling;
    union {
        struct cm_tt_group group;
        struct cm_token token;
    } data;
};

enum cm_tt_error_kind {
    CM_TT_ERROR_UNEXPECTED_CLOSE = 0,
    CM_TT_ERROR_MISMATCHED_CLOSE,
    CM_TT_ERROR_UNCLOSED_GROUP
};

struct cm_tt_error {
    enum cm_tt_error_kind kind;
    struct cm_tt_span span;
    struct cm_tt_span opener_span;
    enum cm_tt_delimiter expected;
    enum cm_tt_delimiter actual;
};

struct cm_token_tree {
    CmArena node_arena;
    CmVec nodes;
    CmVec errors;
    cm_tt_id root;
};

struct cm_token_tree_result {
    size_t token_count;
    size_t lexer_error_count;
    size_t delimiter_error_count;
    size_t node_count;
};

void cm_token_tree_init(struct cm_token_tree *tree);
void cm_token_tree_destroy(struct cm_token_tree *tree);

/*
 * A tree is built once after init. Trivia follows the supplied lexer options.
 * A mismatched closer is retained as a token leaf and does not close the
 * current group; this makes recovery deterministic without guessing intent.
 */
struct cm_token_tree_result cm_token_tree_build(
    struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    const struct cm_lexer_options *lexer_options
);

const struct cm_tt_node *cm_token_tree_node(
    const struct cm_token_tree *tree,
    cm_tt_id id
);
const struct cm_tt_error *cm_token_tree_error(
    const struct cm_token_tree *tree,
    size_t index
);

const char *cm_tt_delimiter_name(enum cm_tt_delimiter delimiter);
const char *cm_tt_error_kind_name(enum cm_tt_error_kind kind);

/* Appends the versioned, deterministic differential-test representation. */
void cm_token_tree_dump(
    const struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    CmStrBuf *output
);

#endif
