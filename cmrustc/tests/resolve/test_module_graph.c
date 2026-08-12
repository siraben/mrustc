#define _POSIX_C_SOURCE 200809L

#include "cm/driver/cfg.h"
#include "cm/resolve/module_graph.h"
#include "cm/resolve/dependency_macro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-module-graph: %s\n", message);
        return 0;
    }
    return 1;
}

static int string_equals(const CmModuleGraph *graph, CmResolveStringId id,
    const char *expected)
{
    char buffer[256];

    return cm_module_graph_copy_string(graph, id, buffer, sizeof(buffer)) &&
        strcmp(buffer, expected) == 0;
}

static int module_path_equals(const CmModuleGraph *graph, CmModuleId id,
    const char *expected)
{
    CmResolveModuleInfo module;

    return cm_module_graph_get_module(graph, id, &module) &&
        string_equals(graph, module.absolute_path, expected);
}

static int ast_name_equals(const CmAst *ast, CmInternId id,
    const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return name != NULL && name->len == length &&
        memcmp(name->bytes, expected, length) == 0;
}

static int module_declaration_is(const CmModuleGraph *graph,
    CmModuleId module, const char *expected_name)
{
    CmResolveModuleInfo information;
    const CmAst *parent_ast;
    const CmAstItem *declaration;

    return cm_module_graph_get_module(graph, module, &information) &&
        information.parent != CM_MODULE_NONE &&
        information.declaration.item != CM_AST_ITEM_NONE &&
        cm_module_graph_borrow_item_ast(graph, information.parent,
            information.declaration, &parent_ast) &&
        (declaration = cm_ast_get_item(parent_ast,
            information.declaration.item)) != NULL &&
        declaration->kind == CM_AST_ITEM_MODULE &&
        ast_name_equals(parent_ast, declaration->name, expected_name);
}

static int namespace_has(const CmModuleGraph *graph, CmModuleId module,
    CmResolveNamespace namespace_kind, const char *name)
{
    CmResolveModuleInfo information;
    uint32_t count;
    uint32_t index;

    if (!cm_module_graph_get_module(graph, module, &information)) return 0;
    count = namespace_kind == CM_RESOLVE_NAMESPACE_TYPE ?
        information.type_count :
        (namespace_kind == CM_RESOLVE_NAMESPACE_VALUE ?
         information.value_count : information.macro_count);
    for (index = 0u; index < count; ++index) {
        CmResolveNamespaceEntry entry;

        if (cm_module_graph_get_namespace_entry(graph, module,
            namespace_kind, index, &entry) &&
            string_equals(graph, entry.name, name)) {
            return 1;
        }
    }
    return 0;
}

static int namespace_get_named(const CmModuleGraph *graph,
    CmModuleId module, CmResolveNamespace namespace_kind, const char *name,
    CmResolveNamespaceEntry *out_entry)
{
    CmResolveModuleInfo information;
    uint32_t count;
    uint32_t index;

    if (out_entry != NULL) memset(out_entry, 0, sizeof(*out_entry));
    if (out_entry == NULL
        || !cm_module_graph_get_module(graph, module, &information)) return 0;
    count = namespace_kind == CM_RESOLVE_NAMESPACE_TYPE ?
        information.type_count :
        (namespace_kind == CM_RESOLVE_NAMESPACE_VALUE ?
         information.value_count : information.macro_count);
    for (index = 0u; index < count; ++index) {
        CmResolveNamespaceEntry entry;

        if (cm_module_graph_get_namespace_entry(graph, module,
                namespace_kind, index, &entry)
            && string_equals(graph, entry.name, name)) {
            *out_entry = entry;
            return 1;
        }
    }
    return 0;
}

static int macro_scope_get_named(const CmModuleGraph *graph,
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
                &entry) && string_equals(graph, entry.name, name)) {
            *out_entry = entry;
            return 1;
        }
    }
    return 0;
}

static int effective_named(const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module, const char *name,
    CmResolveEffectiveItem *out_item, uint32_t *out_index)
{
    CmResolveModuleInfo information;
    const CmAst *ast;
    uint32_t index;

    if (!cm_module_graph_get_module(graph, module, &information)
        || !cm_module_graph_borrow_ast(graph, module, &ast)) return 0;
    for (index = 0u; index < information.effective_item_count; ++index) {
        CmResolveEffectiveItem effective;
        const CmAstItem *item;

        if (cm_module_graph_get_effective_item(graph, revision, module,
                index, &effective) != CM_RESOLVE_VIEW_OK) return 0;
        item = cm_ast_get_item(ast, effective.declaration.item);
        if (item != NULL && ast_name_equals(ast, item->name, name)) {
            if (out_item != NULL) *out_item = effective;
            if (out_index != NULL) *out_index = index;
            return 1;
        }
    }
    return 0;
}

static CmModuleId child_named(const CmModuleGraph *graph,
    CmModuleId parent, const char *name)
{
    CmResolveModuleInfo information;
    uint32_t index;

    if (!cm_module_graph_get_module(graph, parent, &information))
        return CM_MODULE_NONE;
    for (index = 0u; index < information.child_count; ++index) {
        CmModuleId child;
        CmResolveModuleInfo child_information;

        if (cm_module_graph_get_child(graph, parent, index, &child)
            && cm_module_graph_get_module(graph, child, &child_information)
            && string_equals(graph, child_information.name, name)) {
            return child;
        }
    }
    return CM_MODULE_NONE;
}

static int load_and_build(const char *path, CmSourceSet *sources,
    CmModuleGraph *graph, const CmModuleGraphOptions *options,
    CmModuleGraphResult *out_result)
{
    CmSourceId root;
    CmCfgSet cfg;
    CmModuleGraphOptions default_options;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    if (cm_source_load_file(sources, path, &root) != CM_SOURCE_OK) return 0;
    if (options == NULL) {
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&default_options);
        default_options.cfg = &cfg;
        options = &default_options;
    }
    *out_result = cm_module_graph_build(graph, sources, root, options);
    return 1;
}

static int load_and_build_include_fixture(const char *path,
    CmSourceSet *sources, CmModuleGraph *graph,
    CmModuleGraphResult *out_result)
{
    CmCfgSet cfg;
    CmModuleGraphOptions options;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.include_expansion = CM_INCLUDE_EXPANSION_SOURCE_FIXTURE;
    return load_and_build(path, sources, graph, &options, out_result);
}

static CmModuleGraphResult build_with_empty_cfg(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root)
{
    CmCfgSet cfg;
    CmModuleGraphOptions options;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    return cm_module_graph_build(graph, sources, root, &options);
}

static CmModuleGraphResult build_include_fixture(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root)
{
    CmCfgSet cfg;
    CmModuleGraphOptions options;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.include_expansion = CM_INCLUDE_EXPANSION_SOURCE_FIXTURE;
    return cm_module_graph_build(graph, sources, root, &options);
}

static int module_items_are(const CmModuleGraph *graph, CmModuleId module,
    const CmAstItemKind *expected, uint32_t expected_count,
    const CmAst **out_ast)
{
    const CmAst *ast;
    const CmAstItemId *items;
    uint32_t count;
    uint32_t index;

    ast = NULL;
    items = NULL;
    count = 0u;
    if (!cm_module_graph_borrow_ast(graph, module, &ast) || ast == NULL ||
        !cm_module_graph_borrow_items(graph, module, &items, &count) ||
        count != expected_count || (count != 0u && items == NULL)) return 0;
    for (index = 0u; index < count; ++index) {
        const CmAstItem *item;

        item = cm_ast_get_item(ast, items[index]);
        if (item == NULL || item->kind != expected[index]) return 0;
    }
    if (out_ast != NULL) *out_ast = ast;
    return 1;
}

static int active_module_items_are(const CmModuleGraph *graph,
    CmModuleId module, const CmAstItemKind *expected,
    uint32_t expected_count)
{
    CmResolveModuleInfo information;
    const CmResolveItemRef *items;
    const CmAst *ast;
    uint32_t count;
    uint32_t index;

    items = NULL;
    count = 0u;
    if (!cm_module_graph_get_module(graph, module, &information) ||
        !cm_module_graph_borrow_ast(graph, module, &ast) ||
        !cm_module_graph_borrow_active_items(graph, module, &items, &count) ||
        count != expected_count || (count != 0u && items == NULL)) return 0;
    for (index = 0u; index < count; ++index) {
        const CmAstItem *item;

        item = cm_ast_get_item(ast, items[index].item);
        if (items[index].source != information.source || item == NULL ||
            item->kind != expected[index]) return 0;
    }
    return information.active_item_count == expected_count;
}

static int test_ast_views(const CmModuleGraph *graph)
{
    static const CmAstItemKind root_items[] = {
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_USE,
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_FUNCTION
    };
    static const CmAstItemKind alpha_items[] = {
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_FUNCTION,
        CM_AST_ITEM_MODULE
    };
    static const CmAstItemKind deep_items[] = {
        CM_AST_ITEM_TYPE_ALIAS
    };
    static const CmAstItemKind inline_items[] = {
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_USE,
        CM_AST_ITEM_CONST
    };
    static const CmAstItemKind nested_items[] = {
        CM_AST_ITEM_STATIC
    };
    const CmAst *root_ast;
    const CmAst *inline_ast;
    const CmAst *alpha_ast;
    const CmAst *invalid_ast;
    const CmAstItemId *invalid_items;
    const CmResolveItemRef *invalid_active_items;
    uint32_t invalid_count;
    int ok;

    root_ast = NULL;
    inline_ast = NULL;
    alpha_ast = NULL;
    ok = module_items_are(graph, 1u, root_items,
        (uint32_t)CM_ARRAY_LEN(root_items), &root_ast);
    ok &= module_items_are(graph, 2u, alpha_items,
        (uint32_t)CM_ARRAY_LEN(alpha_items), &alpha_ast);
    ok &= module_items_are(graph, 3u, deep_items,
        (uint32_t)CM_ARRAY_LEN(deep_items), NULL);
    ok &= module_items_are(graph, 4u, inline_items,
        (uint32_t)CM_ARRAY_LEN(inline_items), &inline_ast);
    ok &= module_items_are(graph, 5u, nested_items,
        (uint32_t)CM_ARRAY_LEN(nested_items), NULL);
    ok &= root_ast != NULL && inline_ast == root_ast &&
        alpha_ast != NULL && alpha_ast != root_ast;
    ok &= active_module_items_are(graph, 1u, root_items,
        (uint32_t)CM_ARRAY_LEN(root_items));

    invalid_ast = (const CmAst *)(const void *)graph;
    invalid_items = (const CmAstItemId *)(const void *)graph;
    invalid_count = 99u;
    ok &= !cm_module_graph_borrow_ast(graph, CM_MODULE_NONE, &invalid_ast) &&
        invalid_ast == NULL;
    ok &= !cm_module_graph_borrow_items(graph, CM_MODULE_NONE,
        &invalid_items, &invalid_count) && invalid_items == NULL &&
        invalid_count == 0u;
    invalid_items = (const CmAstItemId *)(const void *)graph;
    ok &= !cm_module_graph_borrow_items(graph, 1u, &invalid_items, NULL) &&
        invalid_items == NULL;
    invalid_count = 99u;
    ok &= !cm_module_graph_borrow_items(graph, 1u, NULL, &invalid_count) &&
        invalid_count == 0u;
    invalid_active_items = (const CmResolveItemRef *)(const void *)graph;
    invalid_count = 99u;
    ok &= !cm_module_graph_borrow_active_items(graph, CM_MODULE_NONE,
        &invalid_active_items, &invalid_count) &&
        invalid_active_items == NULL && invalid_count == 0u;
    return ok;
}

static int test_basic_graph(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root;
    CmResolveModuleInfo alpha;
    CmResolveModuleInfo inline_module;
    CmResolveImport import_directive;
    CmModuleId child;
    CmModuleId graph_root;
    CmResolveModuleInfo enumerated;
    int ok;

    ok = check(load_and_build("tests/resolve/fixtures/basic/lib.rs",
        &sources, &graph, NULL, &result), "failed to load basic root");
    if (!ok) return 0;
    ok &= check(result.error_count == 0u, "basic graph has errors");
    ok &= check(cm_module_graph_module_count(&graph) == 5u,
        "basic graph module count differs");
    memset(&enumerated, 0, sizeof(enumerated));
    graph_root = CM_MODULE_NONE;
    ok &= check(cm_module_graph_get_root(&graph, &graph_root) &&
        graph_root == result.root &&
        cm_module_graph_get_module_at(&graph, 0u, &enumerated) &&
        enumerated.id == result.root &&
        cm_module_graph_get_module_at(&graph, 4u, &enumerated) &&
        enumerated.id == 5u,
        "root or deterministic module enumeration differs");
    memset(&enumerated, 0xff, sizeof(enumerated));
    ok &= check(!cm_module_graph_get_module_at(&graph, 5u, &enumerated)
        && enumerated.id == CM_MODULE_NONE,
        "out-of-range module enumeration did not clear output");
    ok &= check(result.root == 1u &&
        module_path_equals(&graph, 1u, "crate") &&
        module_path_equals(&graph, 2u, "crate::alpha") &&
        module_path_equals(&graph, 3u, "crate::alpha::deep") &&
        module_path_equals(&graph, 4u, "crate::inline") &&
        module_path_equals(&graph, 5u, "crate::inline::nested"),
        "module IDs or absolute paths are not deterministic");
    ok &= check(cm_module_graph_get_module(&graph, 1u, &root) &&
        root.declaration.source == 0u &&
        root.declaration.item == CM_AST_ITEM_NONE &&
        root.child_count == 2u && root.type_count == 3u &&
        root.value_count == 2u && root.macro_count == 0u &&
        root.import_count == 1u,
        "root namespace or child counts differ");
    ok &= check(cm_module_graph_get_child(&graph, 1u, 0u, &child) &&
        child == 2u && cm_module_graph_get_child(&graph, 1u, 1u, &child) &&
        child == 4u && cm_module_graph_get_module(&graph, 2u, &alpha) &&
        alpha.parent == 1u && alpha.child_count == 1u,
        "module parent or child ordering differs");
    ok &= check(module_declaration_is(&graph, 2u, "alpha") &&
        module_declaration_is(&graph, 3u, "deep") &&
        module_declaration_is(&graph, 4u, "inline") &&
        module_declaration_is(&graph, 5u, "nested"),
        "module declaration identities do not point into parent syntax");
    ok &= check(namespace_has(&graph, 1u, CM_RESOLVE_NAMESPACE_TYPE,
        "alpha") && namespace_has(&graph, 1u, CM_RESOLVE_NAMESPACE_TYPE,
        "inline") && namespace_has(&graph, 1u,
        CM_RESOLVE_NAMESPACE_TYPE, "Root") &&
        namespace_has(&graph, 1u, CM_RESOLVE_NAMESPACE_VALUE, "Root") &&
        namespace_has(&graph, 1u, CM_RESOLVE_NAMESPACE_VALUE, "root_fn"),
        "root declarations were assigned to the wrong namespace");
    ok &= check(cm_module_graph_get_import(&graph, 1u, 0u,
        &import_directive) && string_equals(&graph, import_directive.tree,
        "alpha::Thing"), "root import was not preserved");
    ok &= check(cm_module_graph_get_module(&graph, 4u, &inline_module) &&
        inline_module.is_inline && inline_module.source == root.source &&
        inline_module.child_count == 1u && inline_module.import_count == 1u,
        "inline module hierarchy or source ownership differs");
    ok &= check(sources.length == 4u,
        "external sources were not loaded exactly once");
    cm_source_set_destroy(&sources);
    ok &= check(test_ast_views(&graph),
        "graph-owned AST or direct item views differ after source teardown");
    cm_module_graph_destroy(&graph);
    return ok;
}

static int test_error_fixture_with_include_mode(const char *path,
    CmResolveErrorKind expected_kind, CmIncludeExpansionMode include_mode)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    CmCfgSet cfg;
    CmModuleGraphOptions options;
    int ok;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.include_expansion = include_mode;
    ok = check(load_and_build(path, &sources, &graph, &options, &result),
        "failed to load error fixture");
    if (!ok) return 0;
    ok &= check(result.error_count == 1u &&
        cm_module_graph_get_error(&graph, 0u, &error) &&
        error.kind == expected_kind
        && (error.module_path != 0u
            || ((expected_kind == CM_RESOLVE_ERROR_ITEM_MACRO
                    || expected_kind == CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED
                    || expected_kind == CM_RESOLVE_ERROR_INCLUDE_CYCLE
                    || expected_kind == CM_RESOLVE_ERROR_INCLUDE_LIMIT)
                && error.span.source != 0u && error.detail_a != 0u)),
        "structured resolver error differs");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_error_fixture(const char *path,
    CmResolveErrorKind expected_kind)
{
    return test_error_fixture_with_include_mode(path, expected_kind,
        CM_INCLUDE_EXPANSION_DISABLED);
}

static int item_ref_equal(CmResolveItemRef left, CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static int effective_item_is_zero(const CmResolveEffectiveItem *item)
{
    CmResolveItemRef none;

    memset(&none, 0, sizeof(none));
    return item_ref_equal(item->declaration, none) &&
        item_ref_equal(item->provenance.source_item, none) &&
        item_ref_equal(item->provenance.macro_invocation, none) &&
        item_ref_equal(item->provenance.macro_definition, none) &&
        item->provenance.expansion_depth == 0u && item->span.source == 0u &&
        item->span.start == 0u && item->span.end == 0u &&
        item->item_kind == 0 && item->visibility == 0 &&
        item->attribute_count == 0u && item->id == 0u &&
        item->child_kind == CM_EXPANDED_CHILD_NONE &&
        item->child_count == 0u && item->variant_count == 0u &&
        !item->is_generated;
}

static int effective_attribute_is_zero(
    const CmResolveEffectiveAttribute *attribute)
{
    CmResolveItemRef none;

    memset(&none, 0, sizeof(none));
    return attribute->source == 0u && attribute->source_attribute == 0u &&
        item_ref_equal(attribute->owner, none)
        && item_ref_equal(attribute->owner_variant.enumeration, none)
        && attribute->owner_variant.index == 0u && attribute->style == 0 &&
        attribute->span.source == 0u && attribute->span.start == 0u &&
        attribute->span.end == 0u && attribute->metadata == 0u &&
        attribute->expansion_depth == 0u;
}

static int test_effective_enum_variants(void)
{
    static const unsigned char source_text[] =
        "pub enum Choice {\n"
        "    #[cfg(any())] Hidden,\n"
        "    #[cfg_attr(all(), doc = \"kept\")] Unit,\n"
        "    Tuple(u8),\n"
        "    Named { value: u8 },\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmResolveEffectiveItem enumeration;
    CmResolveEffectiveVariant unit;
    CmResolveEffectiveVariant tuple;
    CmResolveEffectiveVariant named;
    CmResolveEffectiveVariant missing;
    CmResolveEffectiveAttribute attribute;
    CmResolveEffectiveAttribute missing_attribute;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/effective-enum/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add effective enum source");
    result = cm_module_graph_build(&graph, &sources, root, &options);
    memset(&enumeration, 0, sizeof(enumeration));
    ok &= check(result.error_count == 0u
        && effective_named(&graph, result.revision, result.root, "Choice",
            &enumeration, NULL)
        && enumeration.item_kind == CM_AST_ITEM_ENUM
        && enumeration.variant_count == 3u,
        "effective enum did not cfg-filter its variant view");
    memset(&unit, 0, sizeof(unit));
    memset(&tuple, 0, sizeof(tuple));
    memset(&named, 0, sizeof(named));
    ok &= check(cm_module_graph_get_effective_variant(&graph,
            result.revision, result.root, enumeration.id, 0u, &unit)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_variant(&graph,
            result.revision, result.root, enumeration.id, 1u, &tuple)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_variant(&graph,
            result.revision, result.root, enumeration.id, 2u, &named)
            == CM_RESOLVE_VIEW_OK
        && item_ref_equal(unit.declaration.enumeration,
            enumeration.declaration)
        && unit.declaration.index == 1u && tuple.declaration.index == 2u
        && named.declaration.index == 3u
        && string_equals(&graph, unit.name, "Unit")
        && string_equals(&graph, tuple.name, "Tuple")
        && string_equals(&graph, named.name, "Named")
        && unit.form == CM_AST_FIELDS_UNIT
        && tuple.form == CM_AST_FIELDS_TUPLE
        && named.form == CM_AST_FIELDS_NAMED
        && unit.attribute_count == 1u && tuple.attribute_count == 0u
        && unit.span.source == root && !unit.is_generated,
        "effective variants lost declaration index, form, or span identity");
    memset(&attribute, 0, sizeof(attribute));
    ok &= check(cm_module_graph_get_effective_variant_attribute(&graph,
            result.revision, result.root, enumeration.id, 0u, 0u,
            &attribute) == CM_RESOLVE_VIEW_OK
        && item_ref_equal(attribute.owner, enumeration.declaration)
        && item_ref_equal(attribute.owner_variant.enumeration,
            enumeration.declaration)
        && attribute.owner_variant.index == 1u
        && string_equals(&graph, attribute.metadata, "doc = \"kept\"")
        && attribute.expansion_depth == 1u,
        "effective variant attribute lost cfg_attr ownership");
    memset(&missing, 0xa5, sizeof(missing));
    memset(&missing_attribute, 0xa5, sizeof(missing_attribute));
    ok &= check(cm_module_graph_get_effective_variant(&graph,
            result.revision, result.root, enumeration.id, 3u, &missing)
            == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && missing.declaration.enumeration.source == 0u
        && cm_module_graph_get_effective_variant_attribute(&graph,
            result.revision + 1u, result.root, enumeration.id, 0u, 0u,
            &missing_attribute) == CM_RESOLVE_VIEW_STALE_REVISION
        && missing_attribute.source == 0u,
        "effective variant views did not fail closed");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_effective_views_and_revisions(void)
{
    static const unsigned char source_text[] =
        "#![allow(dead_code)]\n"
        "#[cfg(feature = \"absent\")] struct Hidden;\n"
        "#[doc = \"kept\"] pub fn live() {}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult first;
    CmModuleGraphResult second;
    CmModuleGraphResult failed;
    CmResolveModuleInfo information;
    CmResolveNamespaceEntry namespace_entry;
    CmResolveEffectiveItem item;
    CmResolveEffectiveAttribute attribute;
    CmResolveEffectiveAttribute inner_attribute;
    CmResolveViewStatus status;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/effective/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add effective-view source");
    cm_module_graph_options_init(&options);
    cm_cfg_set_init(&cfg);
    options.cfg = &cfg;
    first = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(first.root != CM_MODULE_NONE && first.error_count == 0u &&
        first.revision != CM_MODULE_GRAPH_REVISION_NONE &&
        cm_module_graph_revision(&graph) == first.revision,
        "first graph revision or build result differs");
    ok &= check(cm_module_graph_get_module(&graph, first.root,
        &information) && information.active_item_count == 1u &&
        information.effective_item_count == 1u
        && information.inner_attribute_count == 1u,
        "effective item count differs from cfg-active source items");
    memset(&item, 0xa5, sizeof(item));
    status = cm_module_graph_get_effective_item(&graph, first.revision,
        first.root, 0u, &item);
    ok &= check(status == CM_RESOLVE_VIEW_OK &&
        item.declaration.source == root &&
        item.declaration.item != CM_AST_ITEM_NONE &&
        item_ref_equal(item.provenance.source_item, item.declaration) &&
        item.provenance.macro_invocation.item == CM_AST_ITEM_NONE &&
        item.provenance.macro_definition.item == CM_AST_ITEM_NONE &&
        item.provenance.expansion_depth == 0u && !item.is_generated &&
        item.item_kind == CM_AST_ITEM_FUNCTION &&
        item.visibility == CM_AST_VIS_PUBLIC && item.attribute_count == 1u &&
        item.span.source == root,
        "effective source-item identity or provenance differs");
    ok &= check(cm_module_graph_get_namespace_entry(&graph, first.root,
        CM_RESOLVE_NAMESPACE_VALUE, 0u, &namespace_entry) &&
        item_ref_equal(namespace_entry.declaration, item.declaration) &&
        string_equals(&graph, namespace_entry.name, "live"),
        "namespace was not constructed from the effective item identity");
    memset(&attribute, 0xa5, sizeof(attribute));
    status = cm_module_graph_get_effective_attribute(&graph,
        first.revision, first.root, 0u, 0u, &attribute);
    ok &= check(status == CM_RESOLVE_VIEW_OK && attribute.source == root &&
        attribute.source_attribute != 0u &&
        item_ref_equal(attribute.owner, item.declaration) &&
        attribute.style == CM_AST_ATTR_OUTER &&
        attribute.span.source == root && attribute.metadata != 0u &&
        string_equals(&graph, attribute.metadata, "doc = \"kept\"") &&
        attribute.expansion_depth == 0u,
        "effective attribute metadata or provenance differs");
    memset(&inner_attribute, 0xa5, sizeof(inner_attribute));
    status = cm_module_graph_get_effective_inner_attribute(&graph,
        first.revision, first.root, 0u, &inner_attribute);
    ok &= check(status == CM_RESOLVE_VIEW_OK
        && inner_attribute.source == root
        && inner_attribute.source_attribute != 0u
        && inner_attribute.owner.source == 0u
        && inner_attribute.owner.item == CM_AST_ITEM_NONE
        && inner_attribute.style == CM_AST_ATTR_INNER
        && inner_attribute.span.source == root
        && inner_attribute.metadata != CM_RESOLVE_STRING_NONE
        && string_equals(&graph, inner_attribute.metadata,
            "allow(dead_code)")
        && inner_attribute.expansion_depth == 0u,
        "effective inner attribute metadata or provenance differs");

    second = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(second.root != CM_MODULE_NONE && second.error_count == 0u &&
        second.revision != first.revision &&
        cm_module_graph_revision(&graph) == second.revision,
        "successful rebuild did not advance graph revision");
    memset(&item, 0xa5, sizeof(item));
    status = cm_module_graph_get_effective_item(&graph, first.revision,
        first.root, 0u, &item);
    ok &= check(status == CM_RESOLVE_VIEW_STALE_REVISION &&
        effective_item_is_zero(&item),
        "stale effective item access did not fail with cleared output");
    memset(&attribute, 0xa5, sizeof(attribute));
    status = cm_module_graph_get_effective_attribute(&graph,
        first.revision, first.root, 0u, 0u, &attribute);
    ok &= check(status == CM_RESOLVE_VIEW_STALE_REVISION &&
        effective_attribute_is_zero(&attribute),
        "stale effective attribute access did not clear output");
    memset(&inner_attribute, 0xa5, sizeof(inner_attribute));
    status = cm_module_graph_get_effective_inner_attribute(&graph,
        first.revision, first.root, 0u, &inner_attribute);
    ok &= check(status == CM_RESOLVE_VIEW_STALE_REVISION
        && effective_attribute_is_zero(&inner_attribute),
        "stale effective inner-attribute access did not clear output");
    ok &= check(cm_module_graph_get_effective_item(&graph,
        second.revision, second.root, 0u, &item) == CM_RESOLVE_VIEW_OK &&
        cm_module_graph_get_effective_attribute(&graph, second.revision,
            second.root, 0u, 0u, &attribute) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_inner_attribute(&graph,
            second.revision, second.root, 0u,
            &inner_attribute) == CM_RESOLVE_VIEW_OK,
        "current effective view failed after rebuild");
    memset(&inner_attribute, 0xa5, sizeof(inner_attribute));
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            second.revision, CM_MODULE_NONE, 0u,
            &inner_attribute) == CM_RESOLVE_VIEW_INVALID_MODULE
        && effective_attribute_is_zero(&inner_attribute)
        && cm_module_graph_get_effective_inner_attribute(&graph,
            second.revision, second.root, 0u,
            NULL) == CM_RESOLVE_VIEW_INVALID_ARGUMENT,
        "invalid effective inner-attribute arguments were accepted");
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            second.revision, second.root, 0u,
            &inner_attribute) == CM_RESOLVE_VIEW_OK,
        "current inner-attribute view was not restored after argument tests");

    cm_source_set_destroy(&sources);
    ok &= check(string_equals(&graph, attribute.metadata,
        "doc = \"kept\"")
        && string_equals(&graph, inner_attribute.metadata,
            "allow(dead_code)") &&
        cm_module_graph_get_effective_item(&graph, second.revision,
            second.root, 0u, &item) == CM_RESOLVE_VIEW_OK,
        "effective view retained source-set storage");

    failed = cm_module_graph_build(&graph, NULL, root, &options);
    ok &= check(failed.root == CM_MODULE_NONE && failed.error_count == 1u &&
        failed.revision != second.revision &&
        cm_module_graph_revision(&graph) == failed.revision,
        "failed rebuild did not advance graph revision");
    memset(&item, 0xa5, sizeof(item));
    ok &= check(cm_module_graph_get_effective_item(&graph, second.revision,
        second.root, 0u, &item) == CM_RESOLVE_VIEW_STALE_REVISION &&
        effective_item_is_zero(&item),
        "failed rebuild did not invalidate the prior item view");
    memset(&item, 0xa5, sizeof(item));
    ok &= check(cm_module_graph_get_effective_item(&graph, failed.revision,
        CM_MODULE_NONE, 0u, &item) == CM_RESOLVE_VIEW_FAILED_BUILD &&
        effective_item_is_zero(&item),
        "failed current graph did not reject its effective snapshot");
    memset(&attribute, 0xa5, sizeof(attribute));
    ok &= check(cm_module_graph_get_effective_attribute(&graph,
        CM_MODULE_GRAPH_REVISION_NONE, CM_MODULE_NONE, 0u, 0u,
        &attribute) == CM_RESOLVE_VIEW_INVALID_ARGUMENT &&
        effective_attribute_is_zero(&attribute),
        "invalid revision did not fail with cleared attribute output");
    memset(&inner_attribute, 0xa5, sizeof(inner_attribute));
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            failed.revision, CM_MODULE_NONE, 0u,
            &inner_attribute) == CM_RESOLVE_VIEW_FAILED_BUILD
        && effective_attribute_is_zero(&inner_attribute),
        "failed current graph exposed an inner-attribute snapshot");
    memset(&inner_attribute, 0xa5, sizeof(inner_attribute));
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            CM_MODULE_GRAPH_REVISION_NONE, CM_MODULE_NONE, 0u,
            &inner_attribute) == CM_RESOLVE_VIEW_INVALID_ARGUMENT
        && effective_attribute_is_zero(&inner_attribute),
        "zero revision did not clear effective inner-attribute output");
    ok &= check(strcmp(cm_resolve_view_status_name(
        CM_RESOLVE_VIEW_STALE_REVISION), "stale revision") == 0,
        "effective view status name differs");
    cm_module_graph_destroy(&graph);
    return ok;
}

static int test_recursive_effective_trait_impl_children(void)
{
    static const unsigned char source_text[] =
        "#[cfg_attr(enabled, doc = \"parent\")]\n"
        "trait Trait {\n"
        " #[cfg(disabled)] type Hidden;\n"
        " #[cfg(enabled)] type Assoc;\n"
        " #[cfg(disabled)] fn skipped(&self);\n"
        " #[cfg_attr(enabled, inline)]\n"
        " #[allow(dead_code)] fn call(&self);\n"
        "}\n"
        "struct Subject;\n"
        "impl Trait for Subject {\n"
        " #[cfg(disabled)] type Hidden = u16;\n"
        " #[cfg(enabled)] type Assoc = u8;\n"
        " #[cfg(disabled)] fn skipped(&self) {}\n"
        " #[cfg_attr(enabled, inline)]\n"
        " #[allow(dead_code)] fn call(&self) {}\n"
        "}\n";
    static const CmCfgEntry entries[] = {
        { "enabled", NULL }
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult first;
    CmModuleGraphResult second;
    CmModuleGraphResult failed;
    CmResolveModuleInfo information;
    const CmAst *ast;
    const CmAstItem *ast_item;
    CmResolveEffectiveItem trait_item;
    CmResolveEffectiveItem impl_item;
    CmResolveEffectiveItem trait_assoc;
    CmResolveEffectiveItem trait_call;
    CmResolveEffectiveItem impl_assoc;
    CmResolveEffectiveItem impl_call;
    CmResolveEffectiveItem output_item;
    CmResolveEffectiveAttribute parent_attribute;
    CmResolveEffectiveAttribute cfg_attr_attribute;
    CmResolveEffectiveAttribute ordinary_attribute;
    CmResolveEffectiveAttribute output_attribute;
    CmResolveViewStatus status;
    CmResolveEffectiveItemId effective_ids[6];
    uint32_t first_trait_id;
    uint32_t first_impl_id;
    uint32_t index;
    uint32_t other_index;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/effective-children/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add recursive effective-child source");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = entries;
    cfg.environment.entry_count = CM_ARRAY_LEN(entries);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    first = cm_module_graph_build(&graph, &sources, root, &options);
    memset(&information, 0, sizeof(information));
    memset(&trait_item, 0, sizeof(trait_item));
    memset(&impl_item, 0, sizeof(impl_item));
    ast = NULL;
    ok &= check(first.root != CM_MODULE_NONE && first.error_count == 0u
        && cm_module_graph_get_module(&graph, first.root, &information)
        && information.effective_item_count == 3u
        && cm_module_graph_borrow_ast(&graph, first.root, &ast)
        && ast != NULL,
        "recursive effective-child fixture did not build one root view");
    for (index = 0u; index < information.effective_item_count; ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, first.revision,
                first.root, index, &item) != CM_RESOLVE_VIEW_OK) {
            continue;
        }
        if (item.item_kind == CM_AST_ITEM_TRAIT) trait_item = item;
        if (item.item_kind == CM_AST_ITEM_IMPL) impl_item = item;
    }
    ok &= check(trait_item.id != 0u && impl_item.id != 0u
        && trait_item.id != impl_item.id
        && trait_item.child_kind == CM_EXPANDED_CHILD_TRAIT
        && trait_item.child_count == 2u
        && trait_item.attribute_count == 1u
        && impl_item.child_kind == CM_EXPANDED_CHILD_IMPL
        && impl_item.child_count == 2u
        && impl_item.attribute_count == 0u,
        "trait or impl root lost recursive effective-child metadata");

    memset(&trait_assoc, 0, sizeof(trait_assoc));
    memset(&trait_call, 0, sizeof(trait_call));
    memset(&impl_assoc, 0, sizeof(impl_assoc));
    memset(&impl_call, 0, sizeof(impl_call));
    ok &= check(cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, trait_item.id, 0u,
            &trait_assoc) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, trait_item.id, 1u,
            &trait_call) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, impl_item.id, 0u,
            &impl_assoc) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, impl_item.id, 1u,
            &impl_call) == CM_RESOLVE_VIEW_OK,
        "active trait or impl children were not recursively addressable");
    effective_ids[0] = trait_item.id;
    effective_ids[1] = impl_item.id;
    effective_ids[2] = trait_assoc.id;
    effective_ids[3] = trait_call.id;
    effective_ids[4] = impl_assoc.id;
    effective_ids[5] = impl_call.id;
    for (index = 0u; index < CM_ARRAY_LEN(effective_ids); ++index) {
        for (other_index = index + 1u;
             other_index < CM_ARRAY_LEN(effective_ids); ++other_index) {
            ok &= check(effective_ids[index] != effective_ids[other_index],
                "effective root and child IDs were not pairwise distinct");
        }
    }
    ast_item = ast == NULL ? NULL
        : cm_ast_get_item(ast, trait_assoc.declaration.item);
    ok &= check(ast_item != NULL
        && ast_name_equals(ast, ast_item->name, "Assoc")
        && trait_assoc.id != 0u && trait_assoc.child_count == 0u
        && trait_assoc.child_kind == CM_EXPANDED_CHILD_NONE
        && trait_assoc.item_kind == CM_AST_ITEM_TYPE_ALIAS
        && trait_assoc.attribute_count == 0u
        && trait_assoc.declaration.source == root
        && item_ref_equal(trait_assoc.provenance.source_item,
            trait_assoc.declaration)
        && trait_assoc.provenance.expansion_depth == 0u
        && !trait_assoc.is_generated && trait_assoc.span.source == root
        && trait_assoc.span.start == ast_item->span.start
        && trait_assoc.span.end == ast_item->span.end,
        "enabled trait associated type lost identity or source provenance");
    ast_item = ast == NULL ? NULL
        : cm_ast_get_item(ast, trait_call.declaration.item);
    ok &= check(ast_item != NULL
        && ast_name_equals(ast, ast_item->name, "call")
        && trait_call.id != 0u && trait_call.item_kind == CM_AST_ITEM_FUNCTION
        && trait_call.attribute_count == 2u
        && trait_call.declaration.source == root
        && item_ref_equal(trait_call.provenance.source_item,
            trait_call.declaration)
        && trait_call.provenance.expansion_depth == 0u
        && !trait_call.is_generated && trait_call.span.source == root
        && trait_call.span.start == ast_item->span.start
        && trait_call.span.end == ast_item->span.end,
        "enabled trait method lost identity or source provenance");
    ast_item = ast == NULL ? NULL
        : cm_ast_get_item(ast, impl_assoc.declaration.item);
    ok &= check(ast_item != NULL
        && ast_name_equals(ast, ast_item->name, "Assoc")
        && impl_assoc.id != 0u
        && impl_assoc.item_kind == CM_AST_ITEM_TYPE_ALIAS
        && impl_assoc.attribute_count == 0u
        && item_ref_equal(impl_assoc.provenance.source_item,
            impl_assoc.declaration)
        && impl_assoc.span.source == root
        && impl_assoc.span.start == ast_item->span.start
        && impl_assoc.span.end == ast_item->span.end,
        "enabled impl associated type lost identity or source provenance");
    ast_item = ast == NULL ? NULL
        : cm_ast_get_item(ast, impl_call.declaration.item);
    ok &= check(ast_item != NULL
        && ast_name_equals(ast, ast_item->name, "call")
        && impl_call.id != 0u && impl_call.item_kind == CM_AST_ITEM_FUNCTION
        && impl_call.attribute_count == 2u
        && item_ref_equal(impl_call.provenance.source_item,
            impl_call.declaration)
        && impl_call.span.source == root
        && impl_call.span.start == ast_item->span.start
        && impl_call.span.end == ast_item->span.end,
        "enabled impl method lost identity or source provenance");

    memset(&parent_attribute, 0, sizeof(parent_attribute));
    memset(&cfg_attr_attribute, 0, sizeof(cfg_attr_attribute));
    memset(&ordinary_attribute, 0, sizeof(ordinary_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, trait_item.id, 0u,
            &parent_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, parent_attribute.metadata,
            "doc = \"parent\"")
        && parent_attribute.expansion_depth == 1u
        && item_ref_equal(parent_attribute.owner, trait_item.declaration)
        && cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, trait_call.id, 0u,
            &cfg_attr_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, cfg_attr_attribute.metadata, "inline")
        && cfg_attr_attribute.expansion_depth == 1u
        && item_ref_equal(cfg_attr_attribute.owner,
            trait_call.declaration)
        && cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, trait_call.id, 1u,
            &ordinary_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, ordinary_attribute.metadata,
            "allow(dead_code)")
        && ordinary_attribute.expansion_depth == 0u
        && item_ref_equal(ordinary_attribute.owner,
            trait_call.declaration)
        && parent_attribute.source == root
        && cfg_attr_attribute.source == root
        && ordinary_attribute.source == root
        && parent_attribute.source_attribute != 0u
        && cfg_attr_attribute.source_attribute != 0u
        && ordinary_attribute.source_attribute != 0u
        && parent_attribute.style == CM_AST_ATTR_OUTER
        && cfg_attr_attribute.style == CM_AST_ATTR_OUTER
        && ordinary_attribute.style == CM_AST_ATTR_OUTER,
        "effective child attributes lost order, metadata, or provenance");
    memset(&cfg_attr_attribute, 0, sizeof(cfg_attr_attribute));
    memset(&ordinary_attribute, 0, sizeof(ordinary_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, impl_call.id, 0u,
            &cfg_attr_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, cfg_attr_attribute.metadata, "inline")
        && cfg_attr_attribute.expansion_depth == 1u
        && item_ref_equal(cfg_attr_attribute.owner, impl_call.declaration)
        && cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, impl_call.id, 1u,
            &ordinary_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, ordinary_attribute.metadata,
            "allow(dead_code)")
        && ordinary_attribute.expansion_depth == 0u
        && item_ref_equal(ordinary_attribute.owner,
            impl_call.declaration),
        "impl method effective attributes lost order or provenance");

    memset(&output_item, 0xa5, sizeof(output_item));
    status = cm_module_graph_get_effective_child(&graph, first.revision,
        first.root, trait_item.id, trait_item.child_count, &output_item);
    ok &= check(status == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && effective_item_is_zero(&output_item),
        "out-of-range effective child access did not clear output");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, 0u, 0u,
            &output_item) == CM_RESOLVE_VIEW_INVALID_ARGUMENT
        && effective_item_is_zero(&output_item),
        "zero effective parent ID was accepted");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, first.revision,
            CM_MODULE_NONE, trait_item.id, 0u,
            &output_item) == CM_RESOLVE_VIEW_INVALID_MODULE
        && effective_item_is_zero(&output_item),
        "invalid effective-child module did not clear output");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, 999999u, 0u,
            &output_item) == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && effective_item_is_zero(&output_item)
        && cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, trait_item.id, 0u,
            NULL) == CM_RESOLVE_VIEW_INVALID_ARGUMENT,
        "invalid effective parent ID or null output was accepted");
    memset(&output_attribute, 0xa5, sizeof(output_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, trait_call.id,
            trait_call.attribute_count,
            &output_attribute) == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && effective_attribute_is_zero(&output_attribute),
        "out-of-range effective item attribute did not clear output");
    memset(&output_attribute, 0xa5, sizeof(output_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, 0u, 0u,
            &output_attribute) == CM_RESOLVE_VIEW_INVALID_ARGUMENT
        && effective_attribute_is_zero(&output_attribute),
        "zero effective item ID was accepted for attribute lookup");
    memset(&output_attribute, 0xa5, sizeof(output_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, 999999u, 0u,
            &output_attribute) == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && effective_attribute_is_zero(&output_attribute)
        && cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, trait_call.id, 0u,
            NULL) == CM_RESOLVE_VIEW_INVALID_ARGUMENT,
        "invalid effective item ID or null attribute output was accepted");

    first_trait_id = trait_item.id;
    first_impl_id = impl_item.id;
    second = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(second.root != CM_MODULE_NONE && second.error_count == 0u
        && second.revision != first.revision,
        "recursive effective-child rebuild did not advance revision");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, first.revision,
            first.root, first_trait_id, 0u,
            &output_item) == CM_RESOLVE_VIEW_STALE_REVISION
        && effective_item_is_zero(&output_item),
        "stale effective child access did not clear output");
    memset(&output_attribute, 0xa5, sizeof(output_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            first.revision, first.root, first_impl_id, 0u,
            &output_attribute) == CM_RESOLVE_VIEW_STALE_REVISION
        && effective_attribute_is_zero(&output_attribute),
        "stale effective item attribute access did not clear output");

    failed = cm_module_graph_build(&graph, NULL, root, &options);
    ok &= check(failed.root == CM_MODULE_NONE && failed.error_count == 1u
        && failed.revision != second.revision,
        "failed recursive-view rebuild did not advance revision");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, failed.revision,
            first.root, first_trait_id, 0u,
            &output_item) == CM_RESOLVE_VIEW_FAILED_BUILD
        && effective_item_is_zero(&output_item),
        "failed build published an effective child");
    memset(&output_attribute, 0xa5, sizeof(output_attribute));
    ok &= check(cm_module_graph_get_effective_item_attribute(&graph,
            failed.revision, first.root, first_impl_id, 0u,
            &output_attribute) == CM_RESOLVE_VIEW_FAILED_BUILD
        && effective_attribute_is_zero(&output_attribute),
        "failed build published an effective child attribute");
    memset(&output_item, 0xa5, sizeof(output_item));
    ok &= check(cm_module_graph_get_effective_child(&graph, second.revision,
            second.root, first_trait_id, 0u,
            &output_item) == CM_RESOLVE_VIEW_STALE_REVISION
        && effective_item_is_zero(&output_item),
        "failed build did not stale the prior recursive view");

    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_late_failure_hides_effective_snapshot(void)
{
    static const unsigned char source_text[] =
        "struct Before; use crate::Before; "
        "mod cmrustc_definitely_missing_late_child;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo information;
    CmResolveEffectiveItem item;
    CmResolveNamespaceEntry entry;
    CmResolveImport import_directive;
    const CmResolveItemRef *active_items;
    uint32_t active_count;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/late-failure/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add late-failure source");
    result = build_with_empty_cfg(&graph, &sources, root);
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 1u &&
        cm_module_graph_get_module(&graph, result.root, &information) &&
        information.effective_item_count != 0u
        && information.type_count != 0u && information.import_count != 0u,
        "late module failure did not construct the partial diagnostic graph");
    memset(&item, 0xa5, sizeof(item));
    ok &= check(cm_module_graph_get_effective_item(&graph, result.revision,
        result.root, 0u, &item) == CM_RESOLVE_VIEW_FAILED_BUILD &&
        effective_item_is_zero(&item),
        "late failed build published a partial effective snapshot");
    active_items = (const CmResolveItemRef *)(const void *)&graph;
    active_count = 99u;
    memset(&entry, 0xa5, sizeof(entry));
    memset(&import_directive, 0xa5, sizeof(import_directive));
    ok &= check(!cm_module_graph_borrow_active_items(&graph, result.root,
            &active_items, &active_count)
        && active_items == NULL && active_count == 0u
        && !cm_module_graph_get_namespace_entry(&graph, result.root,
            CM_RESOLVE_NAMESPACE_TYPE, 0u, &entry)
        && entry.name == CM_RESOLVE_STRING_NONE
        && entry.declaration.source == 0u
        && entry.declaration.item == CM_AST_ITEM_NONE
        && entry.item_kind == 0
        && entry.visibility == 0
        && !cm_module_graph_get_import(&graph, result.root, 0u,
            &import_directive)
        && import_directive.declaration.source == 0u
        && import_directive.declaration.item == CM_AST_ITEM_NONE
        && import_directive.tree == CM_RESOLVE_STRING_NONE
        && import_directive.visibility == 0,
        "late failed build exposed or retained a semantic snapshot");
    ok &= check(strcmp(cm_resolve_view_status_name(
        CM_RESOLVE_VIEW_FAILED_BUILD), "failed graph build") == 0,
        "failed-build effective view status name differs");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_effective_inner_attributes(void)
{
    static const CmAstItemKind root_effective_kinds[] = {
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_STRUCT
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmModuleId inline_module;
    CmModuleId external_module;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo inline_information;
    CmResolveModuleInfo external_information;
    CmResolveEffectiveAttribute root_attribute;
    CmResolveEffectiveAttribute inline_attribute;
    CmResolveEffectiveAttribute external_attribute;
    CmResolveEffectiveItem hidden_replacement;
    CmResolveEffectiveItem conditional_replacement;
    int ok;

    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    ok = check(load_and_build(
        "tests/resolve/fixtures/inner-attributes/lib.rs", &sources,
        &graph, &options, &result),
        "failed to load inner-attribute fixture");
    if (!ok) return 0;
    inline_module = child_named(&graph, result.root, "inline");
    external_module = child_named(&graph, result.root, "child");
    memset(&root_information, 0, sizeof(root_information));
    memset(&inline_information, 0, sizeof(inline_information));
    memset(&external_information, 0, sizeof(external_information));
    memset(&root_attribute, 0, sizeof(root_attribute));
    memset(&inline_attribute, 0, sizeof(inline_attribute));
    memset(&external_attribute, 0, sizeof(external_attribute));
    memset(&hidden_replacement, 0, sizeof(hidden_replacement));
    memset(&conditional_replacement, 0,
        sizeof(conditional_replacement));
    ok &= check(result.error_count == 0u
        && inline_module != CM_MODULE_NONE
        && external_module != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && cm_module_graph_get_module(&graph, inline_module,
            &inline_information)
        && cm_module_graph_get_module(&graph, external_module,
            &external_information)
        && root_information.inner_attribute_count == 1u
        && inline_information.inner_attribute_count == 1u
        && external_information.inner_attribute_count == 1u
        && root_information.child_count == 2u
        && root_information.type_count == 4u
        && root_information.active_item_count == 4u
        && root_information.effective_item_count == 4u
        && child_named(&graph, result.root, "hidden") == CM_MODULE_NONE
        && child_named(&graph, result.root,
            "conditional") == CM_MODULE_NONE
        && active_module_items_are(&graph, result.root,
            root_effective_kinds, 4u)
        && effective_named(&graph, result.revision, result.root, "hidden",
            &hidden_replacement, NULL)
        && hidden_replacement.item_kind == CM_AST_ITEM_STRUCT
        && effective_named(&graph, result.revision, result.root,
            "conditional", &conditional_replacement, NULL)
        && conditional_replacement.item_kind == CM_AST_ITEM_STRUCT,
        "module inner-attribute counts or hierarchy differ");
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            result.revision, result.root, 0u,
            &root_attribute) == CM_RESOLVE_VIEW_OK
        && root_attribute.source == root_information.source
        && root_attribute.style == CM_AST_ATTR_INNER
        && root_attribute.owner.source == 0u
        && root_attribute.owner.item == CM_AST_ITEM_NONE
        && root_attribute.expansion_depth == 1u
        && string_equals(&graph, root_attribute.metadata, "no_core"),
        "effective crate inner attribute lost cfg_attr provenance");
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            result.revision, inline_module, 0u,
            &inline_attribute) == CM_RESOLVE_VIEW_OK
        && inline_attribute.source == inline_information.source
        && item_ref_equal(inline_attribute.owner,
            inline_information.declaration)
        && inline_attribute.expansion_depth == 1u
        && string_equals(&graph, inline_attribute.metadata,
            "allow(dead_code)"),
        "inline-module inner attribute has the wrong owner");
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            result.revision, external_module, 0u,
            &external_attribute) == CM_RESOLVE_VIEW_OK
        && external_attribute.source == external_information.source
        && external_attribute.source != root_information.source
        && item_ref_equal(external_attribute.owner,
            external_information.declaration)
        && external_attribute.expansion_depth == 0u
        && string_equals(&graph, external_attribute.metadata,
            "doc = \"external\""),
        "external-module inner attribute lost source-qualified ownership");
    memset(&root_attribute, 0xa5, sizeof(root_attribute));
    ok &= check(cm_module_graph_get_effective_inner_attribute(&graph,
            result.revision, result.root, 1u,
            &root_attribute) == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && effective_attribute_is_zero(&root_attribute),
        "out-of-range inner-attribute access did not clear output");
    cm_source_set_destroy(&sources);
    ok &= check(string_equals(&graph, inline_attribute.metadata,
            "allow(dead_code)")
        && string_equals(&graph, external_attribute.metadata,
            "doc = \"external\""),
        "inner-attribute metadata borrowed destroyed source storage");
    cm_module_graph_destroy(&graph);
    return ok;
}

static int test_inactive_root_is_empty(void)
{
    static const unsigned char source_text[] =
        "#![cfg(windows)]\nunknown_macro!();\n";
    static const CmAstItemKind raw_kinds[] = { CM_AST_ITEM_MACRO };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmResolveModuleInfo information;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/inactive-root/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add inactive-root source");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    result = cm_module_graph_build(&graph, &sources, root, &options);
    memset(&information, 0, sizeof(information));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && cm_module_graph_module_count(&graph) == 1u
        && cm_module_graph_get_module(&graph, result.root, &information)
        && information.parent == CM_MODULE_NONE
        && information.child_count == 0u
        && information.type_count == 0u
        && information.value_count == 0u
        && information.macro_count == 0u
        && information.active_item_count == 0u
        && information.effective_item_count == 0u
        && information.inner_attribute_count == 0u
        && module_items_are(&graph, result.root, raw_kinds, 1u, NULL),
        "inactive crate root was not retained as an empty compilation unit");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_cfg_set(void)
{
    static const CmAstItemKind direct_items[] = {
        CM_AST_ITEM_MODULE,
        CM_AST_ITEM_FUNCTION
    };
    static const CmAstItemKind active_items[] = {
        CM_AST_ITEM_FUNCTION
    };
    static const unsigned char source_text[] =
        "#[cfg(feature = \"absent\")] mod missing; fn live() {}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmResolveError error;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/cfg/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add cfg source");
    result = cm_module_graph_build(&graph, &sources, root, NULL);
    ok &= check(result.error_count == 1u &&
        cm_module_graph_get_error(&graph, 0u, &error) &&
        error.kind == CM_RESOLVE_ERROR_INVALID_ARGUMENT,
        "graph build accepted no explicit cfg set");
    cm_module_graph_options_init(&options);
    cm_cfg_set_init(&cfg);
    options.cfg = &cfg;
    result = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(result.error_count == 0u &&
        cm_module_graph_module_count(&graph) == 1u &&
        namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "live"), "explicit cfg decision was not respected");
    ok &= check(module_items_are(&graph, result.root, direct_items,
        (uint32_t)CM_ARRAY_LEN(direct_items), NULL),
        "direct item view incorrectly filtered a cfg-disabled item");
    ok &= check(active_module_items_are(&graph, result.root, active_items,
        (uint32_t)CM_ARRAY_LEN(active_items)),
        "cfg-active direct item view did not preserve exact decisions");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_duplicate_path(void)
{
    static const unsigned char source_text[] =
        "mod duplicate {} mod duplicate {}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    const CmAstItemId *empty_items;
    uint32_t empty_count;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources, "duplicate/lib.rs",
        source_text, sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add duplicate source");
    result = build_with_empty_cfg(&graph, &sources, root);
    ok &= check(result.error_count == 1u &&
        cm_module_graph_get_error(&graph, 0u, &error) &&
        error.kind == CM_RESOLVE_ERROR_DUPLICATE_MODULE_PATH &&
        string_equals(&graph, error.module_path, "crate::duplicate"),
        "duplicate absolute module path was not diagnosed");
    empty_items = (const CmAstItemId *)(const void *)&graph;
    empty_count = 99u;
    ok &= check(cm_module_graph_borrow_items(&graph, 2u, &empty_items,
        &empty_count) && empty_count == 0u,
        "valid empty inline module did not expose an empty item list");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_namespace(void)
{
    static const unsigned char source_text[] =
        "macro_rules! named { () => { struct Generated; } } named!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo information;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources, "macro/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add macro namespace source");
    result = build_with_empty_cfg(&graph, &sources, root);
    ok &= check(result.error_count == 0u &&
        cm_module_graph_get_module(&graph, result.root, &information) &&
        information.macro_count == 1u && information.type_count == 1u &&
        namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_MACRO,
            "named") &&
        namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "Generated"),
        "macro declaration or item-macro output namespace differs");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_retained_macro_namespaces(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root;
    CmResolveModuleInfo nested_information;
    CmResolveNamespaceEntry include_entry;
    CmResolveNamespaceEntry opaque_entry;
    CmResolveMacroDeclaration include_declaration;
    CmResolveMacroDeclaration opaque_declaration;
    CmResolveEffectiveAttribute include_attribute;
    CmResolveEffectiveAttribute opaque_attribute;
    CmModuleId nested;
    int ok;

    ok = check(load_and_build(
        "tests/resolve/fixtures/macro-declarations/lib.rs",
        &sources, &graph, NULL, &result),
        "failed to load macro declaration fixture");
    if (!ok) return 0;
    nested = child_named(&graph, result.root, "nested");
    memset(&include_entry, 0, sizeof(include_entry));
    memset(&opaque_entry, 0, sizeof(opaque_entry));
    memset(&include_declaration, 0, sizeof(include_declaration));
    memset(&opaque_declaration, 0, sizeof(opaque_declaration));
    memset(&include_attribute, 0, sizeof(include_attribute));
    memset(&opaque_attribute, 0, sizeof(opaque_attribute));
    ok &= check(result.error_count == 0u && result.root != CM_MODULE_NONE
        && nested != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root, &root)
        && cm_module_graph_get_module(&graph, nested, &nested_information)
        && root.macro_count == 1u && root.active_item_count == 1u
        && root.effective_item_count == 1u
        && nested_information.macro_count == 1u
        && nested_information.active_item_count == 0u
        && nested_information.effective_item_count == 0u
        && cm_module_graph_get_namespace_entry(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, 0u, &include_entry)
        && string_equals(&graph, include_entry.name, "include")
        && include_entry.item_kind == CM_AST_ITEM_MACRO
        && cm_module_graph_get_namespace_entry(&graph, nested,
            CM_RESOLVE_NAMESPACE_MACRO, 0u, &opaque_entry)
        && string_equals(&graph, opaque_entry.name, "opaque")
        && opaque_entry.item_kind == CM_AST_ITEM_MACRO
        && !namespace_has(&graph, nested, CM_RESOLVE_NAMESPACE_MACRO,
            "hidden"),
        "cfg-active macro declarations were not registered by container");
    ok &= check(cm_module_graph_get_macro_declaration(&graph,
            result.revision, include_entry.declaration,
            &include_declaration) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_macro_declaration(&graph,
            result.revision, opaque_entry.declaration,
            &opaque_declaration) == CM_RESOLVE_VIEW_OK
        && include_declaration.owner_module == result.root
        && include_declaration.name == include_entry.name
        && include_declaration.form == CM_AST_MACRO_RULES_DEFINITION
        && include_declaration.visibility == CM_AST_VIS_INHERITED
        && include_declaration.attribute_count == 2u
        && include_declaration.span.source == include_entry.declaration.source
        && item_ref_equal(include_declaration.provenance.source_item,
            include_entry.declaration)
        && opaque_declaration.owner_module == nested
        && opaque_declaration.name == opaque_entry.name
        && opaque_declaration.form == CM_AST_MACRO_DECLARATIVE_DEFINITION
        && opaque_declaration.visibility == CM_AST_VIS_PUBLIC
        && opaque_declaration.attribute_count == 1u
        && item_ref_equal(opaque_declaration.provenance.source_item,
            opaque_entry.declaration),
        "macro declaration metadata lost form, owner, or provenance");
    ok &= check(cm_module_graph_get_macro_declaration_attribute(&graph,
            result.revision, include_entry.declaration, 0u,
            &include_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, include_attribute.metadata,
            "rustc_builtin_macro")
        && item_ref_equal(include_attribute.owner,
            include_entry.declaration)
        && cm_module_graph_get_macro_declaration_attribute(&graph,
            result.revision, opaque_entry.declaration, 0u,
            &opaque_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, opaque_attribute.metadata,
            "doc = \"opaque\"")
        && item_ref_equal(opaque_attribute.owner,
            opaque_entry.declaration),
        "macro declaration effective attributes lost exact metadata");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_exports(void)
{
    static const unsigned char source_text[] =
        "#[macro_export]\n"
        "macro_rules! root_export { () => {}; }\n"
        "mod nested {\n"
        "  #[macro_export]\n"
        "  macro_rules! lifted { () => {}; }\n"
        "  macro_rules! local_only { () => {}; }\n"
        "  #[cfg(windows)]\n"
        "  #[macro_export]\n"
        "  macro_rules! disabled_export { () => {}; }\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo nested_information;
    CmResolveNamespaceEntry root_export;
    CmResolveNamespaceEntry lifted_root;
    CmResolveNamespaceEntry lifted_nested;
    CmResolveNamespaceEntry local_only;
    CmModuleId nested;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/macro-export/lib.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add macro_export source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    nested = child_named(&graph, result.root, "nested");
    memset(&root_export, 0, sizeof(root_export));
    memset(&lifted_root, 0, sizeof(lifted_root));
    memset(&lifted_nested, 0, sizeof(lifted_nested));
    memset(&local_only, 0, sizeof(local_only));
    ok &= check(result.error_count == 0u && nested != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && cm_module_graph_get_module(&graph, nested, &nested_information)
        && root_information.macro_count == 2u
        && nested_information.macro_count == 2u
        && namespace_get_named(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "root_export", &root_export)
        && namespace_get_named(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "lifted", &lifted_root)
        && namespace_get_named(&graph, nested,
            CM_RESOLVE_NAMESPACE_MACRO, "lifted", &lifted_nested)
        && namespace_get_named(&graph, nested,
            CM_RESOLVE_NAMESPACE_MACRO, "local_only", &local_only)
        && !namespace_has(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "local_only")
        && !namespace_has(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "disabled_export")
        && !namespace_has(&graph, nested,
            CM_RESOLVE_NAMESPACE_MACRO, "disabled_export"),
        "macro_export did not produce the exact root and local namespaces");
    ok &= check(root_export.visibility == CM_AST_VIS_PUBLIC
        && lifted_root.visibility == CM_AST_VIS_PUBLIC
        && lifted_nested.visibility == CM_AST_VIS_INHERITED
        && local_only.visibility == CM_AST_VIS_INHERITED
        && item_ref_equal(lifted_root.declaration,
            lifted_nested.declaration)
        && lifted_root.declaration.source == root_source
        && root_export.declaration.source == root_source,
        "macro_export lost identity, visibility, or root deduplication");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_declaration_metadata_views(void)
{
    static const unsigned char source_text[] =
        "macro_rules! emit { () => {\n"
        "  #[doc = \"generated\"]\n"
        "  macro_rules! generated { () => {}; }\n"
        "} }\n"
        "emit!();\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult first;
    CmModuleGraphResult second;
    CmModuleGraphResult failed;
    CmResolveNamespaceEntry generated_entry;
    CmResolveMacroDeclaration declaration;
    CmResolveEffectiveAttribute attribute;
    CmResolveItemRef unknown;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/macro-metadata/lib.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add macro metadata source");
    first = build_with_empty_cfg(&graph, &sources, root_source);
    memset(&generated_entry, 0, sizeof(generated_entry));
    memset(&declaration, 0, sizeof(declaration));
    memset(&attribute, 0, sizeof(attribute));
    ok &= check(first.error_count == 0u
        && namespace_get_named(&graph, first.root,
            CM_RESOLVE_NAMESPACE_MACRO, "generated", &generated_entry)
        && cm_module_graph_get_macro_declaration(&graph, first.revision,
            generated_entry.declaration, &declaration) == CM_RESOLVE_VIEW_OK
        && declaration.is_generated
        && declaration.owner_module == first.root
        && declaration.form == CM_AST_MACRO_RULES_DEFINITION
        && declaration.attribute_count == 1u
        && declaration.provenance.source_item.source == 0u
        && declaration.provenance.source_item.item == CM_AST_ITEM_NONE
        && declaration.provenance.macro_invocation.source == root_source
        && declaration.provenance.macro_invocation.item != CM_AST_ITEM_NONE
        && declaration.provenance.macro_definition.source == root_source
        && declaration.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && declaration.provenance.expansion_depth == 1u
        && declaration.span.source == root_source
        && cm_module_graph_get_macro_declaration_attribute(&graph,
            first.revision, generated_entry.declaration, 0u,
            &attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, attribute.metadata,
            "doc = \"generated\"")
        && attribute.span.source == root_source,
        "generated macro metadata lost exact provenance or attributes");
    unknown.source = root_source;
    unknown.item = 999999u;
    memset(&declaration, 0xa5, sizeof(declaration));
    memset(&attribute, 0xa5, sizeof(attribute));
    ok &= check(cm_module_graph_get_macro_declaration(&graph,
            first.revision, unknown, &declaration)
                == CM_RESOLVE_VIEW_NOT_FOUND
        && declaration.declaration.source == 0u
        && declaration.declaration.item == CM_AST_ITEM_NONE
        && declaration.owner_module == CM_MODULE_NONE
        && cm_module_graph_get_macro_declaration_attribute(&graph,
            first.revision, generated_entry.declaration, 1u,
            &attribute) == CM_RESOLVE_VIEW_OUT_OF_RANGE
        && attribute.metadata == CM_RESOLVE_STRING_NONE,
        "macro metadata missing/out-of-range lookup did not fail closed");
    second = build_with_empty_cfg(&graph, &sources, root_source);
    memset(&declaration, 0xa5, sizeof(declaration));
    ok &= check(second.error_count == 0u
        && cm_module_graph_get_macro_declaration(&graph, first.revision,
            generated_entry.declaration, &declaration)
                == CM_RESOLVE_VIEW_STALE_REVISION
        && declaration.declaration.source == 0u,
        "macro metadata accepted a stale graph revision");
    failed = cm_module_graph_build(&graph, NULL, root_source, NULL);
    memset(&attribute, 0xa5, sizeof(attribute));
    ok &= check(failed.error_count == 1u
        && cm_module_graph_get_macro_declaration_attribute(&graph,
            failed.revision, generated_entry.declaration, 0u,
            &attribute) == CM_RESOLVE_VIEW_FAILED_BUILD
        && attribute.metadata == CM_RESOLVE_STRING_NONE,
        "failed graph build exposed macro declaration metadata");
    ok &= check(strcmp(cm_resolve_view_status_name(
        CM_RESOLVE_VIEW_NOT_FOUND), "not found") == 0,
        "macro metadata not-found status name differs");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_export_rejection(const unsigned char *source_text,
    size_t source_length, const char *message)
{
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    CmResolveNamespaceEntry entry;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/macro-export-error/lib.rs", source_text,
        source_length, &root_source) == CM_SOURCE_OK,
        "failed to add macro_export rejection source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    memset(&entry, 0xa5, sizeof(entry));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 1u
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO
        && error.span.source == root_source
        && !cm_module_graph_get_namespace_entry(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, 0u, &entry)
        && entry.name == CM_RESOLVE_STRING_NONE
        && entry.declaration.source == 0u
        && entry.declaration.item == CM_AST_ITEM_NONE,
        message);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_export_rejections(void)
{
    static const unsigned char argument_source[] =
        "#[macro_export(reason)]\n"
        "macro_rules! malformed { () => {}; }\n";
    static const unsigned char duplicate_attribute_source[] =
        "#[macro_export]\n"
        "#[macro_export]\n"
        "macro_rules! duplicated { () => {}; }\n";
    static const unsigned char declarative_source[] =
        "#[macro_export]\n"
        "pub macro declarative { }\n";
    static const unsigned char collision_source[] =
        "mod first {\n"
        "  #[macro_export]\n"
        "  macro_rules! collision { () => {}; }\n"
        "}\n"
        "mod second {\n"
        "  #[macro_export]\n"
        "  macro_rules! collision { () => {}; }\n"
        "}\n";
    int ok;

    ok = test_macro_export_rejection(argument_source,
        sizeof(argument_source) - 1u,
        "macro_export with arguments did not fail closed");
    ok &= test_macro_export_rejection(duplicate_attribute_source,
        sizeof(duplicate_attribute_source) - 1u,
        "duplicate macro_export attributes did not fail closed");
    ok &= test_macro_export_rejection(declarative_source,
        sizeof(declarative_source) - 1u,
        "declarative macro_export did not fail closed");
    ok &= test_macro_export_rejection(collision_source,
        sizeof(collision_source) - 1u,
        "distinct same-name macro exports did not fail atomically");
    return ok;
}

static int test_macro_use_scope(void)
{
    static const unsigned char source_text[] =
        "macro_rules! before { () => {}; }\n"
        "#[macro_use]\n"
        "mod first {\n"
        "  macro_rules! before { () => {}; }\n"
        "  macro_rules! first_only { () => {}; }\n"
        "  pub macro not_legacy { }\n"
        "  #[macro_use]\n"
        "  mod deep { macro_rules! deep_only { () => {}; } }\n"
        "}\n"
        "#[macro_use]\n"
        "mod second {\n"
        "  macro_rules! first_only { () => {}; }\n"
        "  macro_rules! after { () => {}; }\n"
        "}\n"
        "macro_rules! after { () => {}; }\n"
        "#[cfg(windows)]\n"
        "#[macro_use]\n"
        "mod disabled { macro_rules! absent { () => {}; } }\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveMacroScopeEntry before;
    CmResolveMacroScopeEntry first_only;
    CmResolveMacroScopeEntry after;
    CmResolveMacroScopeEntry deep_only;
    CmResolveMacroScopeEntry missing;
    CmResolveNamespaceEntry first_before;
    CmResolveNamespaceEntry second_first_only;
    CmResolveNamespaceEntry root_after;
    CmModuleId first;
    CmModuleId second;
    CmModuleId deep;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/macro-use/lib.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add macro_use scope source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    first = child_named(&graph, result.root, "first");
    second = child_named(&graph, result.root, "second");
    deep = child_named(&graph, first, "deep");
    memset(&before, 0, sizeof(before));
    memset(&first_only, 0, sizeof(first_only));
    memset(&after, 0, sizeof(after));
    memset(&deep_only, 0, sizeof(deep_only));
    memset(&missing, 0, sizeof(missing));
    memset(&first_before, 0, sizeof(first_before));
    memset(&second_first_only, 0, sizeof(second_first_only));
    memset(&root_after, 0, sizeof(root_after));
    ok &= check(result.error_count == 0u
        && first != CM_MODULE_NONE && second != CM_MODULE_NONE
        && deep != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && root_information.macro_scope_count == 4u
        && macro_scope_get_named(&graph, result.root, "before", &before)
        && macro_scope_get_named(&graph, result.root, "first_only",
            &first_only)
        && macro_scope_get_named(&graph, result.root, "after", &after)
        && macro_scope_get_named(&graph, result.root, "deep_only",
            &deep_only)
        && !macro_scope_get_named(&graph, result.root, "not_legacy", &missing)
        && !macro_scope_get_named(&graph, result.root, "absent", &missing)
        && namespace_get_named(&graph, first,
            CM_RESOLVE_NAMESPACE_MACRO, "before", &first_before)
        && namespace_get_named(&graph, second,
            CM_RESOLVE_NAMESPACE_MACRO, "first_only", &second_first_only)
        && namespace_get_named(&graph, result.root,
            CM_RESOLVE_NAMESPACE_MACRO, "after", &root_after),
        "macro_use scope did not preserve cfg, transitivity, or forms");
    ok &= check(item_ref_equal(before.declaration,
            first_before.declaration)
        && before.is_macro_use
        && before.introduced_by.item != CM_AST_ITEM_NONE
        && item_ref_equal(first_only.declaration,
            second_first_only.declaration)
        && first_only.is_macro_use
        && item_ref_equal(after.declaration, root_after.declaration)
        && !after.is_macro_use
        && item_ref_equal(after.introduced_by, after.declaration)
        && deep_only.is_macro_use
        && deep_only.declaration.source == root_source
        && deep_only.introduction_span.source == root_source,
        "macro_use did not apply exact declaration-ordered shadowing");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_use_rejection(const unsigned char *source_text,
    size_t source_length, const char *message)
{
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    CmResolveMacroScopeEntry entry;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/macro-use-error/lib.rs", source_text,
        source_length, &root_source) == CM_SOURCE_OK,
        "failed to add macro_use rejection source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    memset(&entry, 0xa5, sizeof(entry));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 1u
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO
        && !cm_module_graph_get_macro_scope_entry(&graph, result.root,
            0u, &entry)
        && entry.name == CM_RESOLVE_STRING_NONE
        && entry.declaration.source == 0u,
        message);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_macro_use_rejections(void)
{
    static const unsigned char argument_source[] =
        "#[macro_use(one)] mod child { macro_rules! one { () => {}; } }\n";
    static const unsigned char duplicate_source[] =
        "#[macro_use] #[macro_use]\n"
        "mod child { macro_rules! one { () => {}; } }\n";
    static const unsigned char non_module_source[] =
        "#[macro_use] struct NotAModule;\n";
    static const unsigned char generated_source[] =
        "macro_rules! emit { () => {\n"
        "  #[macro_use]\n"
        "  mod generated { macro_rules! one { () => {}; } }\n"
        "} }\n"
        "emit!();\n";
    int ok;

    ok = test_macro_use_rejection(argument_source,
        sizeof(argument_source) - 1u,
        "macro_use arguments did not fail closed");
    ok &= test_macro_use_rejection(duplicate_source,
        sizeof(duplicate_source) - 1u,
        "duplicate macro_use attributes did not fail closed");
    ok &= test_macro_use_rejection(non_module_source,
        sizeof(non_module_source) - 1u,
        "macro_use on a non-module did not fail closed");
    ok &= test_macro_use_rejection(generated_source,
        sizeof(generated_source) - 1u,
        "generated macro_use module did not fail closed");
    return ok;
}

static int test_generated_effective_graph(void)
{
    static const unsigned char source_text[] =
        "macro_rules! parent { () => { struct FromParent; } }\n"
        "macro_rules! make_leaf { () => {\n"
        "  #[cfg(windows)] unknown_disabled!();\n"
        "  #[cfg_attr(unix, doc = \"generated\")] pub struct Generated;\n"
        "  mod made { #![cfg_attr(unix, allow(dead_code))] "
            "#[cfg_attr(unix, doc = \"inner\")] struct Inner; }\n"
        "} }\n"
        "macro_rules! outer { () => { make_leaf!(); } }\n"
        "macro_rules! nested_outer { () => {\n"
        "  mod nested_made {\n"
        "    macro_rules! nested_inner { () => { struct Deep; } }\n"
        "    nested_inner!();\n"
        "  }\n"
        "} }\n"
        "#[cfg_attr(unix, doc = \"source\")] struct Source;\n"
        "mod source_inline { parent!(); }\n"
        "outer!();\n"
        "nested_outer!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveEffectiveItem source_item;
    CmResolveEffectiveItem generated_item;
    CmResolveEffectiveItem inner_item;
    CmResolveEffectiveItem made_module_item;
    CmResolveEffectiveItem nested_module_item;
    CmResolveEffectiveAttribute source_attribute;
    CmResolveEffectiveAttribute generated_attribute;
    CmResolveEffectiveAttribute made_attribute;
    CmResolveModuleInfo made_information;
    uint32_t source_index;
    uint32_t generated_index;
    CmModuleId made;
    CmModuleId nested_made;
    CmModuleId source_inline;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/generated/lib.rs", source_text,
        sizeof(source_text) - 1u, &root) == CM_SOURCE_OK,
        "failed to add generated graph source");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    result = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && root_information.effective_item_count == 5u
        && root_information.active_item_count == 5u
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "Source")
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "Generated")
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "made")
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "nested_made"),
        "generated declarations did not define the effective namespace");
    source_index = 0u;
    generated_index = 0u;
    ok &= check(effective_named(&graph, result.revision, result.root,
        "Source", &source_item, &source_index)
        && !source_item.is_generated
        && item_ref_equal(source_item.provenance.source_item,
            source_item.declaration)
        && source_item.provenance.expansion_depth == 0u
        && source_item.attribute_count == 1u
        && source_item.span.source == root,
        "source effective item lost active attributes or provenance");
    ok &= check(cm_module_graph_get_effective_attribute(&graph,
        result.revision, result.root, source_index, 0u,
        &source_attribute) == CM_RESOLVE_VIEW_OK
        && string_equals(&graph, source_attribute.metadata,
            "doc = \"source\"")
        && source_attribute.expansion_depth == 1u
        && source_attribute.span.source == root,
        "source cfg_attr payload was not materialized");
    ok &= check(effective_named(&graph, result.revision, result.root,
        "Generated", &generated_item, &generated_index)
        && generated_item.is_generated
        && generated_item.provenance.source_item.item == CM_AST_ITEM_NONE
        && generated_item.provenance.macro_invocation.source == root
        && generated_item.provenance.macro_invocation.item
            != CM_AST_ITEM_NONE
        && generated_item.provenance.macro_definition.source == root
        && generated_item.provenance.macro_definition.item
            != CM_AST_ITEM_NONE
        && generated_item.provenance.expansion_depth == 2u
        && generated_item.span.source == root
        && generated_item.span.end > generated_item.span.start
        && generated_item.attribute_count == 1u,
        "generated item provenance or invocation anchor differs");
    ok &= check(cm_module_graph_get_effective_attribute(&graph,
        result.revision, result.root, generated_index, 0u,
        &generated_attribute) == CM_RESOLVE_VIEW_OK
        && generated_attribute.source == root
        && generated_attribute.source_attribute != 0u
        && string_equals(&graph, generated_attribute.metadata,
            "doc = \"generated\"")
        && generated_attribute.expansion_depth == 1u
        && generated_attribute.span.source == generated_item.span.source
        && generated_attribute.span.start == generated_item.span.start
        && generated_attribute.span.end == generated_item.span.end,
        "generated effective attribute or span differs");
    made = child_named(&graph, result.root, "made");
    nested_made = child_named(&graph, result.root, "nested_made");
    source_inline = child_named(&graph, result.root, "source_inline");
    memset(&made_attribute, 0, sizeof(made_attribute));
    memset(&made_information, 0, sizeof(made_information));
    memset(&made_module_item, 0, sizeof(made_module_item));
    ok &= check(made != CM_MODULE_NONE
        && module_declaration_is(&graph, made, "made")
        && effective_named(&graph, result.revision, result.root,
            "made", &made_module_item, NULL)
        && cm_module_graph_get_module(&graph, made, &made_information)
        && made_information.inner_attribute_count == 1u
        && cm_module_graph_get_effective_inner_attribute(&graph,
            result.revision, made, 0u,
            &made_attribute) == CM_RESOLVE_VIEW_OK
        && item_ref_equal(made_attribute.owner,
            made_information.declaration)
        && made_attribute.expansion_depth == 1u
        && string_equals(&graph, made_attribute.metadata,
            "allow (dead_code)")
        && made_attribute.span.source == made_module_item.span.source
        && made_attribute.span.start == made_module_item.span.start
        && made_attribute.span.end == made_module_item.span.end,
        "generated inline-module inner attribute lost provenance");
    ok &= check(made != CM_MODULE_NONE && source_inline != CM_MODULE_NONE
        && effective_named(&graph, result.revision, made, "Inner",
            &inner_item, NULL)
        && inner_item.is_generated
        && inner_item.provenance.expansion_depth == 2u
        && effective_named(&graph, result.revision, source_inline,
            "FromParent", &inner_item, NULL)
        && inner_item.is_generated
        && inner_item.provenance.expansion_depth == 1u,
        "inline module plan topology or inherited macro scope differs");
    ok &= check(nested_made != CM_MODULE_NONE
        && effective_named(&graph, result.revision, result.root,
            "nested_made", &nested_module_item, NULL)
        && effective_named(&graph, result.revision, nested_made,
            "Deep", &inner_item, NULL)
        && inner_item.is_generated
        && inner_item.provenance.expansion_depth == 2u
        && inner_item.span.source == root
        && inner_item.span.start == nested_module_item.span.start
        && inner_item.span.end == nested_module_item.span.end,
        "nested generated invocation lost its outer source anchor");
    cm_source_set_destroy(&sources);
    ok &= check(string_equals(&graph, generated_attribute.metadata,
        "doc = \"generated\"")
        && effective_named(&graph, result.revision, result.root,
            "Generated", &generated_item, NULL),
        "generated graph storage retained the source set");
    cm_module_graph_destroy(&graph);
    return ok;
}

static int test_dependency_macro_graph_staging(void)
{
    static const unsigned char dependency_source[] =
        "#[macro_export] macro_rules! outer {"
        "  ($name:ident) => {"
        "    struct OuterMarker;"
        "    $crate::api::helper!($name);"
        "  };"
        "}"
        "#[macro_export] macro_rules! helper {"
        "  ($name:ident) => { type $name = $crate::Marker; };"
        "}"
        "pub struct Marker;"
        "pub mod api {"
        "  pub use crate::outer;"
        "  pub use crate::helper;"
        "}";
    static const unsigned char consumer_source[] =
        "use dep::api::outer as alias;"
        "alias!(MadeByDependency);";
    CmSourceSet dependency_sources;
    CmSourceSet consumer_sources;
    CmModuleGraph dependency_graph;
    CmModuleGraph consumer_graph;
    CmDependencyMacroArtifact artifact;
    const CmDependencyMacroArtifact *artifacts[1];
    CmModuleGraphResult dependency_result;
    CmModuleGraphResult consumer_result;
    CmDependencyMacroArtifactResult artifact_result;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmResolveEffectiveItem outer_item;
    CmResolveEffectiveItem helper_item;
    CmSourceId dependency_root;
    CmSourceId consumer_root;
    int ok;

    cm_source_set_init(&dependency_sources);
    cm_source_set_init(&consumer_sources);
    cm_module_graph_init(&dependency_graph);
    cm_module_graph_init(&consumer_graph);
    cm_dependency_macro_artifact_init(&artifact);
    ok = check(cm_source_add_memory(&dependency_sources,
            "tests/resolve/fixtures/dependency-stage/dep.rs",
            dependency_source, sizeof(dependency_source) - 1u,
            &dependency_root) == CM_SOURCE_OK
        && cm_source_add_memory(&consumer_sources,
            "tests/resolve/fixtures/dependency-stage/consumer.rs",
            consumer_source, sizeof(consumer_source) - 1u,
            &consumer_root) == CM_SOURCE_OK,
        "failed to add dependency macro staging sources");
    if (!ok) {
        cm_dependency_macro_artifact_destroy(&artifact);
        cm_module_graph_destroy(&consumer_graph);
        cm_module_graph_destroy(&dependency_graph);
        cm_source_set_destroy(&consumer_sources);
        cm_source_set_destroy(&dependency_sources);
        return 0;
    }
    dependency_result = build_with_empty_cfg(&dependency_graph,
        &dependency_sources, dependency_root);
    artifact_result = cm_dependency_macro_artifact_build(&artifact,
        &dependency_graph, dependency_result.revision, "dep", "rust_dep");
    artifacts[0] = &artifact;
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.dependency_macros = artifacts;
    options.dependency_macro_count = 1u;
    consumer_result = cm_module_graph_build(&consumer_graph,
        &consumer_sources, consumer_root, &options);
    memset(&outer_item, 0, sizeof(outer_item));
    memset(&helper_item, 0, sizeof(helper_item));
    ok &= check(dependency_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && consumer_result.error_count == 0u
        && effective_named(&consumer_graph, consumer_result.revision,
            consumer_result.root, "OuterMarker", &outer_item, NULL)
        && effective_named(&consumer_graph, consumer_result.revision,
            consumer_result.root, "MadeByDependency", &helper_item, NULL),
        "dependency source import or generated helper did not stage");
    ok &= check(outer_item.is_generated && helper_item.is_generated
        && outer_item.provenance.expansion_depth == 1u
        && helper_item.provenance.expansion_depth == 2u
        && outer_item.provenance.macro_invocation.source == consumer_root
        && helper_item.provenance.macro_invocation.source == consumer_root
        && outer_item.provenance.macro_definition.source == 0u
        && outer_item.provenance.macro_definition.item == CM_AST_ITEM_NONE
        && helper_item.provenance.macro_definition.source == 0u
        && helper_item.provenance.macro_definition.item == CM_AST_ITEM_NONE
        && outer_item.provenance.dependency_macro_definition.consumer_graph
            == &consumer_graph
        && helper_item.provenance.dependency_macro_definition.consumer_graph
            == &consumer_graph
        && outer_item.provenance.dependency_macro_definition
            .consumer_revision == consumer_result.revision
        && helper_item.provenance.dependency_macro_definition
            .consumer_revision == consumer_result.revision
        && outer_item.provenance.dependency_macro_definition.certificate
            != CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        && helper_item.provenance.dependency_macro_definition.certificate
            != CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        && outer_item.provenance.dependency_macro_definition.certificate
            != helper_item.provenance.dependency_macro_definition.certificate
        && outer_item.provenance.dependency_macro_definition.dependency == 1u
        && helper_item.provenance.dependency_macro_definition.dependency == 1u
        && outer_item.provenance.dependency_macro_definition
            .dependency_revision == dependency_result.revision
        && helper_item.provenance.dependency_macro_definition
            .dependency_revision == dependency_result.revision
        && outer_item.provenance.dependency_macro_definition
            .declaration.source == dependency_root
        && helper_item.provenance.dependency_macro_definition
            .declaration.source == dependency_root
        && outer_item.provenance.dependency_macro_definition
            .declaration.item != helper_item.provenance
                .dependency_macro_definition.declaration.item,
        "dependency macro provenance mixed graph namespaces or definitions");
    if (outer_item.provenance.dependency_macro_definition.dependency
            != CM_RESOLVE_DEPENDENCY_NONE) {
        CmResolveDependencyItemRef valid;
        CmResolveDependencyItemRef forged;
        CmModuleGraphResult rebuilt;

        valid = outer_item.provenance.dependency_macro_definition;
        forged = valid;
        forged.declaration.source += 1u;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, valid)
                == CM_RESOLVE_VIEW_OK
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision + 1u, valid)
                == CM_RESOLVE_VIEW_STALE_REVISION
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision,
                (CmResolveDependencyItemRef){ 0 })
                == CM_RESOLVE_VIEW_INVALID_ARGUMENT,
            "dependency macro registry accepted a stale or forged reference");
        forged = valid;
        forged.consumer_graph = &dependency_graph;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted another graph's reference");
        forged = valid;
        forged.consumer_revision += 1u;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a forged consumer revision");
        forged = valid;
        forged.dependency += 1u;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a forged dependency ID");
        forged = valid;
        forged.certificate = (CmResolveDependencyCertificateId)UINT32_MAX;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a forged certificate");
        forged = valid;
        forged.dependency_revision += 1u;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a forged dependency revision");
        forged = valid;
        forged.declaration.item = (CmAstItemId)UINT32_MAX;
        ok &= check(
            cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, consumer_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a forged declaration item");
        rebuilt = cm_module_graph_build(&consumer_graph,
            &consumer_sources, consumer_root, &options);
        ok &= check(rebuilt.error_count == 0u
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, rebuilt.revision, valid)
                == CM_RESOLVE_VIEW_NOT_FOUND,
            "dependency macro registry accepted a prior consumer revision");
        memset(&outer_item, 0, sizeof(outer_item));
        ok &= check(effective_named(&consumer_graph, rebuilt.revision,
                rebuilt.root, "OuterMarker", &outer_item, NULL),
            "rebuilt dependency consumer lost its published macro item");
        valid = outer_item.provenance.dependency_macro_definition;
        dependency_result = build_with_empty_cfg(&dependency_graph,
            &dependency_sources, dependency_root);
        ok &= check(dependency_result.error_count == 0u
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, rebuilt.revision, valid)
                == CM_RESOLVE_VIEW_OK,
            "consumer snapshot unexpectedly requires a live dependency "
            "revision");
        options.dependency_macros = NULL;
        options.dependency_macro_count = 1u;
        rebuilt = cm_module_graph_build(&consumer_graph,
            &consumer_sources, consumer_root, &options);
        ok &= check(rebuilt.error_count != 0u
            && cm_module_graph_validate_dependency_macro_definition(
                &consumer_graph, rebuilt.revision, valid)
                == CM_RESOLVE_VIEW_FAILED_BUILD,
            "dependency provenance remained visible after a failed consumer "
            "rebuild");
    }
    cm_dependency_macro_artifact_destroy(&artifact);
    cm_module_graph_destroy(&consumer_graph);
    cm_module_graph_destroy(&dependency_graph);
    cm_source_set_destroy(&consumer_sources);
    cm_source_set_destroy(&dependency_sources);
    return ok;
}

static int dependency_consumer_case(const char *test, const char *source,
    const CmDependencyMacroArtifact *const *artifacts,
    size_t artifact_count, const char *generated_name,
    CmResolveErrorKind expected_error, const char *expected_detail)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmModuleGraphResult result;
    CmCfgSet cfg;
    CmSourceId root;
    CmResolveError error;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.dependency_macros = artifacts;
    options.dependency_macro_count = artifact_count;
    memset(&result, 0, sizeof(result));
    ok = cm_source_add_memory(&sources, test,
        (const unsigned char *)source, strlen(source), &root)
        == CM_SOURCE_OK;
    if (ok) result = cm_module_graph_build(&graph, &sources, root, &options);
    memset(&error, 0, sizeof(error));
    if (generated_name != NULL) {
        ok = ok && result.error_count == 0u
            && effective_named(&graph, result.revision, result.root,
                generated_name, NULL, NULL);
    } else {
        ok = ok && result.error_count != 0u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == expected_error
            && (expected_detail == NULL
                || string_equals(&graph, error.detail_b,
                    expected_detail));
    }
    ok = check(ok, test);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_dependency_macro_graph_rejections(void)
{
    static const unsigned char dependency_source[] =
        "#[macro_export] macro_rules! helper {"
        "  ($name:ident) => { struct $name; };"
        "}"
        "#[macro_export] macro_rules! helper_a {"
        "  () => { struct HelperA; };"
        "}"
        "#[macro_export] macro_rules! helper_b {"
        "  () => { struct HelperB; };"
        "}"
        "#[macro_export] macro_rules! outer_private {"
        "  () => { $crate::private_api::helper!(GeneratedPrivate); };"
        "}"
        "#[macro_export] macro_rules! outer_ambiguous {"
        "  () => { $crate::ambiguous::helper!(); };"
        "}"
        "pub mod api {"
        "  pub use crate::helper;"
        "  pub use crate::outer_private;"
        "  pub use crate::outer_ambiguous;"
        "}"
        "pub mod ambiguous {"
        "  pub use crate::helper_a as helper;"
        "  pub use crate::helper_b as helper;"
        "}"
        "mod private_api { pub use crate::helper; }";
    CmSourceSet first_sources;
    CmSourceSet second_sources;
    CmModuleGraph first_graph;
    CmModuleGraph second_graph;
    CmDependencyMacroArtifact first_artifact;
    CmDependencyMacroArtifact second_artifact;
    const CmDependencyMacroArtifact *one[1];
    const CmDependencyMacroArtifact *two[2];
    CmModuleGraphResult first_result;
    CmModuleGraphResult second_result;
    CmDependencyMacroArtifactResult artifact_result;
    CmSourceId first_root;
    CmSourceId second_root;
    int ok;

    cm_source_set_init(&first_sources);
    cm_source_set_init(&second_sources);
    cm_module_graph_init(&first_graph);
    cm_module_graph_init(&second_graph);
    cm_dependency_macro_artifact_init(&first_artifact);
    cm_dependency_macro_artifact_init(&second_artifact);
    ok = check(cm_source_add_memory(&first_sources,
            "tests/resolve/fixtures/dependency-rejections/first.rs",
            dependency_source, sizeof(dependency_source) - 1u,
            &first_root) == CM_SOURCE_OK
        && cm_source_add_memory(&second_sources,
            "tests/resolve/fixtures/dependency-rejections/second.rs",
            dependency_source, sizeof(dependency_source) - 1u,
            &second_root) == CM_SOURCE_OK,
        "failed to add dependency rejection fixtures");
    if (!ok) goto cleanup;
    first_result = build_with_empty_cfg(&first_graph, &first_sources,
        first_root);
    second_result = build_with_empty_cfg(&second_graph, &second_sources,
        second_root);
    artifact_result = cm_dependency_macro_artifact_build(&first_artifact,
        &first_graph, first_result.revision, "dep", "rust_dep");
    ok &= check(first_result.error_count == 0u
        && second_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK,
        "failed to build first dependency rejection artifact");
    one[0] = &first_artifact;
    ok &= dependency_consumer_case(
        "dependency-missing-artifact-array", "struct Root;", NULL, 1u,
        NULL, CM_RESOLVE_ERROR_INVALID_ARGUMENT, NULL);
    {
        const CmDependencyMacroArtifact *null_artifact[1];

        null_artifact[0] = NULL;
        ok &= dependency_consumer_case(
            "dependency-null-artifact", "struct Root;", null_artifact, 1u,
            NULL, CM_RESOLVE_ERROR_INVALID_ARGUMENT, NULL);
    }
    ok &= dependency_consumer_case(
        "dependency-private-path",
        "use dep::private_api::helper; helper!(PrivatePath);",
        one, 1u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO, "private path");
    ok &= dependency_consumer_case(
        "dependency-generated-private-path",
        "use dep::api::outer_private; outer_private!();",
        one, 1u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO,
        "generated macro resolver returned an invalid target");
    ok &= dependency_consumer_case(
        "dependency-generated-ambiguous-path",
        "use dep::api::outer_ambiguous; outer_ambiguous!();",
        one, 1u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO,
        "generated qualified macro path is ambiguous");
    ok &= dependency_consumer_case(
        "dependency-source-hygienic-path",
        "rust_dep::api::helper!(SourceWritten);",
        one, 1u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO,
        "no unique public local-crate macro binding");
    ok &= dependency_consumer_case(
        "dependency-glob-import",
        "use dep::api::*; helper!(FromGlob);",
        one, 1u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO,
        "unsupported import");
    ok &= dependency_consumer_case(
        "dependency-unrelated-glob",
        "mod local {} use local::*; use dep::api::helper;"
        "helper!(UnrelatedGlobAccepted);",
        one, 1u, "UnrelatedGlobAccepted", CM_RESOLVE_ERROR_INVALID_ARGUMENT,
        NULL);

    artifact_result = cm_dependency_macro_artifact_build(&second_artifact,
        &second_graph, second_result.revision, "other", "rust_dep");
    two[0] = &first_artifact;
    two[1] = &second_artifact;
    ok &= check(artifact_result.status == CM_DEPENDENCY_MACRO_OK,
        "failed to build duplicate-identifier dependency artifact");
    ok &= dependency_consumer_case(
        "dependency-duplicate-hygienic-identifier", "struct Root;",
        two, 2u, NULL, CM_RESOLVE_ERROR_INVALID_ARGUMENT, NULL);

    artifact_result = cm_dependency_macro_artifact_build(&second_artifact,
        &second_graph, second_result.revision, "dep", "rust_other");
    ok &= check(artifact_result.status == CM_DEPENDENCY_MACRO_OK,
        "failed to rebuild duplicate-extern dependency artifact");
    ok &= dependency_consumer_case(
        "dependency-duplicate-extern-name", "struct Root;",
        two, 2u, NULL, CM_RESOLVE_ERROR_INVALID_ARGUMENT, NULL);

    artifact_result = cm_dependency_macro_artifact_build(&second_artifact,
        &second_graph, second_result.revision, "other", "rust_other");
    ok &= check(artifact_result.status == CM_DEPENDENCY_MACRO_OK,
        "failed to rebuild distinct dependency artifact");
    {
        static const unsigned char cross_source[] =
            "use dep::api::helper as first;"
            "use other::api::helper as second;"
            "first!(FromFirst); second!(FromSecond);";
        CmSourceSet cross_sources;
        CmModuleGraph cross_graph;
        CmModuleGraphOptions cross_options;
        CmModuleGraphResult cross_result;
        CmCfgSet cross_cfg;
        CmSourceId cross_root;
        CmResolveEffectiveItem first_item;
        CmResolveEffectiveItem second_item;
        CmResolveDependencyItemRef forged;
        int cross_ok;

        cm_source_set_init(&cross_sources);
        cm_module_graph_init(&cross_graph);
        cm_cfg_set_init(&cross_cfg);
        cm_module_graph_options_init(&cross_options);
        cross_options.cfg = &cross_cfg;
        cross_options.dependency_macros = two;
        cross_options.dependency_macro_count = 2u;
        memset(&first_item, 0, sizeof(first_item));
        memset(&second_item, 0, sizeof(second_item));
        cross_ok = cm_source_add_memory(&cross_sources,
            "tests/resolve/fixtures/dependency-rejections/cross.rs",
            cross_source, sizeof(cross_source) - 1u, &cross_root)
            == CM_SOURCE_OK;
        memset(&cross_result, 0, sizeof(cross_result));
        if (cross_ok) cross_result = cm_module_graph_build(&cross_graph,
            &cross_sources, cross_root, &cross_options);
        cross_ok = cross_ok && cross_result.error_count == 0u
            && effective_named(&cross_graph, cross_result.revision,
                cross_result.root, "FromFirst", &first_item, NULL)
            && effective_named(&cross_graph, cross_result.revision,
                cross_result.root, "FromSecond", &second_item, NULL)
            && first_item.provenance.dependency_macro_definition.certificate
                != second_item.provenance.dependency_macro_definition
                    .certificate;
        forged = first_item.provenance.dependency_macro_definition;
        forged.dependency = second_item.provenance
            .dependency_macro_definition.dependency;
        cross_ok = cross_ok
            && cm_module_graph_validate_dependency_macro_definition(
                &cross_graph, cross_result.revision, forged)
                == CM_RESOLVE_VIEW_NOT_FOUND;
        ok &= check(cross_ok,
            "dependency registry accepted a cross-artifact field splice");
        cm_module_graph_destroy(&cross_graph);
        cm_source_set_destroy(&cross_sources);
    }
    ok &= dependency_consumer_case(
        "dependency-competing-imports",
        "use dep::api::helper as same;"
        "use other::api::helper as same; same!(Competing);",
        two, 2u, NULL, CM_RESOLVE_ERROR_ITEM_MACRO, "ambiguous");

    first_result = build_with_empty_cfg(&first_graph, &first_sources,
        first_root);
    ok &= check(first_result.error_count == 0u,
        "failed to advance dependency graph revision");
    ok &= dependency_consumer_case(
        "dependency-stale-artifact", "struct Root;", one, 1u, NULL,
        CM_RESOLVE_ERROR_INVALID_ARGUMENT, NULL);
    artifact_result = cm_dependency_macro_artifact_build(&first_artifact,
        &first_graph, first_result.revision, "dep", "rust_dep");
    if (artifact_result.status == CM_DEPENDENCY_MACRO_OK) {
        CmModuleGraphOptions options;
        CmModuleGraphResult self_result;
        CmCfgSet cfg;

        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&options);
        options.cfg = &cfg;
        options.dependency_macros = one;
        options.dependency_macro_count = 1u;
        self_result = cm_module_graph_build(&first_graph, &first_sources,
            first_root, &options);
        ok &= check(self_result.error_count != 0u,
            "dependency self-registration was accepted");
    } else {
        ok &= check(0, "failed to rebuild artifact for self-dependency test");
    }

cleanup:
    cm_dependency_macro_artifact_destroy(&second_artifact);
    cm_dependency_macro_artifact_destroy(&first_artifact);
    cm_module_graph_destroy(&second_graph);
    cm_module_graph_destroy(&first_graph);
    cm_source_set_destroy(&second_sources);
    cm_source_set_destroy(&first_sources);
    return ok;
}

static int test_generated_rejections(void)
{
    static const char *sources_text[] = {
        "macro_rules! bad { () => { use crate::Thing; } } bad!();",
        "macro_rules! bad { () => { extern crate core; } } bad!();",
        "macro_rules! bad { () => { mod external; } } bad!();"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < CM_ARRAY_LEN(sources_text); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions options;
        CmCfgSet cfg;
        CmModuleGraphResult result;
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/generated-reject/lib.rs",
            (const unsigned char *)sources_text[index],
            strlen(sources_text[index]), &root) == CM_SOURCE_OK,
            "failed to add generated rejection source");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&options);
        options.cfg = &cfg;
        result = cm_module_graph_build(&graph, &sources, root, &options);
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u
            && cm_module_graph_module_count(&graph) == 0u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind
                == CM_RESOLVE_ERROR_UNSUPPORTED_GENERATED_ITEM
            && error.span.source == root
            && error.span.end > error.span.start && error.detail_a != 0u,
            "unsupported generated topology/import was not rejected");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int test_expansion_errors(void)
{
    static const char *sources_text[] = {
        "#[cfg_attr(unix)] struct Bad;",
        "unknown_macro!();",
        "macro_rules! outer { () => { unknown_generated!(); } } outer!();"
    };
    static const CmResolveErrorKind expected[] = {
        CM_RESOLVE_ERROR_CFG_EXPANSION,
        CM_RESOLVE_ERROR_ITEM_MACRO,
        CM_RESOLVE_ERROR_ITEM_MACRO
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < CM_ARRAY_LEN(sources_text); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions options;
        CmCfgSet cfg;
        CmModuleGraphResult result;
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/expansion-error/lib.rs",
            (const unsigned char *)sources_text[index],
            strlen(sources_text[index]), &root) == CM_SOURCE_OK,
            "failed to add expansion-error source");
        cm_cfg_set_init(&cfg);
        cfg.environment.target_family = "unix";
        cm_module_graph_options_init(&options);
        options.cfg = &cfg;
        result = cm_module_graph_build(&graph, &sources, root, &options);
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u
            && cm_module_graph_module_count(&graph) == 0u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == expected[index]
            && error.span.source == root
            && error.span.end > error.span.start
            && error.detail_a != CM_RESOLVE_STRING_NONE
            && error.detail_b != CM_RESOLVE_STRING_NONE,
            "cfg or item-macro failure lost its graph diagnostic");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int test_qualified_macro_reexport(void)
{
    static const unsigned char source_text[] =
        "mod macros {"
        "  pub macro make { () => { pub struct Made; }, }"
        "}"
        "pub use crate::macros::make;"
        "mod consumer { crate::make!(); }";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveEffectiveItem made;
    CmResolveModuleInfo macros_information;
    CmModuleId macros;
    CmModuleId consumer;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/qualified-macro/lib.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add qualified macro reexport source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    macros = child_named(&graph, result.root, "macros");
    consumer = child_named(&graph, result.root, "consumer");
    memset(&made, 0, sizeof(made));
    memset(&macros_information, 0, sizeof(macros_information));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && macros != CM_MODULE_NONE && consumer != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, macros,
            &macros_information)
        && effective_named(&graph, result.revision, consumer, "Made",
            &made, NULL)
        && made.is_generated
        && made.provenance.macro_invocation.source == root_source
        && made.provenance.macro_definition.source == root_source
        && made.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && made.provenance.macro_definition.item
            != macros_information.declaration.item,
        "crate-qualified macro reexport did not resolve to its exact "
        "declaration");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_authenticated_cfg_select(void)
{
    static const unsigned char success_source[] =
        "mod builtins {"
        "  #[rustc_builtin_macro]"
        "  pub macro cfg_select($($tt:tt)*) { /* compiler built-in */ }"
        "}"
        "pub use crate::builtins::cfg_select;"
        "crate::cfg_select! {"
        "  windows => { struct Wrong; }"
        "  unix => { struct Selected; }"
        "  _ => { struct WrongFallback; }"
        "}"
        "crate::cfg_select! {"
        "  windows => { struct WrongAgain; }"
        "  _ => { struct Fallback; }"
        "}";
    static const unsigned char missing_attribute[] =
        "pub macro cfg_select($($tt:tt)*) {}"
        "crate::cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char duplicate_attribute[] =
        "#[rustc_builtin_macro] #[rustc_builtin_macro]"
        "pub macro cfg_select($($tt:tt)*) {}"
        "crate::cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char malformed_attribute[] =
        "#[rustc_builtin_macro(cfg_select)]"
        "pub macro cfg_select($($tt:tt)*) {}"
        "crate::cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char similarly_named[] =
        "mod builtins {"
        "  #[rustc_builtin_macro]"
        "  pub macro cfg_select_like($($tt:tt)*) {}"
        "}"
        "pub use crate::builtins::cfg_select_like as cfg_select;"
        "crate::cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char shadowed[] =
        "mod builtins {"
        "  #[rustc_builtin_macro]"
        "  pub macro cfg_select($($tt:tt)*) {}"
        "}"
        "pub use crate::builtins::cfg_select as real_cfg_select;"
        "pub macro cfg_select($($tt:tt)*) {}"
        "cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char cfg_disabled[] =
        "#[cfg(windows)] #[rustc_builtin_macro]"
        "pub macro cfg_select($($tt:tt)*) {}"
        "crate::cfg_select! { _ => { struct Wrong; } }";
    static const unsigned char generated_invocation[] =
        "#[rustc_builtin_macro]"
        "pub macro cfg_select($($tt:tt)*) {}"
        "macro_rules! outer {"
        "  () => { crate::cfg_select! { _ => { struct Wrong; } } };"
        "}"
        "outer!();";
    const unsigned char *rejected[] = {
        missing_attribute,
        duplicate_attribute,
        malformed_attribute,
        similarly_named,
        shadowed,
        cfg_disabled,
        generated_invocation
    };
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmModuleGraphResult result;
    CmCfgSet cfg;
    size_t index;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/cfg-select/lib.rs", success_source,
        sizeof(success_source) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add authenticated cfg_select success source");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    result = cm_module_graph_build(&graph, &sources, root_source, &options);
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && effective_named(&graph, result.revision, result.root,
            "Selected", NULL, NULL)
        && effective_named(&graph, result.revision, result.root,
            "Fallback", NULL, NULL)
        && !effective_named(&graph, result.revision, result.root,
            "Wrong", NULL, NULL)
        && !effective_named(&graph, result.revision, result.root,
            "WrongFallback", NULL, NULL),
        "authenticated qualified cfg_select did not emit exact branches");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    for (index = 0u; index < CM_ARRAY_LEN(rejected); ++index) {
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/cfg-select-rejected/lib.rs",
            rejected[index], strlen((const char *)rejected[index]),
            &root_source) == CM_SOURCE_OK,
            "failed to add rejected cfg_select source");
        result = cm_module_graph_build(&graph, &sources, root_source,
            &options);
        memset(&error, 0, sizeof(error));
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO,
            "unauthenticated, inactive, shadowed, or generated cfg_select "
            "did not fail closed");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int test_external_macro_scope(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo child_information;
    CmResolveEffectiveItem generated;
    CmModuleId child;
    int ok;

    ok = check(load_and_build(
        "tests/resolve/fixtures/external-macro/lib.rs", &sources, &graph,
        NULL, &result), "failed to load external macro fixture");
    if (!ok) return 0;
    child = child_named(&graph, result.root, "child");
    ok &= check(result.error_count == 0u && child != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && cm_module_graph_get_module(&graph, child, &child_information)
        && effective_named(&graph, result.revision, child, "FromParent",
            &generated, NULL)
        && generated.is_generated
        && generated.provenance.macro_invocation.source
            == child_information.source
        && generated.provenance.macro_invocation.item != CM_AST_ITEM_NONE
        && generated.provenance.macro_definition.source
            == root_information.source
        && generated.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && generated.span.source == child_information.source
        && generated.span.end > generated.span.start,
        "external child lost inherited macro scope or source provenance");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_external_module_path_attribute(void)
{
    static const unsigned char malformed_source[] =
        "#[path(\"actual.rs\")] mod renamed;\n";
    static const unsigned char missing_source[] =
        "#[path = \"absent.rs\"] mod renamed;\n";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmModuleId renamed;
    CmModuleId selected;
    CmModuleId nested;
    CmModuleId outer;
    CmModuleId file_relative;
    CmSourceId root_source;
    CmResolveError error;
    int ok;

    ok = check(load_and_build(
        "tests/resolve/fixtures/path-attribute/lib.rs", &sources,
        &graph, NULL, &result), "failed to load path attribute fixture");
    if (!ok) return 0;
    renamed = child_named(&graph, result.root, "renamed");
    selected = child_named(&graph, result.root, "selected");
    nested = child_named(&graph, selected, "nested");
    outer = child_named(&graph, result.root, "outer");
    file_relative = child_named(&graph, outer, "child");
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && sources.length == 6u && renamed != CM_MODULE_NONE
        && selected != CM_MODULE_NONE && nested != CM_MODULE_NONE
        && outer != CM_MODULE_NONE && file_relative != CM_MODULE_NONE
        && effective_named(&graph, result.revision, renamed,
            "FromPath", NULL, NULL)
        && effective_named(&graph, result.revision, nested,
            "NestedFromPath", NULL, NULL)
        && effective_named(&graph, result.revision, file_relative,
            "FileRelativePath", NULL, NULL),
        "module path attribute did not select its exact source and "
        "child directory");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok &= check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/path-attribute/lib.rs", malformed_source,
        sizeof(malformed_source) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add malformed path attribute source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    ok &= check(result.error_count == 1u && sources.length == 1u
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_SOURCE_IO
        && error.span.source == root_source,
        "malformed module path attribute did not fail closed");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok &= check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/path-attribute/lib.rs", missing_source,
        sizeof(missing_source) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add missing path attribute source");
    result = build_with_empty_cfg(&graph, &sources, root_source);
    ok &= check(result.error_count == 1u && sources.length == 1u
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_MISSING_MODULE_FILE
        && error.detail_a != CM_RESOLVE_STRING_NONE
        && error.detail_b == CM_RESOLVE_STRING_NONE,
        "missing path-selected module did not report one exact candidate");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_staged_macro_use_invocation(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo definitions_information;
    CmResolveEffectiveItem generated;
    CmResolveEffectiveItem nested_generated;
    CmResolveEffectiveItem shadowed;
    CmModuleId definitions;
    int ok;

    ok = check(load_and_build(
        "tests/resolve/fixtures/staged-macro-use/lib.rs", &sources,
        &graph, NULL, &result),
        "failed to load staged macro_use fixture");
    if (!ok) return 0;
    definitions = child_named(&graph, result.root, "definitions");
    ok &= check(result.error_count == 0u
        && definitions != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && cm_module_graph_get_module(&graph, definitions,
            &definitions_information)
        && effective_named(&graph, result.revision, result.root,
            "FromMacroUse", &generated, NULL)
        && effective_named(&graph, result.revision, result.root,
            "GeneratedFromMacroUse", &nested_generated, NULL)
        && effective_named(&graph, result.revision, result.root,
            "FromRootShadow", &shadowed, NULL)
        && generated.is_generated
        && nested_generated.is_generated
        && shadowed.is_generated
        && generated.provenance.macro_invocation.source
            == root_information.source
        && generated.provenance.macro_definition.source
            == definitions_information.source
        && shadowed.provenance.macro_definition.source
            == root_information.source
        && generated.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && nested_generated.provenance.macro_definition.source
            == definitions_information.source
        && generated.span.source == root_information.source
        && generated.span.end > generated.span.start,
        "staged macro_use shadowing lost exact declaration order or identity");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_staged_generated_invocation_rejection(void)
{
    static const char *sources_text[] = {
        "macro_rules! make { () => { unknown!(); }; } make!();",
        "pub macro target { () => { struct Wrong; }, }"
        "macro_rules! make { () => { crate::target!(); }; } make!();"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < CM_ARRAY_LEN(sources_text); ++index) {
        CmSourceSet sources;
        CmSourceId root_source;
        CmModuleGraph graph;
        CmModuleGraphResult result;
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/staged-generated-error/lib.rs",
            (const unsigned char *)sources_text[index],
            strlen(sources_text[index]), &root_source) == CM_SOURCE_OK,
            "failed to add staged generated-error source");
        result = build_with_empty_cfg(&graph, &sources, root_source);
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO
            && error.span.source == root_source
            && error.span.end > error.span.start,
            "final staged replan accepted or misanchored a generated "
            "unknown or qualified call");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int test_nested_external_macro_shadowing(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo child_information;
    CmResolveModuleInfo grandchild_information;
    CmResolveEffectiveItem root_expansion;
    CmResolveEffectiveItem child_expansion;
    CmResolveEffectiveItem inherited_shadow;
    CmModuleId child;
    CmModuleId grandchild;
    int ok;

    ok = check(load_and_build(
        "tests/resolve/fixtures/external-macro-nested/lib.rs", &sources,
        &graph, NULL, &result), "failed to load nested macro fixture");
    if (!ok) return 0;
    child = child_named(&graph, result.root, "child");
    grandchild = child == CM_MODULE_NONE ? CM_MODULE_NONE
        : child_named(&graph, child, "grandchild");
    ok &= check(result.error_count == 0u && child != CM_MODULE_NONE
        && grandchild != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && cm_module_graph_get_module(&graph, child, &child_information)
        && cm_module_graph_get_module(&graph, grandchild,
            &grandchild_information)
        && effective_named(&graph, result.revision, child, "RootExpansion",
            &root_expansion, NULL)
        && effective_named(&graph, result.revision, child, "ChildExpansion",
            &child_expansion, NULL)
        && effective_named(&graph, result.revision, grandchild,
            "ChildExpansion", &inherited_shadow, NULL)
        && root_expansion.provenance.macro_definition.source
            == root_information.source
        && root_expansion.provenance.macro_invocation.source
            == child_information.source
        && child_expansion.provenance.macro_definition.source
            == child_information.source
        && inherited_shadow.provenance.macro_definition.source
            == child_information.source
        && inherited_shadow.provenance.macro_invocation.source
            == grandchild_information.source
        && inherited_shadow.span.source == grandchild_information.source,
        "nested external scope did not preserve local shadowing");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_external_macro_cfg_revision(void)
{
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmModuleGraphResult enabled;
    CmModuleGraphResult disabled;
    CmModuleId child;
    CmResolveModuleInfo child_information;
    CmResolveEffectiveItem item;
    CmResolveError error;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_load_file(&sources,
        "tests/resolve/fixtures/external-macro-cfg/lib.rs", &root)
        == CM_SOURCE_OK, "failed to load cfg external macro fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    enabled = cm_module_graph_build(&graph, &sources, root, &options);
    child = child_named(&graph, enabled.root, "child");
    memset(&child_information, 0, sizeof(child_information));
    ok &= check(enabled.error_count == 0u && child != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, child, &child_information)
        && effective_named(&graph, enabled.revision, child, "Enabled",
            &item, NULL),
        "enabled parent macro was not inherited by external child");

    cfg.environment.target_family = "windows";
    cfg.environment.target_os = "windows";
    disabled = cm_module_graph_build(&graph, &sources, root, &options);
    ok &= check(disabled.root != CM_MODULE_NONE
        && disabled.error_count == 1u
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO
        && error.span.source == child_information.source
        && string_equals(&graph, error.detail_a, "unsupported macro")
        && cm_module_graph_get_effective_item(&graph, enabled.revision,
            disabled.root, 0u, &item) == CM_RESOLVE_VIEW_STALE_REVISION
        && cm_module_graph_get_effective_item(&graph, disabled.revision,
            disabled.root, 0u, &item) == CM_RESOLVE_VIEW_FAILED_BUILD,
        "cfg-disabled inherited macro did not fail transactionally");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_external_macro_errors(void)
{
    static const char *paths[] = {
        "tests/resolve/fixtures/external-macro-qualified/lib.rs",
        "tests/resolve/fixtures/external-macro-imported/lib.rs"
    };
    static const char *diagnostics[] = {
        "qualified macro",
        "unsupported macro"
    };
    size_t index;
    int ok;

    ok = 1;
    for (index = 0u; index < CM_ARRAY_LEN(paths); ++index) {
        CmSourceSet sources;
        CmModuleGraph graph;
        CmModuleGraphResult result;
        CmResolveError error;
        int loaded;

        loaded = load_and_build(paths[index], &sources, &graph, NULL,
            &result);
        ok &= check(loaded, "failed to load external macro error fixture");
        if (loaded) {
            ok &= check(result.root != CM_MODULE_NONE
                && result.error_count == 1u
                && cm_module_graph_get_error(&graph, 0u, &error)
                && error.kind == CM_RESOLVE_ERROR_ITEM_MACRO
                && error.span.source != 0u
                && error.span.end > error.span.start
                && string_equals(&graph, error.detail_a,
                    diagnostics[index]),
                "qualified or imported macro did not remain a hard error");
        }
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int effective_name_at(const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module, uint32_t index,
    const char *expected, CmResolveEffectiveItem *out_effective)
{
    CmResolveEffectiveItem effective;
    const CmAst *ast;
    const CmAstItem *item;

    memset(&effective, 0, sizeof(effective));
    if (!cm_module_graph_borrow_ast(graph, module, &ast)
        || cm_module_graph_get_effective_item(graph, revision, module,
            index, &effective) != CM_RESOLVE_VIEW_OK) return 0;
    item = cm_ast_get_item(ast, effective.declaration.item);
    if (item == NULL || !ast_name_equals(ast, item->name, expected))
        return 0;
    if (out_effective != NULL) *out_effective = effective;
    return 1;
}

static int test_item_include_success(void)
{
    static const CmAstItemKind raw_kinds[] = {
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_FUNCTION,
        CM_AST_ITEM_STRUCT
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveEffectiveItem included;
    CmResolveEffectiveAttribute attribute;
    const CmSourceFile *included_file;
    const CmAst *included_ast;
    CmCfgSet cfg;
    CmModuleGraphOptions options;
    int ok;

    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.include_expansion = CM_INCLUDE_EXPANSION_SOURCE_FIXTURE;
    ok = check(load_and_build(
        "tests/resolve/fixtures/include-success/lib.rs", &sources,
        &graph, &options, &result), "failed to load include success fixture");
    if (!ok) return 0;
    memset(&root_information, 0, sizeof(root_information));
    memset(&included, 0, sizeof(included));
    memset(&attribute, 0, sizeof(attribute));
    included_ast = NULL;
    included_file = sources.length == 2u ? &sources.files[1] : NULL;
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && sources.length == 2u && included_file != NULL
        && strstr(included_file->path, "include-success/items.rs") != NULL
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && root_information.effective_item_count == 4u
        && root_information.active_item_count == 4u
        && module_items_are(&graph, result.root, raw_kinds,
            (uint32_t)CM_ARRAY_LEN(raw_kinds), NULL)
        && effective_name_at(&graph, result.revision, result.root, 0u,
            "Before", NULL)
        && effective_name_at(&graph, result.revision, result.root, 1u,
            "Included", &included)
        && effective_name_at(&graph, result.revision, result.root, 2u,
            "included_function", NULL)
        && effective_name_at(&graph, result.revision, result.root, 3u,
            "After", NULL),
        "included items were not spliced in exact lexical order");
    ok &= check(included_file != NULL
        && included.declaration.source == included_file->id
        && included.declaration.source != root_information.source
        && cm_module_graph_borrow_item_ast(&graph, result.root,
            included.declaration, &included_ast)
        && included_ast != NULL
        && cm_ast_get_item(included_ast, included.declaration.item) != NULL
        && item_ref_equal(included.provenance.source_item,
            included.declaration)
        && included.span.source == included_file->id
        && included.attribute_count == 1u
        && cm_module_graph_get_effective_attribute(&graph,
            result.revision, result.root, 1u, 0u,
            &attribute) == CM_RESOLVE_VIEW_OK
        && attribute.source == included_file->id
        && attribute.span.source == included_file->id
        && item_ref_equal(attribute.owner, included.declaration)
        && string_equals(&graph, attribute.metadata, "allow(dead_code)"),
        "included item or attribute lost source-qualified provenance");
    if (included_file != NULL) {
        CmResolveItemRef wrong_source;

        wrong_source = included.declaration;
        wrong_source.source = root_information.source;
        included_ast = (const CmAst *)(size_t)1u;
        ok &= check(!cm_module_graph_borrow_item_ast(&graph, result.root,
                wrong_source, &included_ast)
            && included_ast == NULL,
            "item AST accessor accepted the wrong source qualification");
    }
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_item_include_module_provenance(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo child_information;
    CmResolveEffectiveItem module_item;
    CmModuleId child;
    const CmSourceFile *included_file;
    CmCfgSet cfg;
    CmModuleGraphOptions options;
    int ok;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.include_expansion = CM_INCLUDE_EXPANSION_AUTHENTICATED;
    ok = check(load_and_build(
        "tests/resolve/fixtures/include-module/lib.rs", &sources,
        &graph, &options, &result),
        "failed to load included module fixture");
    if (!ok) return 0;
    memset(&root_information, 0, sizeof(root_information));
    memset(&child_information, 0, sizeof(child_information));
    memset(&module_item, 0, sizeof(module_item));
    included_file = sources.length == 2u ? &sources.files[1] : NULL;
    child = CM_MODULE_NONE;
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && included_file != NULL
        && strstr(included_file->path, "include-module/items.rs") != NULL
        && cm_module_graph_get_module(&graph, result.root,
            &root_information)
        && root_information.effective_item_count == 1u
        && effective_named(&graph, result.revision, result.root,
            "Included", &module_item, NULL)
        && module_item.declaration.source == included_file->id
        && (child = child_named(&graph, result.root, "Included"))
            != CM_MODULE_NONE
        && cm_module_graph_get_module(&graph, child, &child_information)
        && child_information.source == included_file->id
        && child_information.declaration.source == included_file->id
        && child_information.declaration.item
            == module_item.declaration.item
        && child_information.is_inline
        && module_declaration_is(&graph, child, "Included"),
        "included module child lost its source-qualified declaration");
    ok &= check(included_file != NULL && child != CM_MODULE_NONE
        && child_information.effective_item_count == 0u
        && child_information.inner_attribute_count == 0u,
        "empty included module acquired synthetic contents");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_authenticated_item_include(void)
{
    static const unsigned char success_source[] =
        "mod builtins {\n"
        "  #[rustc_builtin_macro]\n"
        "  #[macro_export]\n"
        "  macro_rules! include { ($file:expr $(,)?) => {{}}; }\n"
        "}\n"
        "struct Before; include!(\"items.rs\"); struct After;\n";
    static const unsigned char missing_attribute[] =
        "macro_rules! include { ($file:expr) => {{}}; }\n"
        "include!(\"items.rs\");\n";
    static const unsigned char duplicate_attribute[] =
        "#[rustc_builtin_macro]\n#[rustc_builtin_macro]\n"
        "macro_rules! include { ($file:expr) => {{}}; }\n"
        "include!(\"items.rs\");\n";
    static const unsigned char malformed_attribute[] =
        "#[rustc_builtin_macro(include)]\n"
        "macro_rules! include { ($file:expr) => {{}}; }\n"
        "include!(\"items.rs\");\n";
    static const unsigned char shadowed_source[] =
        "mod builtins {\n"
        "  #[rustc_builtin_macro]\n"
        "  #[macro_export]\n"
        "  macro_rules! include { ($file:expr) => {{}}; }\n"
        "}\n"
        "macro_rules! include { ($file:expr) => { struct Shadowed; }; }\n"
        "include!(\"items.rs\");\n";
    static const unsigned char nonliteral_source[] =
        "#[rustc_builtin_macro]\n"
        "macro_rules! include { ($file:expr) => {{}}; }\n"
        "include!(concat!(\"items\", \".rs\"));\n";
    const unsigned char *rejected[5];
    size_t rejected_lengths[5];
    size_t index;
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root;
    CmCfgSet success_cfg;
    CmModuleGraphOptions success_options;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/include-success/lib.rs", success_source,
        sizeof(success_source) - 1u, &root_source) == CM_SOURCE_OK,
        "failed to add authenticated include success source");
    cm_cfg_set_init(&success_cfg);
    success_cfg.environment.target_family = "unix";
    cm_module_graph_options_init(&success_options);
    success_options.cfg = &success_cfg;
    result = cm_module_graph_build(&graph, &sources, root_source,
        &success_options);
    memset(&root, 0, sizeof(root));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && sources.length == 2u
        && cm_module_graph_get_module(&graph, result.root, &root)
        && effective_named(&graph, result.revision, result.root,
            "Before", NULL, NULL)
        && effective_named(&graph, result.revision, result.root,
            "Included", NULL, NULL)
        && effective_named(&graph, result.revision, result.root,
            "included_function", NULL, NULL)
        && effective_named(&graph, result.revision, result.root,
            "After", NULL, NULL),
        "exact macro_export identity did not authenticate and splice include");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    rejected[0] = missing_attribute;
    rejected[1] = duplicate_attribute;
    rejected[2] = malformed_attribute;
    rejected[3] = shadowed_source;
    rejected[4] = nonliteral_source;
    rejected_lengths[0] = sizeof(missing_attribute) - 1u;
    rejected_lengths[1] = sizeof(duplicate_attribute) - 1u;
    rejected_lengths[2] = sizeof(malformed_attribute) - 1u;
    rejected_lengths[3] = sizeof(shadowed_source) - 1u;
    rejected_lengths[4] = sizeof(nonliteral_source) - 1u;
    for (index = 0u; index < CM_ARRAY_LEN(rejected); ++index) {
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/include-success/lib.rs",
            rejected[index], rejected_lengths[index], &root_source)
            == CM_SOURCE_OK,
            "failed to add authenticated include rejection source");
        result = build_with_empty_cfg(&graph, &sources, root_source);
        ok &= check(result.error_count == 1u && sources.length == 1u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == (index == 3u
                ? CM_RESOLVE_ERROR_ITEM_MACRO
                : CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED)
            && error.span.source == root_source,
            "unauthenticated, shadowed, or malformed include did not fail closed");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    {
        CmCfgSet cfg;
        CmModuleGraphOptions options;
        CmResolveError error;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&sources,
            "tests/resolve/fixtures/include-success/lib.rs",
            success_source, sizeof(success_source) - 1u, &root_source)
            == CM_SOURCE_OK,
            "failed to add disabled authenticated include source");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&options);
        options.cfg = &cfg;
        options.include_expansion = CM_INCLUDE_EXPANSION_DISABLED;
        result = cm_module_graph_build(&graph, &sources, root_source,
            &options);
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u && sources.length == 1u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
            "disabled authenticated include expansion opened a source");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
    return ok;
}

static int test_item_include_rejections(void)
{
    static const unsigned char plain_source[] =
        "include!(\"items.rs\");\n";
    static const unsigned char shadowed_source[] =
        "macro_rules! include { ($file:expr) => { struct Shadowed; }; }\n"
        "include!(\"items.rs\");\n";
    static const unsigned char macro_use_source[] =
        "#[macro_use]\nmod macros;\ninclude!(\"items.rs\");\n";
    static const unsigned char imported_source[] =
        "use other::include;\ninclude!(\"items.rs\");\n";
    static const unsigned char qualified_source[] =
        "core::include!(\"items.rs\");\n";
    static const unsigned char nonliteral_source[] =
        "include!(concat!(\"items\", \".rs\"));\n";
    static const unsigned char absolute_source[] =
        "include!(\"/items.rs\");\n";
    const unsigned char *sources_text[3];
    size_t source_lengths[3];
    size_t index;
    int ok;

    ok = test_error_fixture_with_include_mode(
        "tests/resolve/fixtures/include-cycle/lib.rs",
        CM_RESOLVE_ERROR_INCLUDE_CYCLE,
        CM_INCLUDE_EXPANSION_SOURCE_FIXTURE);
    ok &= test_error_fixture_with_include_mode(
        "tests/resolve/fixtures/include-macro/lib.rs",
        CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED,
        CM_INCLUDE_EXPANSION_SOURCE_FIXTURE);
    {
        CmSourceSet inner_sources;
        CmModuleGraph inner_graph;
        CmModuleGraphResult inner_result;
        CmResolveError inner_error;
        const CmSourceFile *inner_file;

        ok &= check(load_and_build_include_fixture(
            "tests/resolve/fixtures/include-inner-attribute/lib.rs",
            &inner_sources, &inner_graph, &inner_result),
            "failed to load included inner-attribute fixture");
        inner_file = inner_sources.length == 2u
            ? &inner_sources.files[1] : NULL;
        ok &= check(inner_result.root == CM_MODULE_NONE
            && inner_result.error_count == 1u && inner_file != NULL
            && cm_module_graph_get_error(&inner_graph, 0u, &inner_error)
            && inner_error.kind == CM_RESOLVE_ERROR_PARSE
            && inner_error.span.source == inner_file->id,
            "included inner attribute was not rejected at its own source");
        cm_module_graph_destroy(&inner_graph);
        cm_source_set_destroy(&inner_sources);
    }
    sources_text[0] = qualified_source;
    sources_text[1] = nonliteral_source;
    sources_text[2] = absolute_source;
    source_lengths[0] = sizeof(qualified_source) - 1u;
    source_lengths[1] = sizeof(nonliteral_source) - 1u;
    source_lengths[2] = sizeof(absolute_source) - 1u;
    for (index = 0u; index < CM_ARRAY_LEN(sources_text); ++index) {
        CmSourceSet source_set;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphResult result;
        CmResolveError error;

        cm_source_set_init(&source_set);
        cm_module_graph_init(&graph);
        ok &= check(cm_source_add_memory(&source_set,
            "tests/resolve/fixtures/include-success/lib.rs",
            sources_text[index], source_lengths[index], &root)
            == CM_SOURCE_OK, "failed to add include rejection source");
        result = build_include_fixture(&graph, &source_set, root);
        ok &= check(result.root == CM_MODULE_NONE
            && result.error_count == 1u
            && cm_module_graph_get_error(&graph, 0u, &error)
            && error.kind == CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED
            && error.span.source == root,
            "qualified or nonliteral include did not fail closed");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&source_set);
    }
    {
        const unsigned char *binding_sources[4];
        size_t binding_lengths[4];

        binding_sources[0] = plain_source;
        binding_sources[1] = shadowed_source;
        binding_sources[2] = macro_use_source;
        binding_sources[3] = imported_source;
        binding_lengths[0] = sizeof(plain_source) - 1u;
        binding_lengths[1] = sizeof(shadowed_source) - 1u;
        binding_lengths[2] = sizeof(macro_use_source) - 1u;
        binding_lengths[3] = sizeof(imported_source) - 1u;
        for (index = 0u; index < CM_ARRAY_LEN(binding_sources); ++index) {
            CmSourceSet source_set;
            CmSourceId root;
            CmModuleGraph graph;
            CmModuleGraphResult result;
            CmResolveError error;

            cm_source_set_init(&source_set);
            cm_module_graph_init(&graph);
            ok &= check(cm_source_add_memory(&source_set,
                "tests/resolve/fixtures/include-success/lib.rs",
                binding_sources[index], binding_lengths[index], &root)
                == CM_SOURCE_OK, "failed to add include binding source");
            result = index == 0u
                ? build_with_empty_cfg(&graph, &source_set, root)
                : build_include_fixture(&graph, &source_set, root);
            ok &= check(result.root == CM_MODULE_NONE
                && result.error_count == 1u && source_set.length == 1u
                && cm_module_graph_get_error(&graph, 0u, &error)
                && error.kind == (index == 0u
                    ? CM_RESOLVE_ERROR_ITEM_MACRO
                    : CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED)
                && error.span.source == root,
                "disabled or locally shadowed include did not fail closed");
            cm_module_graph_destroy(&graph);
            cm_source_set_destroy(&source_set);
        }
    }
    return ok;
}

static int test_item_include_source_reallocation(void)
{
    static const CmAstItemKind kinds[] = {
        CM_AST_ITEM_STRUCT, CM_AST_ITEM_STRUCT,
        CM_AST_ITEM_STRUCT, CM_AST_ITEM_STRUCT
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo root;
    char path[256];
    int ok;

    ok = check(load_and_build_include_fixture(
        "tests/resolve/fixtures/include-reallocation/lib.rs", &sources,
        &graph, &result),
        "failed to load include source-reallocation fixture");
    if (!ok) return 0;
    memset(&root, 0, sizeof(root));
    ok &= check(sources.length == 5u && result.root != CM_MODULE_NONE
        && result.error_count == 0u
        && cm_module_graph_get_module(&graph, result.root, &root)
        && root.source == sources.files[0].id
        && cm_module_graph_copy_string(&graph, root.source_path,
            path, sizeof(path))
        && strstr(path, "include-reallocation/lib.rs") != NULL
        && module_items_are(&graph, result.root, kinds,
            (uint32_t)CM_ARRAY_LEN(kinds), NULL),
        "source-set growth invalidated the root or included item order");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_include_parse_provenance(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    CmSourceId root;
    const CmSourceFile *included_file;
    int ok;

    ok = check(load_and_build_include_fixture(
        "tests/resolve/fixtures/include-parse-error/lib.rs", &sources,
        &graph, &result), "failed to load include parse fixture");
    if (!ok) return 0;
    root = sources.files[0].id;
    included_file = sources.length == 2u ? &sources.files[1] : NULL;
    ok &= check(result.root == CM_MODULE_NONE && result.error_count == 1u
        && included_file != NULL
        && strstr(included_file->path, "include-parse-error/items.rs")
            != NULL
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_PARSE
        && error.span.source == included_file->id
        && error.span.source != root
        && error.line == 3u
        && error.column == 6u
        && string_equals(&graph, error.detail_a, "expected type"),
        "included parse failure did not retain its source and location");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_include_cfg_provenance(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    const CmSourceFile *included_file;
    int ok;

    ok = check(load_and_build_include_fixture(
        "tests/resolve/fixtures/include-cfg-error/lib.rs", &sources,
        &graph, &result), "failed to load include cfg fixture");
    if (!ok) return 0;
    included_file = sources.length == 2u ? &sources.files[1] : NULL;
    ok &= check(result.root == CM_MODULE_NONE && result.error_count == 1u
        && included_file != NULL
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_CFG_EXPANSION
        && error.span.source == included_file->id
        && error.span.source != sources.files[0].id,
        "included cfg failure did not retain its source provenance");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_primitive_docs_include_unsupported_provenance(
    const char *core_root)
{
    static const unsigned char driver_source[] =
        "include!(\"primitive_docs.rs\");\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveError error;
    const CmSourceFile *included_file;
    char *driver_path;
    size_t root_length;
    int ok;

    if (core_root == NULL || core_root[0] == 0) return 1;
    root_length = strlen(core_root);
    driver_path = (char *)malloc(root_length
        + sizeof("/src/cmrustc-include-driver.rs"));
    if (driver_path == NULL) return 0;
    (void)snprintf(driver_path,
        root_length + sizeof("/src/cmrustc-include-driver.rs"),
        "%s/src/cmrustc-include-driver.rs", core_root);
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources, driver_path, driver_source,
        sizeof(driver_source) - 1u, &root) == CM_SOURCE_OK,
        "failed to add primitive-docs include driver");
    free(driver_path);
    result = build_include_fixture(&graph, &sources, root);
    included_file = sources.length == 2u ? &sources.files[1] : NULL;
    ok &= check(result.root == CM_MODULE_NONE && result.error_count == 1u
        && included_file != NULL
        && strstr(included_file->path, "core/src/primitive_docs.rs") != NULL
        && cm_module_graph_get_error(&graph, 0u, &error)
        && error.kind == CM_RESOLVE_ERROR_INCLUDE_UNSUPPORTED
        && error.span.source == included_file->id
        && error.span.source != root
        && string_equals(&graph, error.detail_a,
            "unsupported item in included source"),
        "parsed primitive_docs.rs did not fail closed with its provenance");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_unnamed_const_namespace(void)
{
    static const char source_text[] =
        "const _: u8 = 0;\n"
        "const _: u8 = 1;\n"
        "const NAMED: u8 = 2;\n";
    CmSourceSet sources;
    CmSourceId source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo information;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/unnamed-const/lib.rs",
        (const unsigned char *)source_text,
        sizeof(source_text) - 1u, &source) == CM_SOURCE_OK,
        "failed to add unnamed const namespace source");
    result = build_with_empty_cfg(&graph, &sources, source);
    memset(&information, 0, sizeof(information));
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && cm_module_graph_get_module(&graph, result.root, &information)
        && information.active_item_count == 3u
        && information.effective_item_count == 3u
        && information.value_count == 1u
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "NAMED")
        && !namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "_"),
        "unnamed const items were lost or introduced a value binding");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_block_local_const_ownership(void)
{
    static const unsigned char source_text[] =
        "const ROOT: u32 = 1u32;\n"
        "fn value() -> u32 {\n"
        "    fn helper() -> u32 { 3u32 }\n"
        "    use crate::ROOT as LOCAL_ROOT;\n"
        "    struct Local(u32);\n"
        "    impl Local {}\n"
        "    #[allow(dead_code)]\n"
        "    const LOCAL: u32 = 2u32;\n"
        "    LOCAL\n"
        "}\n";
    static const CmAstItemKind root_kinds[] = {
        CM_AST_ITEM_CONST,
        CM_AST_ITEM_FUNCTION
    };
    CmSourceSet sources;
    CmSourceId source;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmResolveModuleInfo information;
    const CmAst *ast;
    int ok;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_add_memory(&sources,
        "tests/resolve/fixtures/local-const/lib.rs", source_text,
        sizeof(source_text) - 1u, &source) == CM_SOURCE_OK,
        "failed to add block-local const ownership source");
    result = build_with_empty_cfg(&graph, &sources, source);
    memset(&information, 0, sizeof(information));
    ast = NULL;
    ok &= check(result.root != CM_MODULE_NONE && result.error_count == 0u
        && cm_module_graph_get_module(&graph, result.root, &information)
        && module_items_are(&graph, result.root, root_kinds,
            (uint32_t)CM_ARRAY_LEN(root_kinds), &ast)
        && active_module_items_are(&graph, result.root, root_kinds,
            (uint32_t)CM_ARRAY_LEN(root_kinds))
        && ast != NULL && ast->root_items.len == 2u && ast->items.len == 7u
        && information.value_count == 2u
        && information.active_item_count == 2u
        && information.effective_item_count == 2u
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "ROOT")
        && namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "value")
        && !namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "LOCAL")
        && !namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "helper")
        && !namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_VALUE,
            "LOCAL_ROOT")
        && !namespace_has(&graph, result.root, CM_RESOLVE_NAMESPACE_TYPE,
            "Local")
        && effective_named(&graph, result.revision, result.root, "ROOT",
            NULL, NULL)
        && effective_named(&graph, result.revision, result.root, "value",
            NULL, NULL)
        && !effective_named(&graph, result.revision, result.root, "LOCAL",
            NULL, NULL)
        && !effective_named(&graph, result.revision, result.root, "helper",
            NULL, NULL)
        && !effective_named(&graph, result.revision, result.root,
            "LOCAL_ROOT",
            NULL, NULL)
        && !effective_named(&graph, result.revision, result.root, "Local",
            NULL, NULL),
        "block-local item leaked into module ownership or value views");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_core_authenticated_graph(const char *core_root)
{
    static const char *const macro_parts[] = {
        "core", "bstr", "impl_partial_eq"
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphResult result;
    CmDependencyMacroArtifact artifact;
    CmDependencyMacroArtifactResult artifact_result;
    CmDependencyMacroDefinition macro_definition;
    CmDependencyMacroStatus macro_status;
    CmResolvePathSegmentView macro_path[3];
    const CmAstItem *macro_item;
    CmResolveError error;
    const CmSourceFile *error_file;
    char error_detail[256];
    char error_message[256];
    CmSourceId root;
    char *path;
    size_t root_length;
    size_t index;
    int ok;

    if (core_root == NULL || core_root[0] == 0) return 1;
    root_length = strlen(core_root);
    path = (char *)malloc(root_length + sizeof("/src/lib.rs"));
    if (path == NULL) return 0;
    (void)snprintf(path, root_length + sizeof("/src/lib.rs"),
        "%s/src/lib.rs", core_root);
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_dependency_macro_artifact_init(&artifact);
    for (index = 0u; index < 3u; ++index) {
        macro_path[index].bytes =
            (const unsigned char *)macro_parts[index];
        macro_path[index].length = strlen(macro_parts[index]);
    }
    ok = check(cm_source_load_file(&sources, path, &root) == CM_SOURCE_OK,
        "failed to load real core root for include binding gate");
    free(path);
    if (!ok) {
        cm_dependency_macro_artifact_destroy(&artifact);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return 0;
    }
    result = build_with_empty_cfg(&graph, &sources, root);
    if (result.error_count != 0u || sources.length != 293u
        || cm_module_graph_module_count(&graph) != 378u
        || result.root == CM_MODULE_NONE) {
        memset(&error, 0, sizeof(error));
        error_file = NULL;
        error_detail[0] = 0;
        error_message[0] = 0;
        if (cm_module_graph_get_error(&graph, 0u, &error)) {
            error_file = cm_source_get(&sources, error.span.source);
            (void)cm_module_graph_copy_string(&graph, error.detail_a,
                error_detail, sizeof(error_detail));
            (void)cm_module_graph_copy_string(&graph, error.detail_b,
                error_message, sizeof(error_message));
        }
        fprintf(stderr, "authenticated core graph actual: "
            "errors=%lu sources=%lu modules=%lu root=%u "
            "%s:%lu:%lu span=%lu..%lu: %s: %s\n",
            (unsigned long)result.error_count,
            (unsigned long)sources.length,
            (unsigned long)cm_module_graph_module_count(&graph),
            (unsigned int)result.root,
            error_file == NULL ? "<unknown>" : error_file->path,
            (unsigned long)error.line, (unsigned long)error.column,
            (unsigned long)error.span.start,
            (unsigned long)error.span.end,
            error_detail, error_message);
    }
    ok &= check(result.error_count == 0u
        && sources.length == 293u
        && cm_module_graph_module_count(&graph) == 378u
        && result.root != CM_MODULE_NONE,
        "authenticated Rust 1.90 core module graph is incomplete");
    artifact_result = cm_dependency_macro_artifact_build(&artifact, &graph,
        result.revision, "core", "rust_core");
    memset(&macro_definition, 0, sizeof(macro_definition));
    macro_status = cm_dependency_macro_artifact_lookup(&artifact,
        macro_path, 3u, &macro_definition);
    macro_item = macro_definition.definition_ast == NULL ? NULL
        : cm_ast_get_item(macro_definition.definition_ast,
            macro_definition.declaration.item);
    if (artifact_result.status != CM_DEPENDENCY_MACRO_OK
        || macro_status != CM_DEPENDENCY_MACRO_OK) {
        fprintf(stderr, "authenticated core macro artifact actual: "
            "build=%s imports=%lu lookup=%s\n",
            cm_dependency_macro_status_name(artifact_result.status),
            (unsigned long)artifact_result.import_error_count,
            cm_dependency_macro_status_name(macro_status));
    }
    ok &= check(artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && macro_status == CM_DEPENDENCY_MACRO_OK
        && macro_definition.dependency_graph == &graph
        && macro_definition.dependency_revision == result.revision
        && macro_definition.definition_ast != NULL
        && macro_item != NULL
        && ast_name_equals(macro_definition.definition_ast,
            macro_item->name, "impl_partial_eq")
        && strcmp(macro_definition.crate_identifier, "rust_core") == 0,
        "authenticated core dependency macro artifact is incomplete");
    macro_path[0].bytes = (const unsigned char *)"rust_core";
    macro_path[0].length = strlen("rust_core");
    memset(&macro_definition, 0, sizeof(macro_definition));
    macro_status = cm_dependency_macro_artifact_lookup_generated(&artifact,
        macro_path, 3u, &macro_definition);
    macro_item = macro_definition.definition_ast == NULL ? NULL
        : cm_ast_get_item(macro_definition.definition_ast,
            macro_definition.declaration.item);
    ok &= check(macro_status == CM_DEPENDENCY_MACRO_OK
        && macro_definition.dependency_graph == &graph
        && macro_definition.dependency_revision == result.revision
        && macro_item != NULL
        && ast_name_equals(macro_definition.definition_ast,
            macro_item->name, "impl_partial_eq"),
        "authenticated core generated $crate macro path is incomplete");
    cm_dependency_macro_artifact_destroy(&artifact);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_core_target_cfg_graph(const char *core_root)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmModuleGraphOptions options;
    CmModuleGraphResult result;
    CmCfgSet cfg;
    CmSourceId root;
    char *path;
    size_t root_length;
    int ok;

    if (core_root == NULL || core_root[0] == 0) return 1;
    root_length = strlen(core_root);
    path = (char *)malloc(root_length + sizeof("/src/lib.rs"));
    if (path == NULL) return 0;
    (void)snprintf(path, root_length + sizeof("/src/lib.rs"),
        "%s/src/lib.rs", core_root);
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    ok = check(cm_source_load_file(&sources, path, &root) == CM_SOURCE_OK,
        "failed to load target-configured Rust 1.90 core root");
    free(path);
    if (!ok) {
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return 0;
    }
    cm_module_graph_options_init(&options);
    ok &= check(cm_target_cfg_set(&cfg,
            cm_target_find("x86_64-unknown-linux-gnu")),
        "failed to construct the canonical x86-64 target cfg set");
    options.cfg = &cfg;
    result = cm_module_graph_build(&graph, &sources, root, &options);
    if (result.error_count != 0u || sources.length != 363u
        || cm_module_graph_module_count(&graph) != 451u) {
        CmResolveError error;
        const CmSourceFile *error_file;
        char detail_a[128];
        char detail_b[256];

        memset(&error, 0, sizeof(error));
        error_file = NULL;
        detail_a[0] = 0;
        detail_b[0] = 0;
        if (cm_module_graph_get_error(&graph, 0u, &error)) {
            error_file = cm_source_get(&sources, error.span.source);
            (void)cm_module_graph_copy_string(&graph, error.detail_a,
                detail_a, sizeof(detail_a));
            (void)cm_module_graph_copy_string(&graph, error.detail_b,
                detail_b, sizeof(detail_b));
        }
        fprintf(stderr, "target-configured core graph actual: "
            "errors=%lu sources=%lu modules=%lu %s:%lu:%lu "
            "kind=%u %s: %s\n", (unsigned long)result.error_count,
            (unsigned long)sources.length,
            (unsigned long)cm_module_graph_module_count(&graph),
            error_file == NULL ? "<unknown>" : error_file->path,
            (unsigned long)error.line, (unsigned long)error.column,
            (unsigned int)error.kind, detail_a, detail_b);
    }
    ok &= check(result.error_count == 0u && sources.length == 363u
        && cm_module_graph_module_count(&graph) == 451u,
        "target-configured Rust 1.90 core graph did not complete");
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return ok;
}

static int test_alloc_authenticated_frontier(const char *core_root,
    const char *alloc_root)
{
    CmSourceSet core_sources;
    CmSourceSet alloc_sources;
    CmModuleGraph core_graph;
    CmModuleGraph alloc_graph;
    CmDependencyMacroArtifact core_artifact;
    const CmDependencyMacroArtifact *artifacts[1];
    CmDependencyMacroArtifactResult artifact_result;
    CmModuleGraphResult core_result;
    CmModuleGraphResult alloc_result;
    CmModuleGraphOptions options;
    CmCfgSet cfg;
    CmSourceId core_source;
    CmSourceId alloc_source;
    CmModuleId bstr;
    char *core_path;
    char *alloc_path;
    size_t core_length;
    size_t alloc_length;
    size_t dependency_generated_count;
    uint32_t effective_index;
    int ok;

    if (core_root == NULL || core_root[0] == 0
        || alloc_root == NULL || alloc_root[0] == 0) return 1;
    core_length = strlen(core_root);
    alloc_length = strlen(alloc_root);
    core_path = (char *)malloc(core_length + sizeof("/src/lib.rs"));
    alloc_path = (char *)malloc(alloc_length + sizeof("/src/lib.rs"));
    if (core_path == NULL || alloc_path == NULL) {
        free(alloc_path);
        free(core_path);
        return 0;
    }
    (void)snprintf(core_path, core_length + sizeof("/src/lib.rs"),
        "%s/src/lib.rs", core_root);
    (void)snprintf(alloc_path, alloc_length + sizeof("/src/lib.rs"),
        "%s/src/lib.rs", alloc_root);
    cm_source_set_init(&core_sources);
    cm_source_set_init(&alloc_sources);
    cm_module_graph_init(&core_graph);
    cm_module_graph_init(&alloc_graph);
    cm_dependency_macro_artifact_init(&core_artifact);
    ok = check(cm_source_load_file(&core_sources, core_path,
            &core_source) == CM_SOURCE_OK
        && cm_source_load_file(&alloc_sources, alloc_path,
            &alloc_source) == CM_SOURCE_OK,
        "failed to load real core/alloc roots for dependency graph gate");
    free(alloc_path);
    free(core_path);
    if (!ok) {
        cm_dependency_macro_artifact_destroy(&core_artifact);
        cm_module_graph_destroy(&alloc_graph);
        cm_module_graph_destroy(&core_graph);
        cm_source_set_destroy(&alloc_sources);
        cm_source_set_destroy(&core_sources);
        return 0;
    }
    core_result = build_with_empty_cfg(&core_graph, &core_sources,
        core_source);
    artifact_result = cm_dependency_macro_artifact_build(&core_artifact,
        &core_graph, core_result.revision, "core", "rust_core");
    artifacts[0] = &core_artifact;
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    options.dependency_macros = artifacts;
    options.dependency_macro_count = 1u;
    alloc_result = cm_module_graph_build(&alloc_graph, &alloc_sources,
        alloc_source, &options);
    bstr = child_named(&alloc_graph, alloc_result.root, "bstr");
    dependency_generated_count = 0u;
    if (bstr != CM_MODULE_NONE) {
        CmResolveModuleInfo information;

        memset(&information, 0, sizeof(information));
        if (cm_module_graph_get_module(&alloc_graph, bstr, &information)) {
            for (effective_index = 0u;
                    effective_index < information.effective_item_count;
                    ++effective_index) {
                CmResolveEffectiveItem effective;

                memset(&effective, 0, sizeof(effective));
                if (cm_module_graph_get_effective_item(&alloc_graph,
                        alloc_result.revision, bstr, effective_index,
                        &effective) == CM_RESOLVE_VIEW_OK
                    && effective.provenance.dependency_macro_definition
                        .dependency != CM_RESOLVE_DEPENDENCY_NONE) {
                    dependency_generated_count += 1u;
                }
            }
        }
    }
    if (alloc_result.error_count != 0u) {
        CmResolveError error;
        const CmSourceFile *error_file;
        char detail_a[128];
        char detail_b[256];

        memset(&error, 0, sizeof(error));
        error_file = NULL;
        detail_a[0] = 0;
        detail_b[0] = 0;
        if (cm_module_graph_get_error(&alloc_graph, 0u, &error)) {
            error_file = cm_source_get(&alloc_sources, error.span.source);
            (void)cm_module_graph_copy_string(&alloc_graph, error.detail_a,
                detail_a, sizeof(detail_a));
            (void)cm_module_graph_copy_string(&alloc_graph, error.detail_b,
                detail_b, sizeof(detail_b));
        }
        fprintf(stderr, "authenticated alloc dependency graph actual: "
            "errors=%lu sources=%lu modules=%lu %s:%lu:%lu "
            "span=%lu..%lu %s: %s\n",
            (unsigned long)alloc_result.error_count,
            (unsigned long)alloc_sources.length,
            (unsigned long)cm_module_graph_module_count(&alloc_graph),
            error_file == NULL ? "<unknown>" : error_file->path,
            (unsigned long)error.line, (unsigned long)error.column,
            (unsigned long)error.span.start,
            (unsigned long)error.span.end, detail_a, detail_b);
    }
    ok &= check(core_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && alloc_result.error_count == 0u
        && alloc_sources.length == 62u
        && cm_module_graph_module_count(&alloc_graph) == 67u
        && alloc_result.root != CM_MODULE_NONE
        && bstr != CM_MODULE_NONE
        && dependency_generated_count != 0u,
        "authenticated Rust 1.90 core-to-alloc macro graph is incomplete");
    cm_dependency_macro_artifact_destroy(&core_artifact);
    cm_module_graph_destroy(&alloc_graph);
    cm_module_graph_destroy(&core_graph);
    cm_source_set_destroy(&alloc_sources);
    cm_source_set_destroy(&core_sources);
    return ok;
}

static int test_cycle(void)
{
    const char *link_path;
    int linked;
    int ok;

    link_path = "tests/resolve/fixtures/cycle/again.rs";
    (void)remove(link_path);
    linked = symlink("lib.rs", link_path) == 0;
    ok = check(linked, "failed to create cycle symlink");
    if (!linked) return 0;
    ok &= test_error_fixture("tests/resolve/fixtures/cycle/lib.rs",
        CM_RESOLVE_ERROR_MODULE_CYCLE);
    ok &= check(remove(link_path) == 0, "failed to remove cycle symlink");
    return ok;
}

int main(void)
{
    int ok;

    ok = test_basic_graph();
    ok &= test_error_fixture("tests/resolve/fixtures/ambiguous/lib.rs",
        CM_RESOLVE_ERROR_AMBIGUOUS_MODULE_FILE);
    ok &= test_error_fixture("tests/resolve/fixtures/missing/lib.rs",
        CM_RESOLVE_ERROR_MISSING_MODULE_FILE);
    ok &= test_external_macro_scope();
    ok &= test_external_module_path_attribute();
    ok &= test_staged_macro_use_invocation();
    ok &= test_staged_generated_invocation_rejection();
    ok &= test_effective_views_and_revisions();
    ok &= test_effective_enum_variants();
    ok &= test_recursive_effective_trait_impl_children();
    ok &= test_late_failure_hides_effective_snapshot();
    ok &= test_effective_inner_attributes();
    ok &= test_inactive_root_is_empty();
    ok &= test_cfg_set();
    ok &= test_duplicate_path();
    ok &= test_macro_namespace();
    ok &= test_retained_macro_namespaces();
    ok &= test_macro_exports();
    ok &= test_macro_declaration_metadata_views();
    ok &= test_macro_export_rejections();
    ok &= test_macro_use_scope();
    ok &= test_macro_use_rejections();
    ok &= test_generated_effective_graph();
    ok &= test_dependency_macro_graph_staging();
    ok &= test_dependency_macro_graph_rejections();
    ok &= test_generated_rejections();
    ok &= test_expansion_errors();
    ok &= test_qualified_macro_reexport();
    ok &= test_authenticated_cfg_select();
    ok &= test_nested_external_macro_shadowing();
    ok &= test_external_macro_cfg_revision();
    ok &= test_external_macro_errors();
    ok &= test_item_include_success();
    ok &= test_item_include_module_provenance();
    ok &= test_authenticated_item_include();
    ok &= test_item_include_rejections();
    ok &= test_item_include_source_reallocation();
    ok &= test_include_parse_provenance();
    ok &= test_include_cfg_provenance();
    ok &= test_primitive_docs_include_unsupported_provenance(
        getenv("RUST190_CORE_ROOT"));
    ok &= test_unnamed_const_namespace();
    ok &= test_block_local_const_ownership();
    ok &= test_core_target_cfg_graph(
        getenv("RUST190_CORE_ROOT"));
    ok &= test_core_authenticated_graph(
        getenv("RUST190_CORE_ROOT"));
    ok &= test_alloc_authenticated_frontier(
        getenv("RUST190_CORE_ROOT"), getenv("RUST190_ALLOC_ROOT"));
    ok &= test_cycle();
    if (ok) puts("module graph tests: ok");
    return ok ? 0 : 1;
}
