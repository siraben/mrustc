#ifndef CMRUSTC_CM_HIR_LOWER_H
#define CMRUSTC_CM_HIR_LOWER_H

#include "cm/hir/library.h"
#include "cm/hir/model.h"
#include "cm/hir/module_map.h"
#include "cm/macro/expand.h"
#include "cm/resolve/imports.h"
#include "cm/syntax/ast.h"

typedef enum CmHirLowerPathUse {
    CM_HIR_LOWER_PATH_TYPE = 0,
    CM_HIR_LOWER_PATH_VISIBILITY
} CmHirLowerPathUse;

typedef enum CmHirLowerResolutionKind {
    /* The callback did not resolve this path. */
    CM_HIR_LOWER_UNRESOLVED = 0,
    /* Construct a named HIR type from definition and named_type_kind. */
    CM_HIR_LOWER_DEFINITION,
    /* Use an already-created type; generic path arguments must be absent. */
    CM_HIR_LOWER_EXISTING_TYPE,
    /* Construct a builtin primitive type without inventing a definition. */
    CM_HIR_LOWER_PRIMITIVE,
    /* Resolution itself failed, rather than finding no candidate. */
    CM_HIR_LOWER_RESOLVER_ERROR
} CmHirLowerResolutionKind;

typedef struct CmHirLowerResolution {
    CmHirLowerResolutionKind kind;
    CmHirDefId definition;
    CmHirTypeKind named_type_kind;
    CmHirTypeId existing_type;
    CmHirPrimitiveKind primitive_kind;
} CmHirLowerResolution;

/*
 * Local modules, local generic parameters, local ADTs, and primitives are
 * resolved by the lowerer first.  The callback is the explicit boundary for
 * extern-prelude and cross-crate definitions.
 */
typedef CmHirLowerResolution (*CmHirLowerResolvePath)(void *user_context,
    const CmAst *ast, CmAstPathId path, CmHirModuleId current_module,
    CmHirLowerPathUse use);

typedef enum CmHirLowerErrorKind {
    CM_HIR_LOWER_INVALID_ARGUMENT = 0,
    CM_HIR_LOWER_STALE_GRAPH,
    CM_HIR_LOWER_INVALID_AST,
    CM_HIR_LOWER_INACTIVE_CRATE,
    CM_HIR_LOWER_DUPLICATE_NAME,
    CM_HIR_LOWER_UNSUPPORTED_ITEM,
    CM_HIR_LOWER_UNSUPPORTED_TYPE,
    CM_HIR_LOWER_UNSUPPORTED_GENERIC,
    CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH,
    CM_HIR_LOWER_ALIAS_CYCLE,
    CM_HIR_LOWER_INVALID_ALIAS,
    CM_HIR_LOWER_INVALID_TRAIT,
    CM_HIR_LOWER_INVALID_IMPL,
    CM_HIR_LOWER_UNRESOLVED_PATH,
    CM_HIR_LOWER_WRONG_NAMESPACE,
    CM_HIR_LOWER_RESOLVER_FAILURE,
    CM_HIR_LOWER_HIR_FAILURE
} CmHirLowerErrorKind;

typedef struct CmHirLowerError {
    CmHirLowerErrorKind kind;
    CmSpan span;
    /* Optional second source location for pairwise validation failures. */
    CmSpan related_span;
    int has_related_span;
    CmAstItemId item;
    CmAstTypeId type;
    CmAstPathId path;
    CmHirStatus hir_status;
    char message[192];
} CmHirLowerError;

typedef struct CmHirLowerOptions {
    const char *crate_name;
    CmHirEdition edition;
    CmSourceId source;
    /*
     * Exact configured target pointer width. Zero means unavailable; only
     * target-dependent source forms reject when this authority is absent.
     */
    uint32_t pointer_bits;
    /* Borrowed for the synchronous lowering call; DefIds share `context`. */
    const CmHirLibraryArtifact *const *dependency_libraries;
    size_t dependency_library_count;
    CmHirLowerResolvePath resolve_path;
    void *resolve_context;
} CmHirLowerOptions;

typedef struct CmHirLowerResult {
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    size_t lowered_item_count;
    size_t error_count;
    CmHirLowerError first_error;
} CmHirLowerResult;

void cm_hir_lower_options_init(CmHirLowerOptions *options);

/*
 * Lower one already-parsed, cfg-active AST unit.  Unlowered body expression
 * IDs refer to this AST, so a caller that subsequently lowers bodies must
 * retain it until that work is complete.  On error, the context may contain
 * reserved or partially lowered declarations and should be discarded.
 */
CmHirLowerResult cm_hir_lower_crate(CmHirContext *context, const CmAst *ast,
    const CmHirLowerOptions *options);

/* Lower only declarations present in a successful cfg-derived view. */
CmHirLowerResult cm_hir_lower_expanded_crate(CmHirContext *context,
    const CmAst *ast, const CmExpandedAst *expanded,
    const CmHirLowerOptions *options);

/*
 * Lower one complete resolver module graph into one HIR crate.  The caller
 * owns `modules`, which must be initialized and empty.  Successful lowering
 * binds every graph module exactly once.  The graph owns all borrowed ASTs.
 *
 * This entry point consumes the graph revision's effective-item views.  It
 * does not perform macro expansion. Effective crate/module inner attributes
 * and attributes owned by structural HIR declarations are copied into HIR.
 * Module declaration attributes are owned by the mapped child module rather
 * than a duplicate HIR item.
 * `imports` must be a successfully published resolver snapshot for exactly
 * `graph` and `revision`. It is authoritative for local-crate namespaces,
 * including direct declarations, aliases, and reexports. Resolver errors are
 * rejected except for exact unresolved leaves authenticated by the graph as
 * dependency-macro-only imports that produced published generated syntax.
 * Fully qualified dependency ADT, type-alias, and extern-type paths may be
 * resolved from `options->dependency_libraries`. Every artifact must share
 * `context`, and extern names must be unique. Local definitions take
 * precedence; the legacy path callback remains the final fallback.
 * Each `use` declaration is retained in declaration order as module-owned
 * structural metadata: its raw tree, declared visibility, effective outer
 * attributes, and resolver-produced namespace bindings. This does not create
 * a definition or HIR item for the import, nor does retaining the record add
 * semantic import effects beyond the resolver used by path lowering.
 * Source-written and authenticated macro-generated root consts, bounded
 * immutable source-written statics, and non-default, immutable, explicitly
 * typed, initializer-bearing inherent associated consts and trait-impl
 * associated const definitions retain their graph-owned initializer as an
 * unlowered body. Trait-impl consts link the exact targetless trait declaration.
 * Simple resolved const names in array
 * lengths retain the referenced DefId as an unevaluated HIR const. Inherent
 * impl headers retain their real self type without inventing a trait identity.
 * Immutable, explicitly typed, targetless trait associated const declarations
 * retain their child DefId and trait-owned `Self` type; optional defaults
 * retain an explicit declaration promise independently of an optional
 * declaration-owned unlowered body. Trait methods use the same promise/body
 * split. Mutable or generated statics
 * remain rejected. Direct, attribute-free
 * trait/impl methods retain source-
 * qualified unlowered body identities; nested effective cfg/attribute views
 * are not yet available and therefore hard-error.
 * Precondition rejection leaves caller state unchanged. Once the documented
 * empty-map precondition holds and mutation begins, lowering is transactional:
 * on every error, `modules` is empty and the HIR context is rewound to its
 * exact pre-call semantic contents.
 */
CmHirLowerResult cm_hir_lower_module_graph(CmHirContext *context,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, CmHirModuleMap *modules,
    const CmHirLowerOptions *options);

const char *cm_hir_lower_error_kind_name(CmHirLowerErrorKind kind);

#endif
