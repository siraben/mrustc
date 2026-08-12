#ifndef CMRUSTC_CM_MACRO_EXPAND_H
#define CMRUSTC_CM_MACRO_EXPAND_H

#include "cm/macro.h"
#include "cm/syntax/ast.h"

/*
 * A cfg set owns no strings.  It is a named wrapper around the evaluator
 * environment so expansion cannot accidentally depend on process-global
 * target state.
 */
typedef struct CmCfgSet {
    CmCfgEnvironment environment;
} CmCfgSet;

void cm_cfg_set_init(CmCfgSet *set);

typedef struct CmExpandOptions {
    const CmCfgSet *cfg;
    unsigned int maximum_nesting;
    size_t maximum_items;
    size_t maximum_attribute_expansions;
} CmExpandOptions;

void cm_expand_options_init(CmExpandOptions *options,
    const CmCfgSet *cfg);

typedef enum CmExpandDiagnosticCode {
    CM_EXPAND_DIAG_NONE = 0,
    CM_EXPAND_DIAG_INVALID_ARGUMENT,
    CM_EXPAND_DIAG_INVALID_ITEM_ID,
    CM_EXPAND_DIAG_INVALID_ATTRIBUTE_ID,
    CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE,
    CM_EXPAND_DIAG_UNSUPPORTED_ATTRIBUTE,
    CM_EXPAND_DIAG_CFG_PREDICATE,
    CM_EXPAND_DIAG_NESTING_LIMIT,
    CM_EXPAND_DIAG_ITEM_LIMIT,
    CM_EXPAND_DIAG_ATTRIBUTE_LIMIT
} CmExpandDiagnosticCode;

typedef struct CmExpandDiagnostic {
    CmExpandDiagnosticCode code;
    CmAstItemId item_id;
    CmAstAttributeId attribute_id;
    CmAstSpan span;
    const char *message;
    /* Set for CM_EXPAND_DIAG_CFG_PREDICATE. */
    CmMacroDiagnostic cfg_diagnostic;
} CmExpandDiagnostic;

/*
 * `meta` is the attribute body without `#[` and `]`.  It points into the
 * immutable AST interner and remains valid for the lifetime of the AST.
 * cfg_attr payloads retain the ID of the cfg_attr which produced them.
 */
typedef struct CmEffectiveAttribute {
    CmAstAttributeId source_id;
    CmAstAttributeStyle style;
    CmAstSpan span;
    const unsigned char *meta;
    size_t meta_length;
    unsigned int expansion_depth;
} CmEffectiveAttribute;

typedef enum CmExpandedChildKind {
    CM_EXPANDED_CHILD_NONE = 0,
    CM_EXPANDED_CHILD_MODULE,
    CM_EXPANDED_CHILD_EXTERN_BLOCK,
    CM_EXPANDED_CHILD_TRAIT,
    CM_EXPANDED_CHILD_IMPL
} CmExpandedChildKind;

typedef struct CmExpandedItem {
    CmAstItemId source_id;
    CmAstSpan span;
    CmEffectiveAttribute *attributes;
    size_t attribute_count;
    /* Effective `#![...]` attributes owned by an inline module item. */
    CmEffectiveAttribute *inner_attributes;
    size_t inner_attribute_count;
    CmExpandedChildKind child_kind;
    struct CmExpandedItem *children;
    size_t child_count;
} CmExpandedItem;

typedef struct CmExpandedAst {
    int crate_is_active;
    CmEffectiveAttribute *crate_attributes;
    size_t crate_attribute_count;
    CmExpandedItem *root_items;
    size_t root_item_count;
} CmExpandedAst;

/*
 * An owning cfg-expanded view of an arbitrary AST item-ID sequence.  Item and
 * attribute arrays are owned by the sequence; effective attribute metadata
 * continues to borrow immutable bytes from `ast`, like CmExpandedAst.
 */
typedef struct CmExpandedItemSequence {
    CmExpandedItem *items;
    size_t item_count;
} CmExpandedItemSequence;

/*
 * An owning effective view of an attribute list which is not itself attached
 * to an AST item, such as an enum variant's outer attributes.  Metadata bytes
 * continue to borrow the immutable AST interner.
 */
typedef struct CmExpandedAttributeList {
    CmEffectiveAttribute *attributes;
    size_t attribute_count;
    int is_active;
} CmExpandedAttributeList;

typedef struct CmExpandResult {
    CmMacroStatus status;
    CmExpandDiagnostic diagnostic;
} CmExpandResult;

void cm_expanded_ast_init(CmExpandedAst *expanded);
void cm_expanded_ast_destroy(CmExpandedAst *expanded);
void cm_expanded_item_sequence_init(CmExpandedItemSequence *expanded);
void cm_expanded_item_sequence_destroy(CmExpandedItemSequence *expanded);
void cm_expanded_attribute_list_init(CmExpandedAttributeList *expanded);
void cm_expanded_attribute_list_destroy(CmExpandedAttributeList *expanded);

/*
 * Builds an immutable-source, owning derived view.  The caller initializes
 * `expanded` first and destroys it when done.  On failure it is left empty.
 * This phase intentionally handles attributes only; expression-level builtin
 * and macro invocation expansion can be added as later expansion phases
 * without changing the source AST or the view's item identity contract.
 */
CmExpandResult cm_expand_cfg_view(const CmAst *ast,
    const CmExpandOptions *options, CmExpandedAst *expanded);

/*
 * Applies cfg and cfg_attr recursively to `item_ids` without consulting
 * ast->root_items.  The caller initializes `expanded` first and destroys it
 * when done.  On failure it is left empty.
 */
CmExpandResult cm_expand_cfg_item_sequence(const CmAst *ast,
    const CmAstItemId *item_ids, size_t item_count,
    const CmExpandOptions *options, CmExpandedItemSequence *expanded);

/*
 * Applies the same cfg/cfg_attr processor to an arbitrary outer-attribute
 * list. The caller initializes and ultimately destroys `expanded`.
 */
CmExpandResult cm_expand_cfg_attribute_list(const CmAst *ast,
    CmAstItemId diagnostic_owner, const CmAstAttributeId *attribute_ids,
    size_t attribute_count, const CmExpandOptions *options,
    CmExpandedAttributeList *expanded);

const char *cm_expand_diagnostic_code_name(CmExpandDiagnosticCode code);

#endif
