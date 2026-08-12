#ifndef CMRUSTC_CM_MACRO_ITEM_MACRO_PLAN_H
#define CMRUSTC_CM_MACRO_ITEM_MACRO_PLAN_H

#include "cm/macro/expand.h"
#include "cm/macro/rules_reparse.h"

#define CM_ITEM_MACRO_PLAN_DEFAULT_MAX_NESTING 64u
#define CM_ITEM_MACRO_PLAN_DEFAULT_MAX_ITEMS ((size_t)1000000u)
#define CM_ITEM_MACRO_PLAN_DEFAULT_MAX_EXPANSIONS ((size_t)100000u)
#define CM_ITEM_MACRO_PLAN_DEFAULT_MAX_GENERATED_BYTES ((size_t)67108864u)

typedef enum CmItemMacroPlanStage {
    CM_ITEM_MACRO_PLAN_STAGE_VALIDATE = 0,
    CM_ITEM_MACRO_PLAN_STAGE_CATALOG,
    CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
    CM_ITEM_MACRO_PLAN_STAGE_REPARSE,
    CM_ITEM_MACRO_PLAN_STAGE_CFG,
    CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
    CM_ITEM_MACRO_PLAN_STAGE_COMPLETE
} CmItemMacroPlanStage;

typedef enum CmItemMacroPlanDiagnosticKind {
    CM_ITEM_MACRO_PLAN_DIAG_NONE = 0,
    CM_ITEM_MACRO_PLAN_DIAG_INVALID_ARGUMENT,
    CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
    CM_ITEM_MACRO_PLAN_DIAG_FORWARD_MACRO,
    CM_ITEM_MACRO_PLAN_DIAG_OUT_OF_SCOPE_MACRO,
    CM_ITEM_MACRO_PLAN_DIAG_QUALIFIED_MACRO,
    CM_ITEM_MACRO_PLAN_DIAG_AMBIGUOUS_MACRO,
    CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
    CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO,
    CM_ITEM_MACRO_PLAN_DIAG_REPARSE,
    CM_ITEM_MACRO_PLAN_DIAG_CFG,
    CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT,
    CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT,
    CM_ITEM_MACRO_PLAN_DIAG_EXPANSION_LIMIT,
    CM_ITEM_MACRO_PLAN_DIAG_OUTPUT_LIMIT
} CmItemMacroPlanDiagnosticKind;

/*
 * Stable opaque caller-defined identity for one AST. Item IDs are meaningful
 * only together with this owner when scope crosses source files. Local graphs
 * use their 32-bit source IDs; values above UINT32_MAX are reserved for
 * graph-local external AST owners and must never be truncated into CmSourceId.
 */
typedef uint64_t CmItemMacroAstOwner;

#define CM_ITEM_MACRO_AST_OWNER_NONE ((CmItemMacroAstOwner)0)

typedef struct CmItemMacroItemRef {
    CmItemMacroAstOwner owner;
    CmAstItemId item;
} CmItemMacroItemRef;

/* The AST pointer is borrowed only for one cm_plan_item_macros call. */
typedef struct CmItemMacroScopeSeed {
    CmItemMacroItemRef definition;
    const CmAst *definition_ast;
} CmItemMacroScopeSeed;

typedef enum CmItemMacroResolvedBuiltin {
    CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE = 0,
    CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT
} CmItemMacroResolvedBuiltin;

/* Exact resolver-certified target for one source-qualified invocation. */
typedef struct CmItemMacroResolvedInvocation {
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    const CmAst *definition_ast;
    /* Borrowed defining-crate identifier for `$crate`; NULL means `crate`. */
    const char *crate_identifier;
    CmItemMacroResolvedBuiltin builtin;
} CmItemMacroResolvedInvocation;

typedef struct CmItemMacroPathSegment {
    const unsigned char *bytes;
    size_t length;
} CmItemMacroPathSegment;

/*
 * Reusable exact target for qualified invocations created during expansion.
 * It never authorizes a source invocation, whose item identity must use
 * CmItemMacroResolvedInvocation. Segment and crate-identifier storage is
 * borrowed for one planner call.
 */
typedef struct CmItemMacroResolvedGeneratedPath {
    const CmItemMacroPathSegment *segments;
    size_t segment_count;
    CmItemMacroItemRef definition;
    const CmAst *definition_ast;
    const char *crate_identifier;
} CmItemMacroResolvedGeneratedPath;

typedef struct CmItemMacroResolvedGeneratedTarget {
    CmItemMacroItemRef definition;
    const CmAst *definition_ast;
    const char *crate_identifier;
} CmItemMacroResolvedGeneratedTarget;

typedef enum CmItemMacroGeneratedLookupStatus {
    CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND = 0,
    CM_ITEM_MACRO_GENERATED_LOOKUP_OK,
    CM_ITEM_MACRO_GENERATED_LOOKUP_AMBIGUOUS,
    CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID
} CmItemMacroGeneratedLookupStatus;

/* Segment bytes borrow the consumer AST only for the callback duration. */
typedef CmItemMacroGeneratedLookupStatus
(*CmItemMacroGeneratedPathResolver)(void *context,
    const CmItemMacroPathSegment *segments, size_t segment_count,
    CmItemMacroResolvedGeneratedTarget *out_target);

typedef struct CmItemMacroPlanOptions {
    const CmCfgSet *cfg;
    CmItemMacroAstOwner current_owner;
    const CmItemMacroScopeSeed *initial_scope;
    size_t initial_scope_count;
    const CmItemMacroResolvedInvocation *resolved_invocations;
    size_t resolved_invocation_count;
    const CmItemMacroResolvedGeneratedPath *resolved_generated_paths;
    size_t resolved_generated_path_count;
    CmItemMacroGeneratedPathResolver resolve_generated_path;
    void *resolve_generated_path_context;
    int defer_source_invocations;
    CmMacroReparseOptions reparse;
    unsigned int maximum_nesting;
    size_t maximum_items;
    size_t maximum_expansions;
    size_t maximum_generated_bytes;
} CmItemMacroPlanOptions;

typedef struct CmItemMacroPlanNode {
    CmAstItemId item_id;
    CmAstSpan span;
    int is_generated;
    CmEffectiveAttribute *attributes;
    size_t attribute_count;
    CmEffectiveAttribute *inner_attributes;
    size_t inner_attribute_count;
    /* Outermost producing invocation whose span belongs to source input. */
    CmItemMacroItemRef source_invocation;
    /* Immediate producing invocation and definition, which may be generated. */
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    unsigned int expansion_depth;
    /* Lexical macro_rules scope at a source module declaration. */
    CmItemMacroItemRef *external_scope;
    size_t external_scope_count;
    CmExpandedChildKind child_kind;
    struct CmItemMacroPlanNode *children;
    size_t child_count;
} CmItemMacroPlanNode;

typedef struct CmItemMacroDeclaration {
    CmAstItemId item_id;
    /* Immediate inline-module owner, or NONE for this source unit's root. */
    CmAstItemId container_item;
    CmAstSpan span;
    CmAstMacroForm form;
    int is_generated;
    CmEffectiveAttribute *attributes;
    size_t attribute_count;
    CmItemMacroItemRef source_invocation;
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    unsigned int expansion_depth;
} CmItemMacroDeclaration;

typedef struct CmItemMacroExpansion {
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    CmAstItemId *generated_items;
    size_t generated_item_count;
} CmItemMacroExpansion;

typedef struct CmItemMacroPendingInvocation {
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef source_invocation;
    CmAstItemId container_item;
    CmAstSpan span;
    int is_generated;
    int is_qualified;
} CmItemMacroPendingInvocation;

typedef struct CmItemMacroPlan {
    CmItemMacroAstOwner owner;
    /* Distinguishes an active empty crate/module file from `#![cfg(false)]`. */
    int crate_is_active;
    /* Arrays are owned by the plan; metadata continues to borrow its AST. */
    CmEffectiveAttribute *crate_attributes;
    size_t crate_attribute_count;
    CmItemMacroPlanNode *roots;
    size_t root_count;
    CmAstItemId *root_items;
    size_t root_item_count;
    /* Named cfg-active macro declarations, separate from semantic roots. */
    CmItemMacroDeclaration *declarations;
    size_t declaration_count;
    CmItemMacroExpansion *expansions;
    size_t expansion_count;
    /* Unresolved invocations omitted only in explicit skeleton/defer mode. */
    CmItemMacroPendingInvocation *pending_invocations;
    size_t pending_invocation_count;
} CmItemMacroPlan;

typedef struct CmItemMacroPlanResult {
    CmMacroStatus status;
    CmItemMacroPlanStage stage;
    CmItemMacroPlanDiagnosticKind kind;
    const char *message;
    CmAstItemId item_id;
    CmItemMacroItemRef source_invocation;
    CmItemMacroItemRef definition;
    CmInternId macro_name;
    size_t items_visited;
    size_t expansions;
    size_t generated_bytes;
    CmMacroReparseResult reparse;
    CmExpandResult cfg;
} CmItemMacroPlanResult;

void cm_item_macro_plan_options_init(CmItemMacroPlanOptions *options,
    const CmCfgSet *cfg);
void cm_item_macro_plan_init(CmItemMacroPlan *plan);
void cm_item_macro_plan_destroy(CmItemMacroPlan *plan);

/*
 * Plans item-position macro expansion over an already cfg-active tree, using
 * options->cfg to reapply cfg and cfg_attr to every generated item sequence.
 * The cfg set must be the same explicit set used to construct `active`.
 * Named macro definitions and invocation items are consumed; named
 * definitions appear in the separate declaration array, while ordinary and
 * generated items appear in the owning output tree. Declarations retain
 * cfg-effective attributes and their immediate inline-module container.
 * Nodes own their effective attribute arrays (whose metadata bytes still
 * borrow `ast`) and
 * generated nodes record source-qualified producing invocation and
 * definition references and recursive expansion depth.  Source external
 * module nodes own the lexical macro_rules scope at their declaration.
 * Resolver-certified source invocation targets are source-qualified,
 * validated as exact macro declaration identities, and consumed exactly
 * once. Such a target can authorize an aliased, imported, or qualified macro
 * path; spelling and the planner's incomplete local stack never override it.
 * Separately validated generated-path targets may authorize only an exact
 * qualified path created during recursive expansion and may be reused. A
 * resolver callback can authenticate such a generated path when it first
 * appears, without pre-enumerating dependency definitions. It is never called
 * for source invocations. A
 * resolver may additionally certify the exact compiler-builtin cfg_select
 * declaration; no declarative macro is treated as that builtin by name alone.
 * Generated root IDs
 * are never appended to ast->root_items. Without an exact resolved target,
 * resolution is lexical and local only: options->initial_scope seeds textual
 * parent scope. There is no planner-owned import lookup, hygiene, proc-macro
 * lookup, or expression expansion.
 * When defer_source_invocations is set, unresolved source invocations are
 * recorded in pending_invocations and omitted from the semantic tree. Local
 * forward references and unresolved generated invocations still reject.
 * `plan` must be initialized before the call and remains empty on failure.
 * Planning is not transactional for `ast`: failed expansion can leave
 * unreachable generated or recovery nodes, although ast->root_items and all
 * public plan arrays remain unchanged/empty.
 */
CmItemMacroPlanResult cm_plan_item_macros(const CmExpandedAst *active,
    CmAst *ast, const CmItemMacroPlanOptions *options,
    CmItemMacroPlan *plan);

const char *cm_item_macro_plan_stage_name(CmItemMacroPlanStage stage);
const char *cm_item_macro_plan_diagnostic_kind_name(
    CmItemMacroPlanDiagnosticKind kind);

#endif
