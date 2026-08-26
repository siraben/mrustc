#ifndef CM_RESOLVE_IMPORTS_H
#define CM_RESOLVE_IMPORTS_H

#include "cm/resolve/module_graph.h"

typedef enum CmImportErrorKind {
    CM_IMPORT_ERROR_INVALID_ARGUMENT = 0,
    CM_IMPORT_ERROR_INVALID_TREE,
    CM_IMPORT_ERROR_UNRESOLVED,
    CM_IMPORT_ERROR_AMBIGUOUS,
    CM_IMPORT_ERROR_CYCLE
} CmImportErrorKind;

typedef enum CmResolvePrimitiveKind {
    CM_RESOLVE_PRIMITIVE_NONE = 0,
    CM_RESOLVE_PRIMITIVE_BOOL,
    CM_RESOLVE_PRIMITIVE_CHAR,
    CM_RESOLVE_PRIMITIVE_STR,
    CM_RESOLVE_PRIMITIVE_I8,
    CM_RESOLVE_PRIMITIVE_I16,
    CM_RESOLVE_PRIMITIVE_I32,
    CM_RESOLVE_PRIMITIVE_I64,
    CM_RESOLVE_PRIMITIVE_I128,
    CM_RESOLVE_PRIMITIVE_ISIZE,
    CM_RESOLVE_PRIMITIVE_U8,
    CM_RESOLVE_PRIMITIVE_U16,
    CM_RESOLVE_PRIMITIVE_U32,
    CM_RESOLVE_PRIMITIVE_U64,
    CM_RESOLVE_PRIMITIVE_U128,
    CM_RESOLVE_PRIMITIVE_USIZE,
    CM_RESOLVE_PRIMITIVE_F16,
    CM_RESOLVE_PRIMITIVE_F32,
    CM_RESOLVE_PRIMITIVE_F64,
    CM_RESOLVE_PRIMITIVE_F128
} CmResolvePrimitiveKind;

typedef struct CmResolvedBinding {
    CmModuleGraphRevision revision;
    CmModuleId module;
    CmResolveStringId name;
    CmResolveNamespace namespace_kind;
    /* Empty only for a synthetic alias of the crate-root module itself. */
    CmResolveItemRef declaration;
    /* Nonempty for an enum-variant binding; declaration remains the enum. */
    CmResolveVariantRef variant;
    /* Non-none only for a builtin primitive with no AST declaration. */
    CmResolvePrimitiveKind primitive_kind;
    CmAstItemKind item_kind;
    CmModuleId target_module;
    CmResolveItemRef import_declaration;
    int is_public;
    /* Reachable from every module in this crate, including `pub(crate)`. */
    int is_crate_visible;
    int is_import;
    int is_reexport;
    int is_ambiguous;
    /* True for a successfully resolved `as _` leaf with no published name. */
    int is_anonymous;
} CmResolvedBinding;

typedef struct CmImportError {
    CmImportErrorKind kind;
    CmModuleId module;
    CmResolveItemRef import_declaration;
    CmResolveStringId name;
    CmResolveStringId detail;
} CmImportError;

typedef struct CmImportResult {
    size_t binding_count;
    size_t error_count;
    CmModuleGraphRevision revision;
} CmImportResult;

typedef struct CmImportResolver {
    void *state;
} CmImportResolver;

typedef struct CmResolvePathSegmentView {
    const unsigned char *bytes;
    size_t length;
} CmResolvePathSegmentView;

typedef enum CmImportLookupStatus {
    CM_IMPORT_LOOKUP_OK = 0,
    CM_IMPORT_LOOKUP_NOT_FOUND,
    CM_IMPORT_LOOKUP_AMBIGUOUS,
    CM_IMPORT_LOOKUP_CYCLE,
    CM_IMPORT_LOOKUP_STALE_REVISION,
    CM_IMPORT_LOOKUP_FAILED_BUILD,
    CM_IMPORT_LOOKUP_INVALID
} CmImportLookupStatus;

/*
 * One parsed use-tree leaf. String IDs and segment views are owned by the
 * resolver and remain valid only until its next resolve call or destruction.
 * A leaf can be inspected even when local-crate resolution did not find a
 * binding; this is the structured boundary used by external-crate resolvers.
 */
typedef struct CmImportLeafView {
    CmModuleGraphRevision revision;
    CmModuleId module;
    CmResolveItemRef declaration;
    CmResolveStringId import_name;
    size_t segment_count;
    size_t binding_count;
    int absolute;
    int is_glob;
    int is_anonymous;
    int is_public;
    int is_crate_visible;
    int is_resolved;
    int saw_ambiguous;
} CmImportLeafView;

void cm_import_resolver_init(CmImportResolver *resolver);
void cm_import_resolver_destroy(CmImportResolver *resolver);
CmImportResult cm_import_resolve(CmImportResolver *resolver,
    const CmModuleGraph *graph, CmModuleGraphRevision expected_revision);

/*
 * A resolver revision is published only after resolution completes against a
 * successfully built, error-free graph whose current revision equals the
 * caller-supplied nonzero expected revision.  The graph revision and error
 * state are checked both before mirroring and immediately before publication.
 * Import errors remain revision-bound diagnostic results.  Every resolve
 * attempt first invalidates the previous revision.  `matches_graph`
 * additionally checks graph identity and its current revision; `graph` must
 * still be a live CmModuleGraph object.
 */
CmModuleGraphRevision cm_import_resolver_revision(
    const CmImportResolver *resolver);
/* Process-local object lifetime and monotonic resolve-attempt generation. */
uint64_t cm_import_resolver_lifetime_id(const CmImportResolver *resolver);
uint64_t cm_import_resolver_generation(const CmImportResolver *resolver);
/* Graph lifetime captured by the latest successful resolve; otherwise zero. */
uint64_t cm_import_resolver_graph_lifetime_id(
    const CmImportResolver *resolver);
int cm_import_resolver_matches_graph(const CmImportResolver *resolver,
    const CmModuleGraph *graph);

size_t cm_import_binding_count(const CmImportResolver *resolver,
    CmModuleId module, CmResolveNamespace namespace_kind);
int cm_import_get_binding(const CmImportResolver *resolver,
    CmModuleId module, CmResolveNamespace namespace_kind, uint32_t index,
    CmResolvedBinding *out_binding);

/*
 * Read the successfully resolved bindings produced by one source-qualified
 * import declaration.  Results are resolver-owned snapshots ordered first by
 * parsed use-tree leaf and then by that leaf's resolution order.  They include
 * anonymous (`as _`) bindings, glob-expanded bindings, and bindings that were
 * not published because a higher-priority destination binding shadowed them.
 * Repeated fixed-point attempts do not duplicate a leaf result.
 *
 * Returned bindings are stamped with the resolver's published revision and
 * remain valid only until the next cm_import_resolve call or destruction.
 */
size_t cm_import_declaration_binding_count(
    const CmImportResolver *resolver, CmModuleId module,
    CmResolveItemRef import_declaration);
int cm_import_get_declaration_binding(const CmImportResolver *resolver,
    CmModuleId module, CmResolveItemRef import_declaration, uint32_t index,
    CmResolvedBinding *out_binding);

/* Deterministic parsed leaf order across all modules and use declarations. */
size_t cm_import_leaf_count(const CmImportResolver *resolver);
int cm_import_get_leaf(const CmImportResolver *resolver, uint32_t index,
    CmImportLeafView *out_leaf);
int cm_import_get_leaf_segment(const CmImportResolver *resolver,
    uint32_t leaf_index, uint32_t segment_index,
    CmResolvePathSegmentView *out_segment);

/*
 * Resolve one already-parsed local-crate path through direct modules,
 * cfg-active enum variants, aliases, globs, reexports, and the exact
 * cfg-active root glob marked `#[prelude_import]`. Prelude bindings are a
 * lowest-priority fallback only for an unqualified first segment; ordinary
 * module bindings retain normal shadowing. Malformed, duplicate, nested,
 * public, or non-glob prelude imports fail closed during resolution. Segment
 * bytes are looked up in the resolver-owned interner and are never retained.
 * `absolute` starts at the crate root and does not require a valid `module`;
 * otherwise lookup starts at `module`. The final segment must name a binding,
 * so a path containing only `crate`, `self`, or `super` is invalid.
 * Extern-prelude lookup is intentionally outside this local resolver.
 *
 * Returned string IDs and bindings are owned by `resolver` and remain valid
 * only until the next cm_import_resolve call or resolver destruction.  The
 * source-qualified declaration itself remains meaningful only while its
 * owning syntax/source context is retained by the caller.  Each returned
 * binding is stamped with the resolver's published graph revision.  This
 * unchecked form reads the resolver snapshot even if that graph has since
 * changed; use cm_import_resolve_path_checked when a live graph is available.
 */
CmImportLookupStatus cm_import_resolve_path(
    const CmImportResolver *resolver, CmModuleId module, int absolute,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmResolveNamespace namespace_kind, CmResolvedBinding *out_binding);

/*
 * Revision-checked form of cm_import_resolve_path. `expected_revision` is the
 * caller's transaction revision, not an inferred current revision. It clears
 * `out_binding` and rejects stale resolver/graph snapshots or a failed graph
 * build before reading resolver-owned bindings.
 */
CmImportLookupStatus cm_import_resolve_path_checked(
    const CmImportResolver *resolver, const CmModuleGraph *graph,
    CmModuleGraphRevision expected_revision, CmModuleId module, int absolute,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmResolveNamespace namespace_kind, CmResolvedBinding *out_binding);

size_t cm_import_error_count(const CmImportResolver *resolver);
int cm_import_get_error(const CmImportResolver *resolver, uint32_t index,
    CmImportError *out_error);

size_t cm_import_string_length(const CmImportResolver *resolver,
    CmResolveStringId id);
int cm_import_copy_string(const CmImportResolver *resolver,
    CmResolveStringId id, char *buffer, size_t buffer_size);
const char *cm_import_error_kind_name(CmImportErrorKind kind);
const char *cm_import_lookup_status_name(CmImportLookupStatus status);

#endif
