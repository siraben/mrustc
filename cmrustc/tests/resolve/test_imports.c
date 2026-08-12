#include "cm/driver/cfg.h"
#include "cm/resolve/imports.h"
#include "cm/resolve/dependency_macro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-imports: %s\n", message);
        return 0;
    }
    return 1;
}

static CmModuleGraphResult build_graph_with_empty_cfg(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root)
{
    CmCfgSet cfg;
    CmModuleGraphOptions options;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    return cm_module_graph_build(graph, sources, root, &options);
}

static int import_string_equals(const CmImportResolver *resolver,
    CmResolveStringId id, const char *expected)
{
    char buffer[128];

    return cm_import_copy_string(resolver, id, buffer, sizeof(buffer)) &&
        strcmp(buffer, expected) == 0;
}

static int graph_string_equals(const CmModuleGraph *graph,
    CmResolveStringId id, const char *expected)
{
    char buffer[128];

    return cm_module_graph_copy_string(graph, id, buffer, sizeof(buffer)) &&
        strcmp(buffer, expected) == 0;
}

static int item_ref_equals(CmResolveItemRef left, CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static CmModuleId find_module(const CmModuleGraph *graph,
    const char *absolute_path)
{
    size_t count;
    CmModuleId id;

    count = cm_module_graph_module_count(graph);
    for (id = 1u; (size_t)id <= count; ++id) {
        CmResolveModuleInfo module;

        if (cm_module_graph_get_module(graph, id, &module) &&
            graph_string_equals(graph, module.absolute_path, absolute_path))
            return id;
    }
    return CM_MODULE_NONE;
}

static int find_binding(const CmImportResolver *resolver, CmModuleId module,
    CmResolveNamespace namespace_kind, const char *name,
    CmResolvedBinding *out_binding)
{
    size_t count;
    uint32_t index;

    count = cm_import_binding_count(resolver, module, namespace_kind);
    for (index = 0u; (size_t)index < count; ++index) {
        CmResolvedBinding binding;

        if (cm_import_get_binding(resolver, module, namespace_kind, index,
            &binding) && import_string_equals(resolver, binding.name, name)) {
            if (out_binding != NULL) *out_binding = binding;
            return 1;
        }
    }
    return 0;
}

static int find_macro_scope_entry(const CmModuleGraph *graph,
    CmModuleId module, const char *name, CmResolveMacroScopeEntry *out_entry)
{
    CmResolveModuleInfo information;
    uint32_t index;

    if (out_entry != NULL) memset(out_entry, 0, sizeof(*out_entry));
    if (out_entry == NULL
        || !cm_module_graph_get_module(graph, module, &information)) return 0;
    for (index = 0u; index < information.macro_scope_count; ++index) {
        CmResolveMacroScopeEntry entry;

        if (cm_module_graph_get_macro_scope_entry(graph, module, index,
                &entry) && graph_string_equals(graph, entry.name, name)) {
            *out_entry = entry;
            return 1;
        }
    }
    return 0;
}

static CmImportLookupStatus lookup_path(const CmImportResolver *resolver,
    CmModuleId module, int absolute, CmResolveNamespace namespace_kind,
    const char *const *parts, size_t part_count,
    CmResolvedBinding *out_binding)
{
    CmResolvePathSegmentView segments[8];
    size_t index;

    if (part_count > sizeof(segments) / sizeof(segments[0]))
        return CM_IMPORT_LOOKUP_INVALID;
    for (index = 0u; index < part_count; ++index) {
        segments[index].bytes = (const unsigned char *)parts[index];
        segments[index].length = strlen(parts[index]);
    }
    return cm_import_resolve_path(resolver, module, absolute, segments,
        part_count, namespace_kind, out_binding);
}

static CmImportLookupStatus lookup_path_checked(
    const CmImportResolver *resolver, const CmModuleGraph *graph,
    CmModuleGraphRevision expected_revision, CmModuleId module, int absolute,
    CmResolveNamespace namespace_kind,
    const char *const *parts, size_t part_count,
    CmResolvedBinding *out_binding)
{
    CmResolvePathSegmentView segments[8];
    size_t index;

    if (part_count > sizeof(segments) / sizeof(segments[0]))
        return CM_IMPORT_LOOKUP_INVALID;
    for (index = 0u; index < part_count; ++index) {
        segments[index].bytes = (const unsigned char *)parts[index];
        segments[index].length = strlen(parts[index]);
    }
    return cm_import_resolve_path_checked(resolver, graph, expected_revision,
        module, absolute, segments, part_count, namespace_kind, out_binding);
}

static int load_and_resolve(const char *path, CmSourceSet *sources,
    CmModuleGraph *graph, CmImportResolver *resolver,
    CmModuleGraphResult *graph_result, CmImportResult *import_result)
{
    CmSourceId root;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    cm_import_resolver_init(resolver);
    if (cm_source_load_file(sources, path, &root) != CM_SOURCE_OK) return 0;
    *graph_result = build_graph_with_empty_cfg(graph, sources, root);
    *import_result = cm_import_resolve(resolver, graph,
        graph_result->revision);
    return 1;
}

static int load_memory_and_resolve(const char *path, const char *text,
    CmSourceSet *sources, CmModuleGraph *graph, CmImportResolver *resolver,
    CmModuleGraphResult *graph_result, CmImportResult *import_result)
{
    CmSourceId root;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    cm_import_resolver_init(resolver);
    if (cm_source_add_memory(sources, path,
            (const unsigned char *)text, strlen(text), &root)
        != CM_SOURCE_OK) {
        return 0;
    }
    *graph_result = build_graph_with_empty_cfg(graph, sources, root);
    *import_result = cm_import_resolve(resolver, graph,
        graph_result->revision);
    return 1;
}

static void destroy_all(CmSourceSet *sources, CmModuleGraph *graph,
    CmImportResolver *resolver)
{
    cm_import_resolver_destroy(resolver);
    cm_module_graph_destroy(graph);
    cm_source_set_destroy(sources);
}

static int test_core_target_cfg_import_frontier(const char *core_root)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphOptions options;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmCfgSet cfg;
    CmSourceId root;
    char path[4096];
    size_t unresolved;
    size_t ambiguous;
    size_t cycles;
    size_t index;
    int ok;

    if (core_root == NULL || core_root[0] == 0) return 1;
    if (snprintf(path, sizeof(path), "%s/src/lib.rs", core_root) < 0
        || strlen(path) + 1u >= sizeof(path)) {
        return check(0, "target-configured core path is too long");
    }
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_import_resolver_init(&resolver);
    ok = check(cm_source_load_file(&sources, path, &root) == CM_SOURCE_OK,
        "failed to load target-configured Rust 1.90 core root");
    cm_module_graph_options_init(&options);
    ok &= check(cm_target_cfg_set(&cfg,
            cm_target_find("x86_64-unknown-linux-gnu")),
        "failed to construct canonical x86-64 target cfg facts");
    options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root, &options);
    import_result = cm_import_resolve(&resolver, &graph,
        graph_result.revision);
    unresolved = 0u;
    ambiguous = 0u;
    cycles = 0u;
    for (index = 0u; index < import_result.error_count; ++index) {
        CmImportError error;

        memset(&error, 0, sizeof(error));
        if (index > (size_t)UINT32_MAX
            || !cm_import_get_error(&resolver, (uint32_t)index, &error)) {
            ok = 0;
            break;
        }
        if (error.kind == CM_IMPORT_ERROR_UNRESOLVED) {
            unresolved += 1u;
        } else if (error.kind == CM_IMPORT_ERROR_AMBIGUOUS) {
            ambiguous += 1u;
        } else if (error.kind == CM_IMPORT_ERROR_CYCLE) {
            cycles += 1u;
        }
    }
    if (graph_result.error_count != 0u || import_result.error_count != 0u
        || unresolved != 0u || ambiguous != 0u || cycles != 0u) {
        fprintf(stderr, "target-configured core imports actual: graph=%lu "
            "imports=%lu unresolved=%lu ambiguous=%lu cycles=%lu\n",
            (unsigned long)graph_result.error_count,
            (unsigned long)import_result.error_count,
            (unsigned long)unresolved, (unsigned long)ambiguous,
            (unsigned long)cycles);
    }
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u && unresolved == 0u
        && ambiguous == 0u && cycles == 0u,
        "target-configured Rust 1.90 core import frontier changed");
    cm_import_resolver_destroy(&resolver);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_extern_crate_self_alias(void)
{
    static const char source[] =
        "extern crate self as core;\n"
        "pub mod marker { pub struct PhantomData; }\n"
        "mod nested { use core::marker::PhantomData as Data; }\n";
    const char *core_path[] = { "core" };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    CmModuleId nested;
    int ok;

    ok = check(load_memory_and_resolve("self-alias/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load self-crate alias fixture");
    if (!ok) return 0;
    nested = find_module(&graph, "crate::nested");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u && nested != CM_MODULE_NONE,
        "self-crate alias fixture did not resolve");
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "core", &binding)
        && binding.item_kind == CM_AST_ITEM_EXTERN_CRATE
        && binding.target_module == graph_result.root
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "self", NULL),
        "self-crate declaration did not publish its alias");
    ok &= check(find_binding(&resolver, nested, CM_RESOLVE_NAMESPACE_TYPE,
            "Data", &binding)
        && binding.item_kind == CM_AST_ITEM_STRUCT,
        "nested import did not traverse the self-crate alias");
    ok &= check(lookup_path(&resolver, nested, 0,
            CM_RESOLVE_NAMESPACE_TYPE, core_path, 1u, &binding)
            == CM_IMPORT_LOOKUP_OK
        && binding.item_kind == CM_AST_ITEM_EXTERN_CRATE
        && binding.target_module == graph_result.root,
        "self-crate alias was absent from nested extern-prelude lookup");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_success(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    CmResolvedBinding direct_thing;
    CmModuleId root;
    CmModuleId a;
    CmModuleId inner;
    const char *crate_thing[] = { "crate", "a", "Thing" };
    const char *nested_deep[] = { "a", "nested", "Deep" };
    const char *reexported[] = { "b", "ReThing" };
    const char *from_inner[] = { "super", "a", "Thing" };
    const char *alias[] = { "Alias" };
    const char *missing[] = { "a", "Missing" };
    int ok;

    ok = check(load_and_resolve("tests/resolve/fixtures/imports/lib.rs",
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load success fixture");
    if (!ok) return 0;
    root = graph_result.root;
    a = find_module(&graph, "crate::a");
    inner = find_module(&graph, "crate::inner");
    ok &= check(graph_result.error_count == 0u &&
        import_result.error_count == 0u, "success fixture has errors");
    ok &= check(graph_result.revision != CM_MODULE_GRAPH_REVISION_NONE &&
        import_result.revision == graph_result.revision &&
        cm_import_resolver_revision(&resolver) == graph_result.revision &&
        cm_import_resolver_matches_graph(&resolver, &graph),
        "successful import resolution did not publish the graph revision");
    ok &= check(a != CM_MODULE_NONE && inner != CM_MODULE_NONE,
        "fixture module paths were not found");
    ok &= check(find_binding(&resolver, a, CM_RESOLVE_NAMESPACE_TYPE,
        "Thing", &direct_thing),
        "direct target binding was not found");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
        "Alias", &binding) && binding.is_import &&
        binding.declaration.source != 0u &&
        binding.revision == graph_result.revision,
        "crate path or alias did not resolve in the type namespace");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_VALUE,
        "value", NULL), "function did not resolve in the value namespace");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
        "Pair", NULL) && find_binding(&resolver, root,
        CM_RESOLVE_NAMESPACE_VALUE, "Pair", NULL),
        "tuple struct did not resolve in both namespaces");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
        "Deep", NULL), "nested use tree did not resolve");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
        "AbsoluteThing", NULL), "absolute path did not resolve");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_VALUE,
        "FLAG", NULL) && find_binding(&resolver, root,
        CM_RESOLVE_NAMESPACE_TYPE, "ReThing", NULL),
        "glob or chained public reexport did not resolve");
    ok &= check(find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
        "Public", &binding) && binding.is_public && binding.is_reexport,
        "public use was not marked as a reexport");
    ok &= check(find_binding(&resolver, inner, CM_RESOLVE_NAMESPACE_TYPE,
        "SuperThing", &binding) && binding.is_public,
        "super path in an inline module did not resolve");
    ok &= check(cm_import_binding_count(&resolver, root,
        CM_RESOLVE_NAMESPACE_MACRO) == 0u,
        "macro namespace was not kept independent");
    ok &= check(lookup_path(&resolver, root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, crate_thing, 3u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.item_kind == CM_AST_ITEM_STRUCT,
        "crate-qualified lookup did not resolve a direct item");
    ok &= check(lookup_path(&resolver, root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, nested_deep, 3u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.item_kind == CM_AST_ITEM_STRUCT,
        "multi-module lookup did not resolve a nested item");
    ok &= check(lookup_path(&resolver, root, 1,
        CM_RESOLVE_NAMESPACE_TYPE, reexported, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.is_reexport,
        "absolute lookup did not follow a reexport");
    ok &= check(lookup_path(&resolver, inner, 0,
        CM_RESOLVE_NAMESPACE_TYPE, from_inner, 3u, &binding)
        == CM_IMPORT_LOOKUP_OK,
        "super-qualified lookup did not traverse from an inline module");
    ok &= check(lookup_path(&resolver, root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.is_import
        && binding.revision == graph_result.revision
        && binding.declaration.source == direct_thing.declaration.source
        && binding.declaration.item == direct_thing.declaration.item,
        "alias lookup did not preserve the target declaration identity");
    ok &= check(lookup_path(&resolver, root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, missing, 2u, &binding)
        == CM_IMPORT_LOOKUP_NOT_FOUND,
        "missing final path segment did not report not-found");
    import_result = cm_import_resolve(&resolver, &graph,
        graph_result.revision);
    ok &= check(import_result.error_count == 0u &&
        import_result.revision == graph_result.revision &&
        find_binding(&resolver, root, CM_RESOLVE_NAMESPACE_TYPE,
            "Alias", NULL), "reusing a resolver changed the result");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_descendant_private_glob_visibility(void)
{
    static const char source[] =
        "pub struct Origin;\n"
        "mod parent {\n"
        "    use crate::Origin as PrivateImport;\n"
        "    struct PrivateLocal;\n"
        "    pub struct PublicLocal;\n"
        "    mod child { use super::*; }\n"
        "    mod reexport { pub use super::*; }\n"
        "}\n"
        "mod sibling { use crate::parent::*; }\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmModuleId child;
    CmModuleId reexport;
    CmModuleId sibling;
    CmResolvedBinding binding;
    int ok;

    ok = check(load_memory_and_resolve("private-glob/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load descendant private glob fixture");
    if (!ok) return 0;
    child = find_module(&graph, "crate::parent::child");
    reexport = find_module(&graph, "crate::parent::reexport");
    sibling = find_module(&graph, "crate::sibling");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u && child != CM_MODULE_NONE
        && reexport != CM_MODULE_NONE && sibling != CM_MODULE_NONE,
        "descendant private glob fixture did not resolve");
    ok &= check(find_binding(&resolver, child, CM_RESOLVE_NAMESPACE_TYPE,
            "PrivateImport", &binding) && binding.is_import
        && find_binding(&resolver, child, CM_RESOLVE_NAMESPACE_TYPE,
            "PrivateLocal", NULL)
        && find_binding(&resolver, child, CM_RESOLVE_NAMESPACE_TYPE,
            "PublicLocal", NULL),
        "private glob omitted a binding visible from its descendant");
    ok &= check(!find_binding(&resolver, sibling, CM_RESOLVE_NAMESPACE_TYPE,
            "PrivateImport", NULL)
        && !find_binding(&resolver, sibling, CM_RESOLVE_NAMESPACE_TYPE,
            "PrivateLocal", NULL)
        && find_binding(&resolver, sibling, CM_RESOLVE_NAMESPACE_TYPE,
            "PublicLocal", NULL),
        "private glob binding escaped into a non-descendant module");
    ok &= check(!find_binding(&resolver, reexport,
            CM_RESOLVE_NAMESPACE_TYPE, "PrivateImport", NULL)
        && !find_binding(&resolver, reexport, CM_RESOLVE_NAMESPACE_TYPE,
            "PrivateLocal", NULL)
        && find_binding(&resolver, reexport, CM_RESOLVE_NAMESPACE_TYPE,
            "PublicLocal", &binding) && binding.is_public
        && binding.is_reexport,
        "public glob reexported a private ancestor binding");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_lookup_errors(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    const char *clash[] = { "Clash" };
    const char *bad_prefix[] = { "x", "crate", "Clash" };
    CmResolvePathSegmentView empty;
    int ok;

    ok = check(load_and_resolve(
        "tests/resolve/fixtures/imports_ambiguous/lib.rs", &sources,
        &graph, &resolver, &graph_result, &import_result),
        "could not load ambiguous lookup fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u,
        "ambiguous lookup fixture has graph errors");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, clash, 1u, &binding)
        == CM_IMPORT_LOOKUP_AMBIGUOUS,
        "ambiguous binding did not produce ambiguous lookup status");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, bad_prefix, 3u, &binding)
        == CM_IMPORT_LOOKUP_INVALID,
        "mid-path crate keyword was not rejected");
    empty.bytes = (const unsigned char *)"";
    empty.length = 0u;
    ok &= check(cm_import_resolve_path(&resolver, graph_result.root, 0,
        &empty, 1u, CM_RESOLVE_NAMESPACE_TYPE, &binding)
        == CM_IMPORT_LOOKUP_INVALID,
        "empty path segment was not rejected");
    ok &= check(cm_import_resolve_path(&resolver, CM_MODULE_NONE, 0,
        &empty, 1u, CM_RESOLVE_NAMESPACE_TYPE, &binding)
        == CM_IMPORT_LOOKUP_INVALID,
        "invalid start module was not rejected");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_lookup_keywords_and_namespaces(void)
{
    static const char source[] =
        "mod a {\n"
        "    pub struct Thing;\n"
        "    pub struct Pair(pub usize);\n"
        "    pub union Bits { pub byte: u8 }\n"
        "    pub trait Base {}\n"
        "    pub trait Thin = Base;\n"
        "    pub fn value() {}\n"
        "    pub mod inner { pub struct Local; }\n"
        "}\n"
        "use a::inner as InnerAlias;\n"
        "use crate::{self as RootAlias};\n"
        "use crate::{self as _};\n"
        "use a::Thing as _;\n"
        "use a::Bits as UnionAlias;\n"
        "use a::Thin as ThinAlias;\n"
        "use a::Pair as _;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    CmModuleId a;
    CmModuleId inner;
    const char *self_thing[] = { "self", "a", "Thing" };
    const char *super_thing[] = { "super", "Thing" };
    const char *too_many_super[] = { "super", "super", "super", "Thing" };
    const char *absolute_thing[] = { "a", "Thing" };
    const char *module_a[] = { "a" };
    const char *aliased_local[] = { "InnerAlias", "Local" };
    const char *root_aliased[] = { "RootAlias", "a", "Thing" };
    const char *value[] = { "a", "value" };
    const char *pair[] = { "a", "Pair" };
    const char *bits[] = { "a", "Bits" };
    const char *thin[] = { "a", "Thin" };
    int ok;

    ok = check(load_memory_and_resolve("keywords/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load keyword lookup fixture");
    if (!ok) return 0;
    a = find_module(&graph, "crate::a");
    inner = find_module(&graph, "crate::a::inner");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u && a != CM_MODULE_NONE
        && inner != CM_MODULE_NONE, "keyword lookup fixture has errors");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, self_thing, 3u, &binding)
        == CM_IMPORT_LOOKUP_OK,
        "self keyword depended on incidental interning");
    ok &= check(lookup_path(&resolver, inner, 0,
        CM_RESOLVE_NAMESPACE_TYPE, super_thing, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK,
        "super keyword depended on incidental interning");
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(lookup_path(&resolver, inner, 0,
        CM_RESOLVE_NAMESPACE_TYPE, too_many_super, 4u, &binding)
        == CM_IMPORT_LOOKUP_NOT_FOUND && binding.module == CM_MODULE_NONE,
        "too many super segments did not fail and clear output");
    ok &= check(lookup_path(&resolver, CM_MODULE_NONE, 1,
        CM_RESOLVE_NAMESPACE_TYPE, absolute_thing, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK,
        "absolute lookup incorrectly required a start module");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, module_a, 1u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.target_module == a,
        "final module binding lost its target module");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, aliased_local, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK,
        "imported module alias did not resolve as an intermediate segment");
    ok &= check(find_binding(&resolver, graph_result.root,
        CM_RESOLVE_NAMESPACE_TYPE, "RootAlias", &binding)
        && binding.item_kind == CM_AST_ITEM_MODULE
        && binding.target_module == graph_result.root
        && binding.declaration.source == 0u
        && binding.declaration.item == CM_AST_ITEM_NONE
        && lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, root_aliased, 3u, &binding)
        == CM_IMPORT_LOOKUP_OK
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "_", NULL),
        "root self alias or anonymous imports were resolved as named items");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_VALUE, value, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, value, 2u, &binding)
            == CM_IMPORT_LOOKUP_NOT_FOUND,
        "final lookup did not keep type and value namespaces separate");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, pair, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_VALUE, pair, 2u, &binding)
            == CM_IMPORT_LOOKUP_OK,
        "tuple struct was not visible in both final namespaces");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, bits, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.item_kind == CM_AST_ITEM_UNION
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_VALUE, bits, 2u, &binding)
            == CM_IMPORT_LOOKUP_NOT_FOUND
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "UnionAlias", &binding)
        && binding.item_kind == CM_AST_ITEM_UNION
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "UnionAlias", NULL),
        "union was not confined to the type namespace");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, thin, 2u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.item_kind == CM_AST_ITEM_TRAIT
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_VALUE, thin, 2u, &binding)
            == CM_IMPORT_LOOKUP_NOT_FOUND
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "ThinAlias", &binding)
        && binding.item_kind == CM_AST_ITEM_TRAIT
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "ThinAlias", NULL),
        "trait alias was not confined to the type namespace");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_root_self_requires_alias(void)
{
    static const char source[] =
        "mod a {}\n"
        "use crate::{a as Imported, self};\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmImportError error;
    int ok;

    ok = check(load_memory_and_resolve("root-self/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load unnamed crate-root import fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 1u
        && cm_import_get_error(&resolver, 0u, &error)
        && error.kind == CM_IMPORT_ERROR_INVALID_TREE
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Imported", NULL)
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "crate", NULL),
        "invalid root self group retained a sibling or keyword binding");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_lookup_intermediate_conflicts(void)
{
    static const char source[] =
        "mod target {\n"
        "    pub mod M { pub struct X; }\n"
        "    pub mod Alias { pub struct X; }\n"
        "}\n"
        "mod M {}\n"
        "struct Alias;\n"
        "use target::M;\n"
        "use target::Alias;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    const char *module_conflict[] = { "M", "X" };
    const char *item_conflict[] = { "Alias", "X" };
    int ok;

    ok = check(load_memory_and_resolve("conflicts/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load intermediate-conflict fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 2u,
        "intermediate-conflict fixture did not record both ambiguities");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, module_conflict, 2u, &binding)
        == CM_IMPORT_LOOKUP_AMBIGUOUS,
        "direct module conflict bypassed intermediate ambiguity");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, item_conflict, 2u, &binding)
        == CM_IMPORT_LOOKUP_AMBIGUOUS,
        "nonmodule conflict collapsed intermediate ambiguity to not-found");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_lookup_cycle(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    const char *first[] = { "first" };
    int ok;

    ok = check(load_and_resolve(
        "tests/resolve/fixtures/imports_cycle/lib.rs", &sources, &graph,
        &resolver, &graph_result, &import_result),
        "could not load cycle lookup fixture");
    if (!ok) return 0;
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 2u
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, first, 1u, &binding)
            == CM_IMPORT_LOOKUP_CYCLE
        && binding.module == CM_MODULE_NONE,
        "cyclic lookup was not distinguished or did not clear output");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_error(const char *path, CmImportErrorKind expected,
    size_t expected_count)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmImportError error;
    int ok;

    ok = check(load_and_resolve(path, &sources, &graph, &resolver,
        &graph_result, &import_result), "could not load error fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u,
        "module graph rejected import-only error fixture");
    if (import_result.error_count != expected_count) {
        fprintf(stderr, "test-imports: %s: expected %lu errors, got %lu\n",
            path, (unsigned long)expected_count,
            (unsigned long)import_result.error_count);
    }
    ok &= check(import_result.error_count == expected_count &&
        cm_import_error_count(&resolver) == expected_count,
        "import error count differs");
    ok &= check(import_result.revision == graph_result.revision &&
        cm_import_resolver_matches_graph(&resolver, &graph),
        "import diagnostics were not tied to the resolved graph revision");
    ok &= check(expected_count != 0u &&
        cm_import_get_error(&resolver, 0u, &error) && error.kind == expected,
        "structured import error kind differs");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_graph_independence(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding binding;
    const char *alias[] = { "Alias" };
    int ok;

    ok = check(load_and_resolve("tests/resolve/fixtures/imports/lib.rs",
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not reload success fixture");
    if (!ok) return 0;
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    ok &= check(import_result.error_count == 0u &&
        find_binding(&resolver, graph_result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "Alias", NULL), "resolver retained graph-owned storage");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.is_import,
        "path lookup retained graph-owned storage");
    cm_import_resolver_destroy(&resolver);
    return ok;
}

static int test_revision_invalidation(void)
{
    static const char source[] =
        "mod a { pub struct Thing; }\n"
        "use a::Thing as Alias;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraph other_graph;
    CmImportResolver resolver;
    CmModuleGraphResult first;
    CmModuleGraphResult other;
    CmModuleGraphResult second;
    CmModuleGraphResult failed;
    CmImportResult import_result;
    CmResolvedBinding binding;
    CmResolveModuleInfo root_information;
    CmSourceId root_source;
    const char *alias[] = { "Alias" };
    int ok;

    ok = check(load_memory_and_resolve("revision/lib.rs", source,
        &sources, &graph, &resolver, &first, &import_result),
        "could not load revision fixture");
    if (!ok) return 0;
    root_source = (CmSourceId)0;
    memset(&root_information, 0, sizeof(root_information));
    ok &= check(first.error_count == 0u && import_result.error_count == 0u &&
        import_result.revision == first.revision &&
        cm_module_graph_get_module(&graph, first.root, &root_information),
        "initial revision fixture did not resolve");
    root_source = root_information.source;
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(lookup_path_checked(&resolver, &graph, first.revision,
        first.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.revision == first.revision,
        "checked lookup rejected the matching graph revision");

    cm_module_graph_init(&other_graph);
    other = build_graph_with_empty_cfg(&other_graph, &sources, root_source);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(other.error_count == 0u && other.revision == first.revision &&
        !cm_import_resolver_matches_graph(&resolver, &other_graph) &&
        lookup_path_checked(&resolver, &other_graph, other.revision,
            other.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
            == CM_IMPORT_LOOKUP_STALE_REVISION &&
        binding.module == CM_MODULE_NONE,
        "equal revisions from distinct graph objects were treated as equal");
    cm_module_graph_destroy(&other_graph);

    second = build_graph_with_empty_cfg(&graph, &sources, root_source);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(second.error_count == 0u && second.revision != first.revision &&
        !cm_import_resolver_matches_graph(&resolver, &graph) &&
        lookup_path_checked(&resolver, &graph, second.revision, second.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
            == CM_IMPORT_LOOKUP_STALE_REVISION &&
        binding.revision == CM_MODULE_GRAPH_REVISION_NONE &&
        binding.module == CM_MODULE_NONE,
        "graph rebuild did not stale checked import lookup and clear output");
    ok &= check(lookup_path(&resolver, first.root, 0,
        CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_OK && binding.revision == first.revision,
        "resolver-owned unchecked snapshot changed after graph rebuild");

    import_result = cm_import_resolve(&resolver, &graph, first.revision);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(import_result.revision == CM_MODULE_GRAPH_REVISION_NONE &&
        import_result.binding_count == 0u && import_result.error_count == 1u &&
        cm_import_resolver_revision(&resolver) ==
            CM_MODULE_GRAPH_REVISION_NONE &&
        cm_import_binding_count(&resolver, first.root,
            CM_RESOLVE_NAMESPACE_TYPE) == 0u &&
        lookup_path(&resolver, first.root, 0, CM_RESOLVE_NAMESPACE_TYPE,
            alias, 1u, &binding) == CM_IMPORT_LOOKUP_INVALID &&
        binding.module == CM_MODULE_NONE,
        "stale expected revision did not invalidate resolver output");

    import_result = cm_import_resolve(&resolver, &graph, second.revision);
    ok &= check(import_result.error_count == 0u &&
        import_result.revision == second.revision &&
        cm_import_resolver_matches_graph(&resolver, &graph),
        "resolver did not adopt the rebuilt graph revision");
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(lookup_path_checked(&resolver, &graph, first.revision,
        second.root, 0, CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_STALE_REVISION
        && binding.module == CM_MODULE_NONE,
        "checked lookup inferred a newer resolver revision than its caller");

    failed = build_graph_with_empty_cfg(&graph, &sources, (CmSourceId)0);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(failed.error_count != 0u &&
        failed.revision != second.revision &&
        !cm_import_resolver_matches_graph(&resolver, &graph) &&
        lookup_path_checked(&resolver, &graph, failed.revision, second.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
            == CM_IMPORT_LOOKUP_FAILED_BUILD &&
        binding.module == CM_MODULE_NONE,
        "failed graph rebuild did not reject checked import lookup");
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(lookup_path_checked(&resolver, &graph, second.revision,
        second.root, 0, CM_RESOLVE_NAMESPACE_TYPE, alias, 1u, &binding)
        == CM_IMPORT_LOOKUP_STALE_REVISION
        && binding.module == CM_MODULE_NONE,
        "older expected revision did not take precedence over failed build");

    import_result = cm_import_resolve(&resolver, &graph, failed.revision);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(import_result.revision == CM_MODULE_GRAPH_REVISION_NONE &&
        import_result.binding_count == 0u && import_result.error_count == 1u &&
        cm_import_resolver_revision(&resolver) ==
            CM_MODULE_GRAPH_REVISION_NONE &&
        !cm_import_resolver_matches_graph(&resolver, &graph) &&
        cm_import_binding_count(&resolver, second.root,
            CM_RESOLVE_NAMESPACE_TYPE) == 0u &&
        lookup_path(&resolver, second.root, 0, CM_RESOLVE_NAMESPACE_TYPE,
            alias, 1u, &binding) == CM_IMPORT_LOOKUP_INVALID &&
        binding.module == CM_MODULE_NONE,
        "invalid graph resolution did not clear the previous resolver state");
    ok &= check(strcmp(cm_import_lookup_status_name(
            CM_IMPORT_LOOKUP_FAILED_BUILD), "failed build") == 0
        && strcmp(cm_import_lookup_status_name(
            CM_IMPORT_LOOKUP_STALE_REVISION), "stale revision") == 0,
        "import lookup status names differ");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_generated_declaration_imports(void)
{
    static const char source[] =
        "macro_rules! make_type {\n"
        "    () => { pub struct Generated; }\n"
        "}\n"
        "mod inner { make_type!(); }\n"
        "use crate::inner::Generated;\n"
        "pub use crate::inner::Generated as Reexported;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding direct;
    CmResolvedBinding imported;
    CmResolvedBinding reexported;
    CmModuleId inner;
    int ok;

    memset(&direct, 0, sizeof(direct));
    memset(&imported, 0, sizeof(imported));
    memset(&reexported, 0, sizeof(reexported));
    ok = check(load_memory_and_resolve("generated-import/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load generated declaration import fixture");
    if (!ok) return 0;
    inner = find_module(&graph, "crate::inner");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u
        && inner != CM_MODULE_NONE,
        "generated declaration import fixture did not resolve");
    ok &= check(find_binding(&resolver, inner, CM_RESOLVE_NAMESPACE_TYPE,
            "Generated", &direct)
        && !direct.is_import && direct.is_public
        && direct.item_kind == CM_AST_ITEM_STRUCT,
        "generated declaration did not enter the source module namespace");
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Generated", &imported)
        && imported.is_import && !imported.is_reexport
        && imported.item_kind == CM_AST_ITEM_STRUCT,
        "source import did not bind the generated declaration");
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Reexported", &reexported)
        && reexported.is_import && reexported.is_reexport
        && reexported.is_public
        && reexported.item_kind == CM_AST_ITEM_STRUCT,
        "source reexport did not bind the generated declaration");
    ok &= check(direct.declaration.source != 0u
        && direct.declaration.item != CM_AST_ITEM_NONE
        && direct.declaration.source == imported.declaration.source
        && direct.declaration.item == imported.declaration.item
        && direct.declaration.source == reexported.declaration.source
        && direct.declaration.item == reexported.declaration.item,
        "generated import bindings did not preserve declaration identity");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_declaration_binding_view(void)
{
    static const char source[] =
        "mod source {\n"
        "    pub struct Pair(pub usize);\n"
        "    pub struct Globbed;\n"
        "    pub fn call() {}\n"
        "}\n"
        "struct Globbed;\n"
        "use source::{Pair as _, call as _};\n"
        "use source::*;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmModuleGraphResult rebuilt;
    CmImportResult import_result;
    CmResolveModuleInfo root_information;
    CmResolveImport anonymous_import;
    CmResolveImport glob_import;
    CmResolvedBinding binding;
    CmResolvedBinding destination;
    CmResolveItemRef wrong_declaration;
    CmSourceId root_source;
    CmModuleId source_module;
    size_t anonymous_count;
    size_t glob_count;
    int ok;

    memset(&root_information, 0, sizeof(root_information));
    memset(&anonymous_import, 0, sizeof(anonymous_import));
    memset(&glob_import, 0, sizeof(glob_import));
    memset(&wrong_declaration, 0, sizeof(wrong_declaration));
    root_source = (CmSourceId)0;
    ok = check(load_memory_and_resolve("declaration-view/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load declaration-binding fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u &&
        import_result.error_count == 0u &&
        cm_module_graph_get_module(&graph, graph_result.root,
            &root_information) && root_information.import_count == 2u &&
        cm_module_graph_get_import(&graph, graph_result.root, 0u,
            &anonymous_import) &&
        cm_module_graph_get_import(&graph, graph_result.root, 1u,
            &glob_import),
        "declaration-binding fixture imports were not preserved");
    root_source = root_information.source;
    source_module = find_module(&graph, "crate::source");
    anonymous_count = cm_import_declaration_binding_count(&resolver,
        graph_result.root, anonymous_import.declaration);
    glob_count = cm_import_declaration_binding_count(&resolver,
        graph_result.root, glob_import.declaration);
    ok &= check(anonymous_count == 3u && glob_count == 5u,
        "declaration binding counts include retries or omit leaf results");

    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, anonymous_import.declaration, 0u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE &&
        import_string_equals(&resolver, binding.name, "_") &&
        binding.item_kind == CM_AST_ITEM_STRUCT && binding.is_import &&
        binding.is_anonymous &&
        binding.revision == graph_result.revision &&
        item_ref_equals(binding.import_declaration,
            anonymous_import.declaration),
        "anonymous group first leaf/type result differs");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, anonymous_import.declaration, 1u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_VALUE &&
        import_string_equals(&resolver, binding.name, "_") &&
        binding.item_kind == CM_AST_ITEM_STRUCT && binding.is_anonymous,
        "anonymous group first leaf/value result lost result order");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, anonymous_import.declaration, 2u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_VALUE &&
        import_string_equals(&resolver, binding.name, "_") &&
        binding.item_kind == CM_AST_ITEM_FUNCTION && binding.is_anonymous,
        "anonymous group second leaf did not follow first leaf results");
    ok &= check(!find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "_", NULL) &&
        !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "_", NULL),
        "anonymous declaration results leaked into destination namespaces");

    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 0u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE &&
        import_string_equals(&resolver, binding.name, "Pair") &&
        !binding.is_anonymous,
        "glob type result order does not begin with Pair");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 1u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE &&
        import_string_equals(&resolver, binding.name, "Globbed") &&
        !binding.is_anonymous,
        "glob omitted the destination-shadowed binding");
    destination = binding;
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Globbed", &destination) &&
        !destination.is_import &&
        !item_ref_equals(destination.declaration, binding.declaration),
        "glob result was not distinct from its shadowing destination item");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 2u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_VALUE &&
        import_string_equals(&resolver, binding.name, "Pair"),
        "glob value namespace order does not follow type results");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 3u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_VALUE &&
        import_string_equals(&resolver, binding.name, "Globbed"),
        "glob omitted the shadowed value-namespace constructor");
    memset(&binding, 0, sizeof(binding));
    ok &= check(cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 4u, &binding) &&
        binding.namespace_kind == CM_RESOLVE_NAMESPACE_VALUE &&
        import_string_equals(&resolver, binding.name, "call"),
        "glob final value result order differs");

    memset(&binding, 0xff, sizeof(binding));
    wrong_declaration = glob_import.declaration;
    ++wrong_declaration.source;
    ok &= check(!cm_import_get_declaration_binding(&resolver,
            graph_result.root, glob_import.declaration, 5u, &binding) &&
        binding.module == CM_MODULE_NONE &&
        cm_import_declaration_binding_count(&resolver, CM_MODULE_NONE,
            glob_import.declaration) == 0u &&
        source_module != CM_MODULE_NONE &&
        cm_import_declaration_binding_count(&resolver, source_module,
            glob_import.declaration) == 0u &&
        cm_import_declaration_binding_count(&resolver, graph_result.root,
            wrong_declaration) == 0u,
        "invalid declaration binding access did not clear output");

    import_result = cm_import_resolve(&resolver, &graph,
        graph_result.revision);
    ok &= check(import_result.error_count == 0u &&
        cm_import_declaration_binding_count(&resolver, graph_result.root,
            anonymous_import.declaration) == 3u &&
        cm_import_declaration_binding_count(&resolver, graph_result.root,
            glob_import.declaration) == 5u,
        "re-resolving duplicated declaration binding results");

    rebuilt = build_graph_with_empty_cfg(&graph, &sources, root_source);
    ok &= check(rebuilt.error_count == 0u &&
        rebuilt.revision != graph_result.revision &&
        cm_import_declaration_binding_count(&resolver, graph_result.root,
            glob_import.declaration) == 5u,
        "resolver-owned declaration snapshot changed before invalidation");
    import_result = cm_import_resolve(&resolver, &graph,
        graph_result.revision);
    memset(&binding, 0xff, sizeof(binding));
    ok &= check(import_result.revision == CM_MODULE_GRAPH_REVISION_NONE &&
        cm_import_declaration_binding_count(&resolver, graph_result.root,
            glob_import.declaration) == 0u &&
        !cm_import_get_declaration_binding(&resolver, graph_result.root,
            glob_import.declaration, 0u, &binding) &&
        binding.revision == CM_MODULE_GRAPH_REVISION_NONE &&
        binding.module == CM_MODULE_NONE,
        "invalidated resolver retained declaration binding results");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_enum_variant_imports(void)
{
    static const char source[] =
        "mod model {\n"
        "    pub enum Choice {\n"
        "        #[cfg(any())] Hidden,\n"
        "        Unit,\n"
        "        Tuple(u8),\n"
        "        Named { value: u8 },\n"
        "    }\n"
        "}\n"
        "use model::Choice::{self, Unit, Tuple, Named};\n"
        "use Choice as Alias;\n"
        "use Alias::Tuple as Again;\n"
        "use model::Choice::*;\n";
    static const char *const tuple_path[] = { "model", "Choice", "Tuple" };
    static const char *const named_path[] = { "model", "Choice", "Named" };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding choice;
    CmResolvedBinding unit_type;
    CmResolvedBinding unit_value;
    CmResolvedBinding tuple_type;
    CmResolvedBinding again_value;
    CmResolvedBinding named_type;
    CmResolvedBinding lookup;
    int ok;

    memset(&choice, 0, sizeof(choice));
    memset(&unit_type, 0, sizeof(unit_type));
    memset(&unit_value, 0, sizeof(unit_value));
    memset(&tuple_type, 0, sizeof(tuple_type));
    memset(&again_value, 0, sizeof(again_value));
    memset(&named_type, 0, sizeof(named_type));
    ok = check(load_memory_and_resolve("enum-imports/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load enum-import fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u,
        "enum self, variant, alias, or glob import did not resolve");
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Choice", &choice)
        && choice.item_kind == CM_AST_ITEM_ENUM
        && choice.variant.enumeration.source == 0u
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Unit", &unit_type)
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "Unit", &unit_value)
        && unit_type.variant.enumeration.source != 0u
        && unit_type.variant.index == 1u
        && item_ref_equals(unit_type.variant.enumeration,
            choice.declaration)
        && unit_value.variant.index == unit_type.variant.index
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Tuple", &tuple_type)
        && tuple_type.variant.index == 2u
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "Again", &again_value)
        && again_value.variant.index == tuple_type.variant.index
        && item_ref_equals(again_value.variant.enumeration,
            tuple_type.variant.enumeration)
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Named", &named_type)
        && named_type.variant.index == 3u
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_VALUE, "Named", NULL)
        && !find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Hidden", NULL),
        "enum imports lost cfg-safe variant identity or namespace roles");
    memset(&lookup, 0, sizeof(lookup));
    ok &= check(lookup_path_checked(&resolver, &graph,
            graph_result.revision, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_VALUE, tuple_path, 3u, &lookup)
            == CM_IMPORT_LOOKUP_OK
        && lookup.variant.index == 2u
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, named_path, 3u, &lookup)
            == CM_IMPORT_LOOKUP_OK
        && lookup.variant.index == 3u
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_VALUE, named_path, 3u, &lookup)
            == CM_IMPORT_LOOKUP_NOT_FOUND,
        "direct path lookup did not traverse effective enum namespaces");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_core_shaped_macro_identity(void)
{
    static const char source[] =
        "use prelude::rust_2024::*;\n"
        "#[macro_use]\n"
        "mod macros {\n"
        "  #[rustc_builtin_macro]\n"
        "  #[macro_export]\n"
        "  macro_rules! include { ($file:expr) => {}; }\n"
        "}\n"
        "mod ub_checks {\n"
        "  #[macro_export]\n"
        "  macro_rules! assert_unsafe_precondition { () => {}; }\n"
        "  pub use assert_unsafe_precondition;\n"
        "}\n"
        "mod prelude {\n"
        "  pub mod v1 { pub use crate::include; }\n"
        "  pub mod rust_2024 { pub use super::v1::*; }\n"
        "}\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding root_binding;
    CmResolvedBinding v1_binding;
    CmResolvedBinding edition_binding;
    CmResolvedBinding exported_binding;
    CmResolvedBinding reexported_binding;
    CmResolveMacroScopeEntry textual_binding;
    CmResolveMacroDeclaration declaration;
    CmResolveEffectiveAttribute attribute;
    const char *v1_path[] = { "prelude", "v1", "include" };
    const char *edition_path[] = { "prelude", "rust_2024", "include" };
    const char *reexported_path[] = {
        "ub_checks", "assert_unsafe_precondition"
    };
    int ok;

    memset(&root_binding, 0, sizeof(root_binding));
    memset(&v1_binding, 0, sizeof(v1_binding));
    memset(&edition_binding, 0, sizeof(edition_binding));
    memset(&exported_binding, 0, sizeof(exported_binding));
    memset(&reexported_binding, 0, sizeof(reexported_binding));
    memset(&textual_binding, 0, sizeof(textual_binding));
    memset(&declaration, 0, sizeof(declaration));
    memset(&attribute, 0, sizeof(attribute));
    ok = check(load_memory_and_resolve("core-shaped/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load core-shaped macro identity fixture");
    if (!ok) return 0;
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "include", &root_binding)
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_MACRO, v1_path, 3u, &v1_binding)
                == CM_IMPORT_LOOKUP_OK
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_MACRO, edition_path, 3u, &edition_binding)
                == CM_IMPORT_LOOKUP_OK
        && find_macro_scope_entry(&graph, graph_result.root, "include",
            &textual_binding),
        "core-shaped macro routes did not all resolve");
    ok &= check(item_ref_equals(root_binding.declaration,
            v1_binding.declaration)
        && item_ref_equals(root_binding.declaration,
            edition_binding.declaration)
        && item_ref_equals(root_binding.declaration,
            textual_binding.declaration)
        && textual_binding.is_macro_use
        && root_binding.item_kind == CM_AST_ITEM_MACRO
        && v1_binding.is_reexport && edition_binding.is_reexport,
        "macro export, macro_use, and prelude routes lost exact identity");
    ok &= check(find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "assert_unsafe_precondition",
            &exported_binding)
        && lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_MACRO, reexported_path, 2u,
            &reexported_binding) == CM_IMPORT_LOOKUP_OK
        && item_ref_equals(exported_binding.declaration,
            reexported_binding.declaration)
        && reexported_binding.is_public && reexported_binding.is_reexport
        && !reexported_binding.is_ambiguous,
        "same-target macro self-reexport became ambiguous");
    ok &= check(cm_module_graph_get_macro_declaration(&graph,
            graph_result.revision, root_binding.declaration,
            &declaration) == CM_RESOLVE_VIEW_OK
        && declaration.form == CM_AST_MACRO_RULES_DEFINITION
        && declaration.attribute_count == 2u
        && cm_module_graph_get_macro_declaration_attribute(&graph,
            graph_result.revision, root_binding.declaration, 0u,
            &attribute) == CM_RESOLVE_VIEW_OK
        && graph_string_equals(&graph, attribute.metadata,
            "rustc_builtin_macro"),
        "resolved macro identity did not recover exact builtin metadata");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_prelude_import_scope(void)
{
    static const char raw_source[] =
        "#[prelude_import] use crate::prelude::*;\n"
        "mod consumer {}\n"
        "mod shadow { pub struct Sized; }\n"
        "mod prelude {\n"
        "  pub use crate::marker::{Later, Sized};\n"
        "  use crate::marker::Private;\n"
        "}\n"
        "mod marker {\n"
        "  pub trait Later {} pub trait Sized {} pub trait Private {}\n"
        "}\n";
    static const char expanded_source[] =
        "#[cfg_attr(all(), prelude_import)]\n"
        "use crate::prelude::*;\n"
        "mod consumer {}\n"
        "mod prelude { pub use crate::marker::Sized; }\n"
        "mod marker { pub trait Sized {} }\n";
    static const char ambiguous_source[] =
        "#[prelude_import] use crate::prelude::*;\n"
        "mod consumer {}\n"
        "mod prelude { pub use crate::left::*; pub use crate::right::*; }\n"
        "mod left { pub trait Clash {} }\n"
        "mod right { pub trait Clash {} }\n";
    static const char duplicate_source[] =
        "#[prelude_import] use crate::first::*;\n"
        "#[prelude_import] use crate::second::*;\n"
        "mod first { pub trait One {} }\n"
        "mod second { pub trait Two {} }\n";
    const char *sized_path[] = { "Sized" };
    const char *later_path[] = { "Later" };
    const char *private_path[] = { "Private" };
    const char *qualified_sized_path[] = { "consumer", "Sized" };
    const char *clash_path[] = { "Clash" };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding sized;
    CmResolvedBinding later;
    CmResolvedBinding shadowed;
    CmModuleId consumer;
    CmModuleId shadow;
    size_t index;
    int saw_invalid;
    int ok;

    memset(&sized, 0, sizeof(sized));
    memset(&later, 0, sizeof(later));
    memset(&shadowed, 0, sizeof(shadowed));
    ok = check(load_memory_and_resolve("prelude-raw/lib.rs", raw_source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load raw prelude-import fixture");
    if (!ok) return 0;
    consumer = find_module(&graph, "crate::consumer");
    shadow = find_module(&graph, "crate::shadow");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u
        && consumer != CM_MODULE_NONE && shadow != CM_MODULE_NONE
        && lookup_path_checked(&resolver, &graph, graph_result.revision,
            consumer, 0, CM_RESOLVE_NAMESPACE_TYPE, sized_path, 1u,
            &sized) == CM_IMPORT_LOOKUP_OK
        && sized.item_kind == CM_AST_ITEM_TRAIT && sized.is_import
        && lookup_path(&resolver, consumer, 0, CM_RESOLVE_NAMESPACE_TYPE,
            later_path, 1u, &later) == CM_IMPORT_LOOKUP_OK
        && later.item_kind == CM_AST_ITEM_TRAIT
        && lookup_path(&resolver, consumer, 0, CM_RESOLVE_NAMESPACE_TYPE,
            private_path, 1u, &later) == CM_IMPORT_LOOKUP_NOT_FOUND
        && lookup_path(&resolver, consumer, 1, CM_RESOLVE_NAMESPACE_TYPE,
            qualified_sized_path, 2u, &later)
            == CM_IMPORT_LOOKUP_NOT_FOUND
        && lookup_path(&resolver, shadow, 0, CM_RESOLVE_NAMESPACE_TYPE,
            sized_path, 1u, &shadowed) == CM_IMPORT_LOOKUP_OK
        && shadowed.item_kind == CM_AST_ITEM_STRUCT
        && !item_ref_equals(shadowed.declaration, sized.declaration)
        && !find_binding(&resolver, consumer, CM_RESOLVE_NAMESPACE_TYPE,
            "Sized", NULL),
        "raw prelude scope lost order, privacy, qualification, or shadowing");
    destroy_all(&sources, &graph, &resolver);

    ok &= check(load_memory_and_resolve("prelude-expanded/lib.rs",
        expanded_source, &sources, &graph, &resolver, &graph_result,
        &import_result), "could not load expanded prelude-import fixture");
    consumer = find_module(&graph, "crate::consumer");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u
        && lookup_path(&resolver, consumer, 0, CM_RESOLVE_NAMESPACE_TYPE,
            sized_path, 1u, &sized) == CM_IMPORT_LOOKUP_OK
        && sized.item_kind == CM_AST_ITEM_TRAIT,
        "effective cfg_attr prelude import was not propagated");
    destroy_all(&sources, &graph, &resolver);

    ok &= check(load_memory_and_resolve("prelude-ambiguous/lib.rs",
        ambiguous_source, &sources, &graph, &resolver, &graph_result,
        &import_result), "could not load ambiguous prelude fixture");
    consumer = find_module(&graph, "crate::consumer");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count != 0u
        && lookup_path(&resolver, consumer, 0, CM_RESOLVE_NAMESPACE_TYPE,
            clash_path, 1u, &sized) != CM_IMPORT_LOOKUP_OK,
        "ambiguous prelude binding escaped fail-closed resolution");
    destroy_all(&sources, &graph, &resolver);

    ok &= check(load_memory_and_resolve("prelude-duplicate/lib.rs",
        duplicate_source, &sources, &graph, &resolver, &graph_result,
        &import_result), "could not load duplicate prelude fixture");
    saw_invalid = 0;
    for (index = 0u; index < import_result.error_count; ++index) {
        CmImportError error;

        memset(&error, 0, sizeof(error));
        if (index <= (size_t)UINT32_MAX
            && cm_import_get_error(&resolver, (uint32_t)index, &error)
            && error.kind == CM_IMPORT_ERROR_INVALID_TREE) {
            saw_invalid = 1;
        }
    }
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count != 0u && saw_invalid,
        "duplicate prelude imports were not rejected structurally");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_builtin_primitive_imports(void)
{
    static const char source[] =
        "pub mod primitive {\n"
        "  pub use bool;\n"
        "  pub use str;\n"
        "  pub use u8 as byte;\n"
        "}\n"
        "use primitive::{bool as Bool, byte, str};\n";
    const char *qualified_bool[] = { "primitive", "bool" };
    const char *bare_u16[] = { "u16" };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmImportResolver resolver;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmResolvedBinding bool_binding;
    CmResolvedBinding str_binding;
    CmResolvedBinding byte_binding;
    CmResolvedBinding lookup;
    CmModuleId primitive;
    int ok;

    memset(&bool_binding, 0, sizeof(bool_binding));
    memset(&str_binding, 0, sizeof(str_binding));
    memset(&byte_binding, 0, sizeof(byte_binding));
    memset(&lookup, 0, sizeof(lookup));
    ok = check(load_memory_and_resolve("primitive-imports/lib.rs", source,
        &sources, &graph, &resolver, &graph_result, &import_result),
        "could not load builtin primitive import fixture");
    if (!ok) return 0;
    primitive = find_module(&graph, "crate::primitive");
    ok &= check(graph_result.error_count == 0u
        && import_result.error_count == 0u
        && primitive != CM_MODULE_NONE
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "Bool", &bool_binding)
        && bool_binding.primitive_kind == CM_RESOLVE_PRIMITIVE_BOOL
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "str", &str_binding)
        && str_binding.primitive_kind == CM_RESOLVE_PRIMITIVE_STR
        && find_binding(&resolver, graph_result.root,
            CM_RESOLVE_NAMESPACE_TYPE, "byte", &byte_binding)
        && byte_binding.primitive_kind == CM_RESOLVE_PRIMITIVE_U8
        && bool_binding.declaration.source == 0u
        && bool_binding.declaration.item == CM_AST_ITEM_NONE
        && byte_binding.declaration.source == 0u
        && byte_binding.declaration.item == CM_AST_ITEM_NONE,
        "primitive imports lost builtin identity or invented declarations");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, qualified_bool, 2u, &lookup)
            == CM_IMPORT_LOOKUP_OK
        && lookup.primitive_kind == CM_RESOLVE_PRIMITIVE_BOOL
        && lookup.is_public && lookup.is_reexport,
        "qualified primitive reexport lookup lost builtin identity");
    ok &= check(lookup_path(&resolver, graph_result.root, 0,
            CM_RESOLVE_NAMESPACE_TYPE, bare_u16, 1u, &lookup)
            == CM_IMPORT_LOOKUP_OK
        && lookup.primitive_kind == CM_RESOLVE_PRIMITIVE_U16
        && lookup.declaration.source == 0u
        && lookup.declaration.item == CM_AST_ITEM_NONE,
        "bare primitive lookup lost builtin identity");
    ok &= check(!find_binding(&resolver, primitive,
            CM_RESOLVE_NAMESPACE_VALUE, "bool", NULL),
        "primitive reexport entered the value namespace");
    destroy_all(&sources, &graph, &resolver);
    return ok;
}

static int test_dependency_macro_artifact(void)
{
    static const char source[] =
        "#[macro_export] macro_rules! exported {"
        "  () => { struct FromDependency; };"
        "}"
        "pub mod api { pub use crate::exported as renamed; }"
        "mod hidden { pub use crate::exported as concealed; }";
    static const char *const public_parts[] = {
        "dep", "api", "renamed"
    };
    static const char *const private_parts[] = {
        "dep", "hidden", "concealed"
    };
    static const char *const other_parts[] = {
        "other", "api", "renamed"
    };
    static const char *const generated_parts[] = {
        "rust_dep", "api", "renamed"
    };
    CmResolvePathSegmentView public_path[3];
    CmResolvePathSegmentView private_path[3];
    CmResolvePathSegmentView other_path[3];
    CmResolvePathSegmentView generated_path[3];
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmDependencyMacroArtifact artifact;
    CmDependencyMacroArtifactResult artifact_result;
    CmDependencyMacroDefinition definition;
    CmSourceId root;
    size_t index;
    int ok;

    for (index = 0u; index < 3u; ++index) {
        public_path[index].bytes =
            (const unsigned char *)public_parts[index];
        public_path[index].length = strlen(public_parts[index]);
        private_path[index].bytes =
            (const unsigned char *)private_parts[index];
        private_path[index].length = strlen(private_parts[index]);
        other_path[index].bytes =
            (const unsigned char *)other_parts[index];
        other_path[index].length = strlen(other_parts[index]);
        generated_path[index].bytes =
            (const unsigned char *)generated_parts[index];
        generated_path[index].length = strlen(generated_parts[index]);
    }
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_dependency_macro_artifact_init(&artifact);
    ok = check(cm_source_add_memory(&sources, "dependency/lib.rs",
        (const unsigned char *)source, strlen(source), &root)
            == CM_SOURCE_OK,
        "could not add dependency macro artifact fixture");
    if (!ok) {
        cm_dependency_macro_artifact_destroy(&artifact);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return 0;
    }
    graph_result = build_graph_with_empty_cfg(&graph, &sources, root);
    artifact_result = cm_dependency_macro_artifact_build(&artifact, &graph,
        graph_result.revision, "dep", "rust_dep");
    memset(&definition, 0, sizeof(definition));
    ok &= check(graph_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && artifact_result.import_error_count == 0u
        && cm_dependency_macro_artifact_lookup(&artifact, public_path, 3u,
            &definition) == CM_DEPENDENCY_MACRO_OK
        && definition.dependency_graph == &graph
        && definition.dependency_revision == graph_result.revision
        && definition.declaration.source != 0u
        && definition.declaration.item != CM_AST_ITEM_NONE
        && definition.definition_ast != NULL
        && definition.form == CM_AST_MACRO_RULES_DEFINITION
        && strcmp(definition.extern_name, "dep") == 0
        && strcmp(definition.crate_identifier, "rust_dep") == 0,
        "public dependency macro path lost its exact live definition");
    ok &= check(cm_dependency_macro_artifact_lookup_generated(&artifact,
            generated_path, 3u, &definition) == CM_DEPENDENCY_MACRO_OK
        && definition.dependency_graph == &graph
        && strcmp(definition.extern_name, "dep") == 0
        && strcmp(definition.crate_identifier, "rust_dep") == 0,
        "generated defining-crate path lost the dependency definition");
    ok &= check(cm_dependency_macro_artifact_lookup_generated(&artifact,
            public_path, 3u, &definition) == CM_DEPENDENCY_MACRO_NOT_FOUND
        && definition.dependency_graph == NULL,
        "consumer extern name was accepted as generated $crate identity");
    ok &= check(cm_dependency_macro_artifact_lookup(&artifact, private_path,
            3u, &definition) == CM_DEPENDENCY_MACRO_PRIVATE_PATH
        && definition.dependency_graph == NULL,
        "private dependency module path was exported");
    ok &= check(cm_dependency_macro_artifact_lookup(&artifact, other_path,
            3u, &definition) == CM_DEPENDENCY_MACRO_NOT_FOUND
        && definition.dependency_graph == NULL,
        "unrelated dependency name was accepted by the artifact");
    artifact_result = cm_dependency_macro_artifact_build(&artifact, &graph,
        graph_result.revision, "dep::guessed", "rust_dep");
    ok &= check(artifact_result.status
            == CM_DEPENDENCY_MACRO_INVALID_ARGUMENT
        && !cm_dependency_macro_artifact_matches(&artifact, &graph,
            graph_result.revision),
        "malformed dependency artifact identity was accepted");
    artifact_result = cm_dependency_macro_artifact_build(&artifact, &graph,
        graph_result.revision, "dep", "rust_dep");
    graph_result = build_graph_with_empty_cfg(&graph, &sources, root);
    ok &= check(artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && cm_dependency_macro_artifact_lookup(&artifact, public_path, 3u,
            &definition) == CM_DEPENDENCY_MACRO_STALE_REVISION,
        "dependency graph rebuild did not invalidate the macro artifact");
    ok &= check(strcmp(cm_dependency_macro_status_name(
            CM_DEPENDENCY_MACRO_PRIVATE_PATH), "private path") == 0,
        "dependency macro status name changed");
    cm_dependency_macro_artifact_destroy(&artifact);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_dependency_macro_consumer_import(void)
{
    static const char dependency_source[] =
        "#[macro_export] macro_rules! exported {"
        "  () => { struct FromDependency; };"
        "}"
        "pub mod api { pub use crate::exported as renamed; }"
        "mod hidden { pub use crate::exported as concealed; }";
    static const char consumer_source[] =
        "mod success {"
        "  pub use dep::api::{renamed as local, renamed as second};"
        "}"
        "mod collision {"
        "  macro_rules! local { () => { struct Local; }; }"
        "  use dep::api::renamed as local;"
        "}"
        "mod ambiguous {"
        "  use dep::api::renamed as local;"
        "  use other::api::renamed as local;"
        "}"
        "mod globbed { use dep::api::*; }"
        "mod private_path { use dep::hidden::concealed as local; }";
    CmSourceSet dependency_sources;
    CmSourceSet consumer_sources;
    CmModuleGraph dependency_graph;
    CmModuleGraph consumer_graph;
    CmModuleGraphResult dependency_result;
    CmModuleGraphResult consumer_result;
    CmDependencyMacroArtifact artifact;
    CmDependencyMacroArtifactResult artifact_result;
    CmDependencyMacroImport imported;
    CmImportResolver imports;
    CmImportResult import_result;
    CmImportLeafView leaf;
    CmResolvePathSegmentView segment;
    CmResolvePathSegmentView local_name;
    CmSourceId dependency_root;
    CmSourceId consumer_root;
    CmModuleId success;
    CmModuleId collision;
    CmModuleId ambiguous;
    CmModuleId globbed;
    CmModuleId private_path;
    size_t leaf_count;
    int saw_grouped_alias;
    int ok;

    cm_source_set_init(&dependency_sources);
    cm_source_set_init(&consumer_sources);
    cm_module_graph_init(&dependency_graph);
    cm_module_graph_init(&consumer_graph);
    cm_dependency_macro_artifact_init(&artifact);
    cm_import_resolver_init(&imports);
    ok = check(cm_source_add_memory(&dependency_sources,
            "dependency/lib.rs", (const unsigned char *)dependency_source,
            strlen(dependency_source), &dependency_root) == CM_SOURCE_OK
        && cm_source_add_memory(&consumer_sources, "consumer/lib.rs",
            (const unsigned char *)consumer_source, strlen(consumer_source),
            &consumer_root) == CM_SOURCE_OK,
        "could not add cross-graph consumer import fixtures");
    if (!ok) {
        cm_import_resolver_destroy(&imports);
        cm_dependency_macro_artifact_destroy(&artifact);
        cm_module_graph_destroy(&consumer_graph);
        cm_module_graph_destroy(&dependency_graph);
        cm_source_set_destroy(&consumer_sources);
        cm_source_set_destroy(&dependency_sources);
        return 0;
    }
    dependency_result = build_graph_with_empty_cfg(&dependency_graph,
        &dependency_sources, dependency_root);
    consumer_result = build_graph_with_empty_cfg(&consumer_graph,
        &consumer_sources, consumer_root);
    artifact_result = cm_dependency_macro_artifact_build(&artifact,
        &dependency_graph, dependency_result.revision, "dep", "rust_dep");
    success = find_module(&consumer_graph, "crate::success");
    collision = find_module(&consumer_graph, "crate::collision");
    ambiguous = find_module(&consumer_graph, "crate::ambiguous");
    globbed = find_module(&consumer_graph, "crate::globbed");
    private_path = find_module(&consumer_graph, "crate::private_path");
    local_name.bytes = (const unsigned char *)"local";
    local_name.length = 5u;
    import_result = cm_import_resolve(&imports, &consumer_graph,
        consumer_result.revision);
    leaf_count = cm_import_leaf_count(&imports);
    saw_grouped_alias = 0;
    {
        size_t index;

        for (index = 0u; index < leaf_count; ++index) {
            if (cm_import_get_leaf(&imports, (uint32_t)index, &leaf)
                && leaf.module == success && !leaf.is_resolved
                && import_string_equals(&imports, leaf.import_name, "local")
                && leaf.segment_count == 3u
                && cm_import_get_leaf_segment(&imports, (uint32_t)index,
                    0u, &segment)
                && segment.length == 3u
                && memcmp(segment.bytes, "dep", 3u) == 0) {
                saw_grouped_alias = 1;
            }
        }
    }
    memset(&imported, 0, sizeof(imported));
    ok &= check(dependency_result.error_count == 0u
        && consumer_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && import_result.revision == consumer_result.revision
        && leaf_count == 7u && saw_grouped_alias,
        "revision-bound structured consumer use leaves were not retained");
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision, success,
            &local_name, &imported) == CM_DEPENDENCY_MACRO_OK
        && imported.consumer_graph == &consumer_graph
        && imported.consumer_revision == consumer_result.revision
        && imported.consumer_module == success
        && imported.import_declaration.source == consumer_root
        && imported.import_declaration.item != CM_AST_ITEM_NONE
        && imported.definition.dependency_graph == &dependency_graph
        && imported.definition.dependency_revision
            == dependency_result.revision
        && imported.definition.declaration.source == dependency_root
        && imported.definition.declaration.item != CM_AST_ITEM_NONE
        && strcmp(imported.definition.crate_identifier, "rust_dep") == 0,
        "explicit grouped dependency import lost cross-graph identity");
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision, collision,
            &local_name, &imported) == CM_DEPENDENCY_MACRO_AMBIGUOUS
        && imported.consumer_graph == NULL,
        "local macro collision was certified as an external import");
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision, ambiguous,
            &local_name, &imported) == CM_DEPENDENCY_MACRO_AMBIGUOUS
        && imported.consumer_graph == NULL,
        "competing unresolved external leaves were not rejected");
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision, globbed,
            &local_name, &imported)
                == CM_DEPENDENCY_MACRO_UNSUPPORTED_IMPORT
        && imported.consumer_graph == NULL,
        "external glob import was treated as an exact leaf certificate");
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision, private_path,
            &local_name, &imported) == CM_DEPENDENCY_MACRO_PRIVATE_PATH
        && imported.consumer_graph == NULL,
        "private dependency path was certified through a consumer use");
    consumer_result = build_graph_with_empty_cfg(&consumer_graph,
        &consumer_sources, consumer_root);
    ok &= check(cm_dependency_macro_artifact_resolve_import(&artifact,
            &consumer_graph, consumer_result.revision - 1u, success,
            &local_name, &imported) == CM_DEPENDENCY_MACRO_STALE_REVISION
        && imported.consumer_graph == NULL,
        "consumer graph rebuild did not invalidate the import certificate");
    ok &= check(strcmp(cm_dependency_macro_status_name(
            CM_DEPENDENCY_MACRO_UNSUPPORTED_IMPORT),
            "unsupported import") == 0,
        "consumer import status name changed");
    cm_import_resolver_destroy(&imports);
    cm_dependency_macro_artifact_destroy(&artifact);
    cm_module_graph_destroy(&consumer_graph);
    cm_module_graph_destroy(&dependency_graph);
    cm_source_set_destroy(&consumer_sources);
    cm_source_set_destroy(&dependency_sources);
    return ok;
}

int main(void)
{
    int ok;

    ok = test_success();
    ok &= test_descendant_private_glob_visibility();
    ok &= test_error("tests/resolve/fixtures/imports_ambiguous/lib.rs",
        CM_IMPORT_ERROR_AMBIGUOUS, 2u);
    ok &= test_error("tests/resolve/fixtures/imports_unresolved/lib.rs",
        CM_IMPORT_ERROR_UNRESOLVED, 1u);
    ok &= test_error("tests/resolve/fixtures/imports_cycle/lib.rs",
        CM_IMPORT_ERROR_CYCLE, 2u);
    ok &= test_error("tests/resolve/fixtures/imports_invalid/lib.rs",
        CM_IMPORT_ERROR_INVALID_TREE, 1u);
    ok &= test_error("tests/resolve/fixtures/imports_item_conflict/lib.rs",
        CM_IMPORT_ERROR_AMBIGUOUS, 1u);
    ok &= test_lookup_errors();
    ok &= test_lookup_keywords_and_namespaces();
    ok &= test_root_self_requires_alias();
    ok &= test_extern_crate_self_alias();
    ok &= test_lookup_intermediate_conflicts();
    ok &= test_lookup_cycle();
    ok &= test_graph_independence();
    ok &= test_revision_invalidation();
    ok &= test_generated_declaration_imports();
    ok &= test_declaration_binding_view();
    ok &= test_enum_variant_imports();
    ok &= test_core_shaped_macro_identity();
    ok &= test_prelude_import_scope();
    ok &= test_builtin_primitive_imports();
    ok &= test_core_target_cfg_import_frontier(
        getenv("RUST190_CORE_ROOT"));
    ok &= test_dependency_macro_artifact();
    ok &= test_dependency_macro_consumer_import();
    if (ok) puts("import resolution tests: ok");
    return ok ? 0 : 1;
}
