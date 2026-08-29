#ifndef CM_SYNTAX_PARSER_H
#define CM_SYNTAX_PARSER_H

#include "cm/syntax/ast.h"
#include "cm/syntax/token.h"

typedef struct CmParseError {
    size_t offset;
    size_t line;
    size_t column;
    char message[160];
} CmParseError;

typedef struct CmParseResult {
    size_t error_count;
    CmParseError first_error;
} CmParseResult;

typedef struct CmExpressionFragment {
    CmParseResult parse;
    CmAstExprId expression;
} CmExpressionFragment;

typedef struct CmTypeFragment {
    CmParseResult parse;
    CmAstTypeId type;
} CmTypeFragment;

typedef struct CmItemListFragment {
    CmParseResult parse;
    const CmAstItemId *items;
    uint32_t item_count;
} CmItemListFragment;

typedef enum CmItemListFragmentContext {
    CM_ITEM_LIST_FRAGMENT_ROOT = 0,
    CM_ITEM_LIST_FRAGMENT_IMPL,
    /* Children of an `extern "C" { .. }` block: `safe fn` is permitted. */
    CM_ITEM_LIST_FRAGMENT_EXTERN
} CmItemListFragmentContext;

CmParseResult cm_parse_crate(CmAst *ast, const char *source,
    size_t source_length, enum cm_edition edition);

/*
 * Parse one expression and require the complete fragment to be consumed.
 * AST nodes and interned strings are appended to `ast`; the expression ID is
 * valid until `ast` is destroyed.  `expression` is NONE on every failure.
 * Parsing is not transactional: failed input may append unreachable recovery
 * nodes, just as normal crate error recovery does.
 */
CmExpressionFragment cm_parse_expression_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition);

/* Parse exactly one complete type without inserting a crate item. */
CmTypeFragment cm_parse_type_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition);

/*
 * Parse zero or more items and require the complete fragment to be consumed.
 * The returned ID array and all referenced nodes are owned by `ast`, remain
 * valid until `ast` is destroyed, and must not be freed by the caller.  On
 * failure `items` is NULL and `item_count` is zero; error-recovery nodes may
 * still have been appended internally.
 */
CmItemListFragment cm_parse_item_list_fragment(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition);

/*
 * Contextual form used when macro output occupies an associated-item list.
 * The root wrapper above remains the conservative default.
 */
CmItemListFragment cm_parse_item_list_fragment_in_context(CmAst *ast,
    const char *source, size_t source_length, enum cm_edition edition,
    CmItemListFragmentContext context);

#endif
