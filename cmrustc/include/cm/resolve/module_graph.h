#ifndef CM_RESOLVE_MODULE_GRAPH_H
#define CM_RESOLVE_MODULE_GRAPH_H

#include "cm/source.h"
#include "cm/macro/item_macro_plan.h"
#include "cm/syntax/ast.h"
#include "cm/syntax/token.h"

struct CmDependencyMacroArtifact;

typedef uint32_t CmModuleId;
typedef uint32_t CmResolveStringId;
typedef uint32_t CmResolveEffectiveItemId;
typedef uint32_t CmResolveDependencyId;
typedef uint32_t CmResolveDependencyCertificateId;
typedef uint64_t CmModuleGraphRevision;
typedef struct CmModuleGraph CmModuleGraph;

#define CM_MODULE_NONE ((CmModuleId)0)
#define CM_RESOLVE_STRING_NONE ((CmResolveStringId)0)
#define CM_RESOLVE_EFFECTIVE_ITEM_NONE ((CmResolveEffectiveItemId)0)
#define CM_RESOLVE_DEPENDENCY_NONE ((CmResolveDependencyId)0)
#define CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE \
    ((CmResolveDependencyCertificateId)0)
#define CM_MODULE_GRAPH_REVISION_NONE ((CmModuleGraphRevision)0)

typedef enum CmResolveNamespace {
    CM_RESOLVE_NAMESPACE_TYPE = 0,
    CM_RESOLVE_NAMESPACE_VALUE,
    CM_RESOLVE_NAMESPACE_MACRO
} CmResolveNamespace;

typedef enum CmResolveCfgStatus {
    CM_RESOLVE_CFG_UNKNOWN = 0,
    CM_RESOLVE_CFG_ENABLED,
    CM_RESOLVE_CFG_DISABLED
} CmResolveCfgStatus;

typedef enum CmResolveErrorKind {
    CM_RESOLVE_ERROR_INVALID_ARGUMENT = 0,
    CM_RESOLVE_ERROR_PARSE,
    CM_RESOLVE_ERROR_SOURCE_IO,
    CM_RESOLVE_ERROR_MISSING_MODULE_FILE,
    CM_RESOLVE_ERROR_AMBIGUOUS_MODULE_FILE,
    CM_RESOLVE_ERROR_DUPLICATE_MODULE_PATH,
    CM_RESOLVE_ERROR_MODULE_CYCLE,
    CM_RESOLVE_ERROR_CFG_UNKNOWN,
    CM_RESOLVE_ERROR_REVISION_EXHAUSTED,
    CM_RESOLVE_ERROR_CFG_EXPANSION,
    CM_RESOLVE_ERROR_ITEM_MACRO,
    CM_RESOLVE_ERROR_UNSUPPORTED_GENERATED_ITEM,
    CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
    CM_RESOLVE_ERROR_INCLUDE_CYCLE,
    CM_RESOLVE_ERROR_INCLUDE_LIMIT
} CmResolveErrorKind;

typedef struct CmResolveItemRef {
    CmSourceId source;
    CmAstItemId item;
} CmResolveItemRef;

/* Exact source identity of one enum variant; index is zero-based in the AST. */
typedef struct CmResolveVariantRef {
    CmResolveItemRef enumeration;
    uint32_t index;
} CmResolveVariantRef;

/*
 * A dependency declaration in its own source-ID namespace. consumer_graph,
 * consumer_revision, certificate, and dependency form an opaque identity
 * local to one successful consumer graph revision; the source and item pair
 * belongs only to that dependency at dependency_revision. The graph pointer
 * is an identity token only and expires with the consumer graph.
 */
typedef struct CmResolveDependencyItemRef {
    const CmModuleGraph *consumer_graph;
    CmModuleGraphRevision consumer_revision;
    CmResolveDependencyCertificateId certificate;
    CmResolveDependencyId dependency;
    CmModuleGraphRevision dependency_revision;
    CmResolveItemRef declaration;
} CmResolveDependencyItemRef;

typedef enum CmResolveViewStatus {
    CM_RESOLVE_VIEW_OK = 0,
    CM_RESOLVE_VIEW_INVALID_ARGUMENT,
    CM_RESOLVE_VIEW_STALE_REVISION,
    CM_RESOLVE_VIEW_FAILED_BUILD,
    CM_RESOLVE_VIEW_INVALID_MODULE,
    CM_RESOLVE_VIEW_OUT_OF_RANGE,
    CM_RESOLVE_VIEW_NOT_FOUND
} CmResolveViewStatus;

/*
 * Source items set source_item and leave the macro fields empty. Generated
 * items leave source_item empty and record their immediate producing
 * invocation, exactly one local or dependency definition, and expansion
 * depth. A dependency declaration's source ID is never copied into
 * macro_definition. Their diagnostic span is the outermost source-backed
 * invocation anchor, not a synthetic reparse offset.
 */
typedef struct CmResolveItemProvenance {
    CmResolveItemRef source_item;
    CmResolveItemRef macro_invocation;
    CmResolveItemRef macro_definition;
    CmResolveDependencyItemRef dependency_macro_definition;
    uint32_t expansion_depth;
} CmResolveItemProvenance;

typedef struct CmResolveEffectiveItem {
    CmResolveItemRef declaration;
    CmResolveItemProvenance provenance;
    CmSpan span;
    CmAstItemKind item_kind;
    CmAstVisibilityKind visibility;
    uint32_t attribute_count;
    int is_generated;
    /* Opaque identity local to one graph revision and module. */
    CmResolveEffectiveItemId id;
    CmExpandedChildKind child_kind;
    uint32_t child_count;
    /* Cfg-active variants, meaningful only for CM_AST_ITEM_ENUM. */
    uint32_t variant_count;
} CmResolveEffectiveItem;

typedef struct CmResolveEffectiveVariant {
    CmResolveVariantRef declaration;
    CmResolveStringId name;
    CmAstFieldForm form;
    uint32_t field_count;
    uint32_t attribute_count;
    CmSpan span;
    int is_generated;
} CmResolveEffectiveVariant;

typedef struct CmResolveEffectiveAttribute {
    CmSourceId source;
    CmAstAttributeId source_attribute;
    /* Owning item, or empty only for crate-root inner attributes. */
    CmResolveItemRef owner;
    /* Nonempty only when this effective attribute belongs to an enum variant. */
    CmResolveVariantRef owner_variant;
    CmAstAttributeStyle style;
    CmSpan span;
    /* Effective metadata body without `#[ ]`, copied by the graph. */
    CmResolveStringId metadata;
    uint32_t expansion_depth;
} CmResolveEffectiveAttribute;

typedef struct CmResolveNamespaceEntry {
    CmResolveStringId name;
    CmResolveItemRef declaration;
    CmAstItemKind item_kind;
    CmAstVisibilityKind visibility;
} CmResolveNamespaceEntry;

typedef struct CmResolveMacroDeclaration {
    CmResolveStringId name;
    CmResolveItemRef declaration;
    CmModuleId owner_module;
    CmAstMacroForm form;
    CmAstVisibilityKind visibility;
    CmSpan span;
    CmResolveItemProvenance provenance;
    uint32_t attribute_count;
    int is_generated;
} CmResolveMacroDeclaration;

typedef struct CmResolveMacroScopeEntry {
    CmResolveStringId name;
    CmResolveItemRef declaration;
    /* The declaration itself, or the `mod` item which imported it. */
    CmResolveItemRef introduced_by;
    CmSpan introduction_span;
    CmAstMacroForm form;
    int is_macro_use;
} CmResolveMacroScopeEntry;

typedef struct CmResolveImport {
    CmResolveItemRef declaration;
    CmResolveStringId tree;
    CmAstVisibilityKind visibility;
} CmResolveImport;

typedef struct CmResolveModuleInfo {
    CmModuleId id;
    CmModuleId parent;
    /* The parent-source `mod` item which declared this module; none at root. */
    CmResolveItemRef declaration;
    CmSourceId source;
    CmResolveStringId name;
    CmResolveStringId absolute_path;
    CmResolveStringId source_path;
    int is_inline;
    uint32_t child_count;
    uint32_t type_count;
    uint32_t value_count;
    uint32_t macro_count;
    uint32_t macro_scope_count;
    uint32_t import_count;
    uint32_t active_item_count;
    uint32_t effective_item_count;
    uint32_t inner_attribute_count;
} CmResolveModuleInfo;

typedef struct CmResolveError {
    CmResolveErrorKind kind;
    CmSpan span;
    CmResolveStringId module_path;
    CmResolveStringId detail_a;
    CmResolveStringId detail_b;
    uint32_t line;
    uint32_t column;
} CmResolveError;

typedef enum CmIncludeExpansionMode {
    CM_INCLUDE_EXPANSION_DISABLED = 0,
    /* Expand only resolver-certified rustc builtin include declarations. */
    CM_INCLUDE_EXPANSION_AUTHENTICATED,
    /*
     * Test-only source adapter. The caller asserts that no inherited macro
     * scope exists; this mode does not authenticate Rust's builtin include.
     */
    CM_INCLUDE_EXPANSION_SOURCE_FIXTURE
} CmIncludeExpansionMode;

typedef struct CmModuleGraphOptions {
    enum cm_edition edition;
    /* Borrowed only for the duration of cm_module_graph_build. */
    const CmCfgSet *cfg;
    CmIncludeExpansionMode include_expansion;
    /* Borrowed only for the synchronous build; artifacts must be live. */
    const struct CmDependencyMacroArtifact *const *dependency_macros;
    size_t dependency_macro_count;
} CmModuleGraphOptions;

typedef struct CmModuleGraphResult {
    CmModuleId root;
    size_t error_count;
    CmModuleGraphRevision revision;
} CmModuleGraphResult;

/* Opaque resolver-owned state; callers retain ownership of CmSourceSet. */
struct CmModuleGraph {
    void *state;
};

void cm_module_graph_options_init(CmModuleGraphOptions *options);
void cm_module_graph_init(CmModuleGraph *graph);
void cm_module_graph_destroy(CmModuleGraph *graph);

/*
 * Builds from an already-loaded root. `options` and options->cfg are required;
 * the cfg set is the single authority for source and generated items.
 * External module files are appended to sources, but the graph retains
 * neither the source set nor the cfg set after the synchronous build.
 * The default authenticated mode resolves each source invocation to one exact
 * cfg-active macro declaration, requires one bare `rustc_builtin_macro`
 * attribute on the exact `macro_rules! include` declaration, and only then
 * opens and splices a simple relative item-list file. Discovery is staged:
 * non-`macro_use` external modules are deferred until pending root calls have
 * exact targets, and every successful splice discards derived graph state and
 * replans from the retained source-qualified ASTs. A simple `#[path = "..."]`
 * on an external module is honored so declaration-ordered `macro_use` scope
 * can be completed without guessing a default file.
 *
 * CM_INCLUDE_EXPANSION_SOURCE_FIXTURE appends exact item-position
 * `include!("path.rs");` files and splices their items in lexical order with
 * distinct source-qualified provenance. It is a fixture adapter, not Rust
 * macro resolution. The bounded adapter accepts only a simple,
 * unescaped ordinary string literal and rejects attributed/qualified/
 * generated forms, ambiguous macro scope, cycles, resource-limit exhaustion,
 * included inner attributes, and included macro/module/use/extern items.
 * Each external file is parsed independently, then planned once with the
 * parent textual macro_rules scope captured at its declaring `mod` item.
 * Imported bindings are not added to that lexical scope. During staging,
 * qualified source invocations can use only an exact public local-crate macro
 * binding resolved through the graph's import model; unresolved, ambiguous,
 * private, extern-prelude, and generated qualified paths fail closed.
 */
CmModuleGraphResult cm_module_graph_build(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root,
    const CmModuleGraphOptions *options);

size_t cm_module_graph_module_count(const CmModuleGraph *graph);
size_t cm_module_graph_error_count(const CmModuleGraph *graph);
/* Process-unique identity of this init/destroy lifetime; zero if not live. */
uint64_t cm_module_graph_lifetime_id(const CmModuleGraph *graph);
CmModuleGraphRevision cm_module_graph_revision(const CmModuleGraph *graph);
/* Deterministic graph-owned module order; index is zero-based. */
int cm_module_graph_get_module_at(const CmModuleGraph *graph, size_t index,
    CmResolveModuleInfo *out_module);
/* Succeeds only when the graph contains exactly one parentless module. */
int cm_module_graph_get_root(const CmModuleGraph *graph,
    CmModuleId *out_root);
int cm_module_graph_get_module(const CmModuleGraph *graph, CmModuleId id,
    CmResolveModuleInfo *out_module);
int cm_module_graph_get_child(const CmModuleGraph *graph, CmModuleId module,
    uint32_t index, CmModuleId *out_child);
/* Semantic payload accessors fail when the most recent graph build failed. */
int cm_module_graph_get_namespace_entry(const CmModuleGraph *graph,
    CmModuleId module, CmResolveNamespace namespace_kind, uint32_t index,
    CmResolveNamespaceEntry *out_entry);
int cm_module_graph_get_macro_scope_entry(const CmModuleGraph *graph,
    CmModuleId module, uint32_t index, CmResolveMacroScopeEntry *out_entry);
int cm_module_graph_get_import(const CmModuleGraph *graph, CmModuleId module,
    uint32_t index, CmResolveImport *out_import);
int cm_module_graph_get_error(const CmModuleGraph *graph, uint32_t index,
    CmResolveError *out_error);

/*
 * Read-only views into graph-owned parsed syntax.  The AST and item list are
 * valid until the next build on `graph` or graph destruction.  Root and
 * external modules expose their parsed crate-root items; inline modules
 * expose only the items directly inside that module.  No view depends on the
 * CmSourceSet used to build the graph.
 */
int cm_module_graph_borrow_ast(const CmModuleGraph *graph, CmModuleId module,
    const CmAst **out_ast);
/*
 * Authenticate one source-qualified declaration in a module's syntax unit.
 * This includes declarations spliced from an item-position `include!`, whose
 * source differs from the module root while their AST storage is shared.
 */
int cm_module_graph_borrow_item_ast(const CmModuleGraph *graph,
    CmModuleId module, CmResolveItemRef declaration,
    const CmAst **out_ast);
int cm_module_graph_borrow_items(const CmModuleGraph *graph,
    CmModuleId module, const CmAstItemId **out_items, uint32_t *out_count);

/*
 * Direct effective items after cfg and supported item-macro expansion, in
 * order.  References are source-qualified because external modules have
 * independent AST ID spaces. Returns false after a failed graph build.
 */
int cm_module_graph_borrow_active_items(const CmModuleGraph *graph,
    CmModuleId module, const CmResolveItemRef **out_items,
    uint32_t *out_count);

/*
 * Revision-checked, graph-owned effective item views.  They reflect the cfg,
 * cfg_attr, and supported item-macro decisions used to construct namespaces.
 * Attribute metadata is the effective body without `#[ ]`.
 * `(span.source, source_attribute)` is the source-qualified attribute
 * identity. Until generated source maps exist, generated item and attribute
 * spans use the outermost source-backed producing invocation as a coarse
 * diagnostic anchor; they never expose synthetic generated-buffer offsets.
 * Crate inner attributes have an empty owner; module inner attributes name
 * the source-qualified parent declaration, including for external files.
 * Returned structs own no storage and remain meaningful only for the supplied
 * graph revision.
 */
CmResolveViewStatus cm_module_graph_get_effective_item(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t index, CmResolveEffectiveItem *out_item);
/*
 * Looks up one cfg-active direct child of a trait or impl effective item.
 * Effective module contents remain roots of their own CmModuleId and are not
 * duplicated here. Item IDs are opaque and valid only with the graph,
 * revision, and module which returned them.
 */
CmResolveViewStatus cm_module_graph_get_effective_child(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId parent,
    uint32_t child_index, CmResolveEffectiveItem *out_item);
/* Looks up a cfg-active variant by effective order while retaining AST index. */
CmResolveViewStatus cm_module_graph_get_effective_variant(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId enumeration,
    uint32_t variant_index, CmResolveEffectiveVariant *out_variant);
CmResolveViewStatus cm_module_graph_get_effective_variant_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId enumeration,
    uint32_t variant_index, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute);
CmResolveViewStatus cm_module_graph_get_effective_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t item_index, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute);
/* Attribute lookup for either a module root or a recursive child item. */
CmResolveViewStatus cm_module_graph_get_effective_item_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveEffectiveItemId item,
    uint32_t attribute_index, CmResolveEffectiveAttribute *out_attribute);
CmResolveViewStatus cm_module_graph_get_effective_inner_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute);

/*
 * Looks up one cfg-active named macro declaration by its source-qualified
 * identity. Definitions remain absent from ordinary effective item roots.
 * The owner is the declaration's exact source root or inline module, before
 * macro_export or macro_use introduces any additional namespace binding.
 */
CmResolveViewStatus cm_module_graph_get_macro_declaration(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveItemRef declaration, CmResolveMacroDeclaration *out_declaration);
CmResolveViewStatus cm_module_graph_get_macro_declaration_attribute(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveItemRef declaration, uint32_t attribute_index,
    CmResolveEffectiveAttribute *out_attribute);

/*
 * Authenticates an opaque dependency macro reference returned by this exact
 * graph instance and successful consumer revision. Structural nonzero fields
 * are insufficient: the dependency and declaration must match the graph's
 * final published certified registry. This validates the consumer-owned
 * snapshot; the dependency graph and build artifact need not remain live.
 */
CmResolveViewStatus
cm_module_graph_validate_dependency_macro_definition(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmResolveDependencyItemRef reference);

/*
 * Authenticates one unresolved consumer import leaf that was the exact,
 * unqualified source binding for a published dependency-macro expansion.
 * This does not admit other unresolved leaves from the same use declaration.
 */
CmResolveViewStatus cm_module_graph_validate_dependency_macro_import(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, CmResolveItemRef import_declaration,
    const unsigned char *local_name, size_t local_name_length);

size_t cm_module_graph_string_length(const CmModuleGraph *graph,
    CmResolveStringId id);
int cm_module_graph_copy_string(const CmModuleGraph *graph,
    CmResolveStringId id, char *buffer, size_t buffer_size);
const char *cm_resolve_error_kind_name(CmResolveErrorKind kind);
const char *cm_resolve_view_status_name(CmResolveViewStatus status);

#endif
