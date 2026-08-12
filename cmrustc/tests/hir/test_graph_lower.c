#define _POSIX_C_SOURCE 200809L

#include "cm/hir/lower.h"
#include "cm/hir/projection.h"
#include "cm/resolve/dependency_macro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "hir-graph-lower: %s\n", message);
        failures += 1;
    }
}

static int write_text(const char *path, const char *text)
{
    FILE *stream;
    size_t length;

    stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    length = strlen(text);
    if (fwrite(text, 1u, length, stream) != length) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int graph_module_named(const CmModuleGraph *graph,
    const char *name, CmModuleId *out_module)
{
    size_t count;
    size_t index;

    *out_module = CM_MODULE_NONE;
    count = cm_module_graph_module_count(graph);
    for (index = 1u; index <= count; ++index) {
        CmResolveModuleInfo information;
        char buffer[64];

        if (cm_module_graph_get_module_at(graph, index - 1u, &information)
            && cm_module_graph_copy_string(graph, information.name, buffer,
                sizeof(buffer))
            && strcmp(buffer, name) == 0) {
            *out_module = information.id;
            return 1;
        }
    }
    return 0;
}

static int hir_name_is(const CmHirContext *hir, CmInternId id,
    const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_interner_get(&hir->strings, id);
    length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static int hir_import_binding_is(const CmHirContext *hir,
    const CmHirImport *import_value, uint32_t index,
    CmHirNamespace namespace_kind, const char *name, CmHirDefId target)
{
    const CmHirImportBinding *binding;

    if (import_value == NULL || index >= import_value->binding_count
        || import_value->bindings == NULL) {
        return 0;
    }
    binding = &import_value->bindings[index];
    return binding->namespace_kind == namespace_kind
        && hir_name_is(hir, binding->name, name)
        && cm_hir_def_id_equal(binding->target, target)
        && !binding->is_anonymous;
}

static uint32_t hir_import_binding_match_count(const CmHirContext *hir,
    const CmHirImport *import_value, CmHirNamespace namespace_kind,
    const char *name, CmHirDefId target)
{
    uint32_t count;
    uint32_t index;

    count = 0u;
    if (import_value == NULL) return 0u;
    for (index = 0u; index < import_value->binding_count; ++index) {
        if (hir_import_binding_is(hir, import_value, index,
                namespace_kind, name, target)) {
            count += 1u;
        }
    }
    return count;
}

static const CmHirItem *find_hir_item(const CmHirContext *hir,
    CmHirModuleId owner, const char *name)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item != NULL && item->owner_module == owner
            && hir_name_is(hir, item->name, name)) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_hir_item_anywhere(const CmHirContext *hir,
    const char *name)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item != NULL && hir_name_is(hir, item->name, name)) return item;
    }
    return NULL;
}

static const CmHirItem *find_hir_impl(const CmHirContext *hir)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item != NULL && item->kind == CM_HIR_ITEM_IMPL) return item;
    }
    return NULL;
}

static const CmHirItem *find_hir_impl_for(const CmHirContext *hir,
    CmHirDefId self_definition)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirType *self_type;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL) continue;
        self_type = cm_hir_get_type(hir, item->data.impl_item.self_type);
        if (self_type != NULL && self_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(self_type->data.named_type.definition,
                self_definition)) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_hir_associated_item(const CmHirContext *hir,
    CmHirDefId parent, CmHirItemKind kind, const char *name)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item != NULL && item->kind == kind
            && cm_hir_def_id_equal(item->parent_definition, parent)
            && hir_name_is(hir, item->name, name)) {
            return item;
        }
    }
    return NULL;
}

static const CmHirType *hir_named_type_argument(const CmHirContext *hir,
    const CmHirType *type, uint32_t index)
{
    if (type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.arguments == NULL
        || index >= type->data.named_type.argument_count
        || type->data.named_type.arguments[index].kind
            != CM_HIR_GENERIC_ARG_TYPE) {
        return NULL;
    }
    return cm_hir_get_type(hir,
        type->data.named_type.arguments[index].data.type);
}

static const CmHirType *hir_reference_pointee(const CmHirContext *hir,
    const CmHirType *type)
{
    return type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        ? cm_hir_get_type(hir, type->data.reference_type.pointee) : NULL;
}

static int hir_type_is_parameter(const CmHirContext *hir, CmHirTypeId type_id,
    CmHirGenericParamId parameter)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter == parameter;
}

static const CmHirType *hir_generic_type_argument(const CmHirContext *hir,
    const CmHirGenericArg *arguments, uint32_t argument_count,
    uint32_t index)
{
    if (arguments == NULL || index >= argument_count
        || arguments[index].kind != CM_HIR_GENERIC_ARG_TYPE) {
        return NULL;
    }
    return cm_hir_get_type(hir, arguments[index].data.type);
}

static int hir_type_is_self(const CmHirContext *hir, CmHirTypeId type_id,
    CmHirDefId owner)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(type->data.self_type.owner, owner);
}

static int hir_type_is_usize(const CmHirContext *hir, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int hir_type_is_projection(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirDefId self_owner, CmHirDefId trait_definition,
    CmHirDefId associated_definition)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && hir_type_is_self(hir, type->data.projection_type.self_type,
            self_owner)
        && cm_hir_def_id_equal(type->data.projection_type
                .trait_type.definition,
            trait_definition)
        && type->data.projection_type.trait_type.argument_count == 0u
        && cm_hir_def_id_equal(type->data.projection_type
                .associated_type.definition,
            associated_definition)
        && type->data.projection_type.associated_type.argument_count == 0u;
}

static int hir_function_is_ordinary(const CmHirContext *hir,
    const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;

    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) return 0;
    signature = &item->data.function_item.signature;
    return hir_name_is(hir, signature->abi, "Rust")
        && signature->safety == CM_HIR_SAFE && !signature->is_const
        && !signature->is_async && !signature->is_variadic;
}

static int hir_type_is_float(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirFloatType kind)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_FLOAT_KIND
        && type->data.float_type.kind == kind;
}

static int ast_name_is(const CmAst *ast, CmInternId id,
    const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static int effective_item_named(const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module, const char *name,
    CmResolveEffectiveItem *out_item)
{
    CmResolveModuleInfo information;
    const CmAst *ast;
    uint32_t index;

    if (!cm_module_graph_get_module(graph, module, &information)
        || !cm_module_graph_borrow_ast(graph, module, &ast)) {
        return 0;
    }
    for (index = 0u; index < information.effective_item_count; ++index) {
        CmResolveEffectiveItem item;
        const CmAstItem *declaration;

        if (cm_module_graph_get_effective_item(graph, revision, module,
                index, &item) != CM_RESOLVE_VIEW_OK) {
            return 0;
        }
        declaration = cm_ast_get_item(ast, item.declaration.item);
        if (declaration != NULL
            && ast_name_is(ast, declaration->name, name)) {
            *out_item = item;
            return 1;
        }
    }
    return 0;
}

static int effective_child_named(const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module,
    const CmResolveEffectiveItem *parent, const char *name,
    CmResolveEffectiveItem *out_item)
{
    const CmAst *ast;
    uint32_t index;

    if (parent == NULL
        || !cm_module_graph_borrow_ast(graph, module, &ast)) {
        return 0;
    }
    for (index = 0u; index < parent->child_count; ++index) {
        CmResolveEffectiveItem item;
        const CmAstItem *declaration;

        if (cm_module_graph_get_effective_child(graph, revision, module,
                parent->id, index, &item) != CM_RESOLVE_VIEW_OK) {
            return 0;
        }
        declaration = cm_ast_get_item(ast, item.declaration.item);
        if (declaration != NULL
            && ast_name_is(ast, declaration->name, name)) {
            *out_item = item;
            return 1;
        }
    }
    return 0;
}

static int effective_item_has_source(const CmResolveEffectiveItem *item,
    CmSourceId source)
{
    return item != NULL && item->declaration.source == source
        && item->provenance.source_item.source == source
        && item->provenance.source_item.item == item->declaration.item
        && item->provenance.expansion_depth == 0u && !item->is_generated
        && item->span.source == source;
}

static int graph_string_is(const CmModuleGraph *graph,
    CmResolveStringId id, const char *expected)
{
    size_t length;
    char *buffer;
    int result;

    length = cm_module_graph_string_length(graph, id);
    if (length == SIZE_MAX || length != strlen(expected)) return 0;
    buffer = (char *)malloc(length + 1u);
    if (buffer == NULL) return 0;
    result = cm_module_graph_copy_string(graph, id, buffer, length + 1u)
        && memcmp(buffer, expected, length) == 0;
    free(buffer);
    return result;
}

static int graph_hir_string_equal(const CmModuleGraph *graph,
    CmResolveStringId graph_id, const CmHirContext *hir,
    CmInternId hir_id)
{
    size_t length;
    char *buffer;
    int result;

    length = cm_module_graph_string_length(graph, graph_id);
    if (length == SIZE_MAX) return 0;
    buffer = (char *)malloc(length + 1u);
    if (buffer == NULL) return 0;
    result = cm_module_graph_copy_string(graph, graph_id, buffer,
            length + 1u)
        && hir_name_is(hir, hir_id, buffer);
    free(buffer);
    return result;
}

static int effective_attributes_are(const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module,
    const CmResolveEffectiveItem *item, const char *const *expected,
    uint32_t expected_count, CmResolveEffectiveAttribute *attributes)
{
    uint32_t index;

    if (item == NULL || item->attribute_count != expected_count
        || (expected_count != 0u
            && (expected == NULL || attributes == NULL))) {
        return 0;
    }
    for (index = 0u; index < expected_count; ++index) {
        int metadata_ok;

        memset(&attributes[index], 0, sizeof(attributes[index]));
        if (cm_module_graph_get_effective_item_attribute(graph, revision,
                module, item->id, index, &attributes[index])
                != CM_RESOLVE_VIEW_OK) {
            return 0;
        }
        metadata_ok = graph_string_is(graph, attributes[index].metadata,
            expected[index]);
        if (!metadata_ok) {
            fprintf(stderr,
                "hir-graph-lower: effective attribute %u metadata differs\n",
                (unsigned int)index);
            return 0;
        }
        if (attributes[index].owner.source != item->declaration.source
            || attributes[index].owner.item != item->declaration.item
            || attributes[index].source != item->declaration.source
            || attributes[index].span.source != item->declaration.source) {
            return 0;
        }
    }
    return 1;
}

static int hir_attributes_match_graph(const CmModuleGraph *graph,
    const CmHirContext *hir, const CmHirItem *item,
    const CmResolveEffectiveAttribute *attributes, uint32_t attribute_count)
{
    uint32_t index;

    if (item == NULL || item->attribute_count != attribute_count
        || (attribute_count != 0u && attributes == NULL)) {
        return 0;
    }
    for (index = 0u; index < attribute_count; ++index) {
        if (!graph_hir_string_equal(graph, attributes[index].metadata, hir,
                item->attributes[index].metadata)
            || item->attributes[index].source_attribute
                != attributes[index].source_attribute
            || item->attributes[index].expansion_depth
                != attributes[index].expansion_depth
            || item->attributes[index].span.source
                != attributes[index].span.source) {
            return 0;
        }
    }
    return 1;
}

static int source_span_is(const CmSourceSet *sources, CmSpan span,
    const char *expected)
{
    const CmSourceFile *source;
    size_t length;

    source = cm_source_get(sources, span.source);
    length = strlen(expected);
    return source != NULL && span.start <= span.end
        && (size_t)span.end <= source->length
        && (size_t)(span.end - span.start) == length
        && memcmp(source->bytes + span.start, expected, length) == 0;
}

static size_t text_count_between(const char *begin, const char *end,
    const char *needle)
{
    const char *position;
    size_t count;
    size_t needle_length;

    if (begin == NULL || end == NULL || begin > end || needle == NULL
        || needle[0] == '\0') {
        return SIZE_MAX;
    }
    position = begin;
    count = 0u;
    needle_length = strlen(needle);
    while ((position = strstr(position, needle)) != NULL && position < end) {
        count += 1u;
        position += needle_length;
    }
    return count;
}

static char *dump_hir(const CmHirContext *hir)
{
    FILE *stream;
    long length;
    char *text;

    stream = tmpfile();
    if (stream == NULL || cm_hir_dump(stream, hir) != 0
        || fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1u);
    if (text == NULL
        || fread(text, 1u, (size_t)length, stream) != (size_t)length) {
        free(text);
        (void)fclose(stream);
        return NULL;
    }
    text[length] = '\0';
    (void)fclose(stream);
    return text;
}

static int build_file_graph(CmSourceSet *sources, CmModuleGraph *graph,
    const char *root_path, const CmCfgSet *cfg,
    CmModuleGraphRevision *out_revision)
{
    CmModuleGraphOptions options;
    CmModuleGraphResult result;
    CmSourceId root;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    if (cm_source_load_file(sources, root_path, &root) != CM_SOURCE_OK) {
        return 0;
    }
    cm_module_graph_options_init(&options);
    options.cfg = cfg;
    result = cm_module_graph_build(graph, sources, root, &options);
    *out_revision = result.revision;
    return result.root != CM_MODULE_NONE && result.error_count == 0u;
}

static CmHirLowerResult lower_module_graph(CmHirContext *hir,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmHirModuleMap *map, const CmHirLowerOptions *options)
{
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirLowerResult result;

    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, graph, revision);
    (void)import_result;
    result = cm_hir_lower_module_graph(hir, graph, revision, &imports, map,
        options);
    cm_import_resolver_destroy(&imports);
    return result;
}

static int hir_is_empty(const CmHirContext *hir)
{
    return hir->crates.len == 0u && hir->modules.len == 0u
        && hir->items.len == 0u && hir->bodies.len == 0u
        && hir->types.len == 0u && hir->generic_parameters.len == 0u
        && hir->definitions.len == 0u
        && hir->prebound_associated_types.len == 0u
        && hir->strings.entries.len == 0u;
}

typedef enum TestMapMutationKind {
    TEST_MAP_KEEP_ROOT = 0,
    TEST_MAP_SWAP_SIBLINGS
} TestMapMutationKind;

typedef struct TestMapMutation {
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    CmHirContext *hir;
    CmHirModuleMap *map;
    TestMapMutationKind kind;
    unsigned int calls;
    int mutation_ok;
} TestMapMutation;

static CmHirLowerResolution mutate_module_map(void *user_context,
    const CmAst *ast, CmAstPathId path_id, CmHirModuleId current_module,
    CmHirLowerPathUse use)
{
    TestMapMutation *mutation;
    const CmAstPath *path;
    const CmHirItem *local;
    CmHirLowerResolution resolution;
    CmHirModuleMapEntry entries[3];
    CmModuleId root;
    size_t index;
    size_t root_index;
    size_t child_indices[2];
    size_t child_count;

    memset(&resolution, 0, sizeof(resolution));
    mutation = (TestMapMutation *)user_context;
    mutation->calls += 1u;
    path = cm_ast_get_path(ast, path_id);
    if (use != CM_HIR_LOWER_PATH_TYPE || path == NULL
        || path->segment_count != 1u
        || !ast_name_is(ast, path->segments[0].name, "External")) {
        return resolution;
    }
    local = find_hir_item(mutation->hir, current_module, "Local");
    if (local == NULL || cm_hir_module_map_count(mutation->map) != 3u
        || !cm_module_graph_get_root(mutation->graph, &root)) {
        resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
        return resolution;
    }
    root_index = 3u;
    child_count = 0u;
    for (index = 0u; index < 3u; ++index) {
        if (cm_hir_module_map_get(mutation->map, mutation->graph,
                mutation->revision, mutation->hir, index, &entries[index])
            != CM_HIR_MODULE_MAP_OK) {
            resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
            return resolution;
        }
        if (entries[index].module == root) {
            root_index = index;
        } else if (child_count < 2u) {
            child_indices[child_count++] = index;
        }
    }
    if (root_index >= 3u || child_count != 2u) {
        resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
        return resolution;
    }
    cm_hir_module_map_clear(mutation->map);
    mutation->mutation_ok = cm_hir_module_map_bind(mutation->map,
        mutation->graph, mutation->revision, entries[root_index].module,
        mutation->hir, entries[root_index].hir_module)
        == CM_HIR_MODULE_MAP_OK;
    if (mutation->mutation_ok
        && mutation->kind == TEST_MAP_SWAP_SIBLINGS) {
        mutation->mutation_ok = cm_hir_module_map_bind(mutation->map,
            mutation->graph, mutation->revision,
            entries[child_indices[0]].module, mutation->hir,
            entries[child_indices[1]].hir_module) == CM_HIR_MODULE_MAP_OK
            && cm_hir_module_map_bind(mutation->map, mutation->graph,
                mutation->revision, entries[child_indices[1]].module,
                mutation->hir, entries[child_indices[0]].hir_module)
                == CM_HIR_MODULE_MAP_OK;
    }
    if (!mutation->mutation_ok) {
        resolution.kind = CM_HIR_LOWER_RESOLVER_ERROR;
        return resolution;
    }
    resolution.kind = CM_HIR_LOWER_DEFINITION;
    resolution.definition = local->definition;
    resolution.named_type_kind = CM_HIR_TYPE_ADT_KIND;
    return resolution;
}

static void test_module_map_mutation_is_transactional(void)
{
    static const char source[] =
        "mod left {} mod right {} struct Local; "
        "struct Consumer { value: External }";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    int mode;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "map-mutation/lib.rs",
        (const unsigned char *)source, strlen(source), &root_source)
        == CM_SOURCE_OK, "could not add module-map mutation fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    check(graph_result.error_count == 0u
        && cm_module_graph_module_count(&graph) == 3u
        && import_result.error_count == 0u,
        "module-map mutation fixture did not resolve");

    for (mode = (int)TEST_MAP_KEEP_ROOT;
         mode <= (int)TEST_MAP_SWAP_SIBLINGS; ++mode) {
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;
        TestMapMutation mutation;
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;

        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        sentinel_name = cm_hir_intern(&hir, "map-sentinel");
        sentinel_span.source = 992u;
        sentinel_span.start = 4u;
        sentinel_span.end = 5u;
        check(cm_hir_create_crate(&hir, sentinel_name,
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root) == CM_HIR_OK,
            "could not create module-map mutation sentinel");
        memset(&mutation, 0, sizeof(mutation));
        mutation.graph = &graph;
        mutation.revision = graph_result.revision;
        mutation.hir = &hir;
        mutation.map = &map;
        mutation.kind = (TestMapMutationKind)mode;
        cm_hir_lower_options_init(&options);
        options.crate_name = "map_mutation";
        options.resolve_path = mutate_module_map;
        options.resolve_context = &mutation;
        result = cm_hir_lower_module_graph(&hir, &graph,
            graph_result.revision, &imports, &map, &options);
        check(mutation.calls == 1u && mutation.mutation_ok
            && result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_STALE_GRAPH
            && result.crate_id == CM_HIR_CRATE_NONE
            && result.root_module == CM_HIR_MODULE_NONE
            && result.lowered_item_count == 0u
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.items.len == 0u && hir.types.len == 0u
            && hir.definitions.len == 1u
            && cm_interner_length(&hir.strings) == 1u
            && hir_name_is(&hir, sentinel_name, "map-sentinel")
            && cm_hir_get_crate(&hir, sentinel_crate) != NULL
            && cm_hir_get_module(&hir, sentinel_root) != NULL
            && cm_hir_module_map_count(&map) == 0u,
            mode == (int)TEST_MAP_KEEP_ROOT
                ? "root-only callback map mutation was published"
                : "swapped sibling callback map mutation was published");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
    }
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_imported_graph_paths(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmModuleId defs_graph;
    CmModuleId consumer_graph;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId defs_hir;
    CmHirModuleId consumer_hir;
    const CmHirModule *root_module;
    const CmHirModule *defs_module;
    const CmHirModule *consumer_module;
    const CmHirItem *direct;
    const CmHirItem *public_item;
    const CmHirItem *generated;
    const CmHirItem *consumer;
    const CmHirDefId *expected[8];
    uint32_t index;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph,
            "tests/hir/fixtures/import-paths/lib.rs", &cfg, &revision)) {
        check(0, "could not build import-aware HIR fixture");
        return;
    }
    root_graph = CM_MODULE_NONE;
    defs_graph = CM_MODULE_NONE;
    consumer_graph = CM_MODULE_NONE;
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "defs", &defs_graph)
        && graph_module_named(&graph, "consumer", &consumer_graph),
        "import-aware fixture modules are missing");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "import_paths";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-import-lower: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    defs_hir = CM_HIR_MODULE_NONE;
    consumer_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u
        && cm_hir_module_map_lookup_hir(&map, &graph, revision, root_graph,
            &hir, &root_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision, defs_graph,
            &hir, &defs_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision,
            consumer_graph, &hir, &consumer_hir) == CM_HIR_MODULE_MAP_OK,
        "import-aware graph lowering or module mapping failed");
    root_module = cm_hir_get_module(&hir, root_hir);
    defs_module = cm_hir_get_module(&hir, defs_hir);
    consumer_module = cm_hir_get_module(&hir, consumer_hir);
    direct = find_hir_item(&hir, defs_hir, "Direct");
    public_item = find_hir_item(&hir, defs_hir, "Public");
    generated = find_hir_item(&hir, defs_hir, "Generated");
    consumer = find_hir_item(&hir, consumer_hir, "Consumer");
    check(direct != NULL && public_item != NULL && generated != NULL
        && consumer != NULL
        && consumer->data.aggregate_item.field_count == 8u,
        "import-aware declarations or consumer fields are missing");
    check(root_module != NULL && defs_module != NULL
        && consumer_module != NULL && direct != NULL
        && public_item != NULL && generated != NULL
        && root_module->import_count == 3u
        && defs_module->import_count == 0u
        && consumer_module->import_count == 4u,
        "import-aware structural module import counts differ");
    if (root_module != NULL && root_module->import_count == 3u
        && defs_module != NULL && consumer_module != NULL
        && consumer_module->import_count == 4u && direct != NULL
        && public_item != NULL && generated != NULL) {
        const CmHirImport *self_alias;
        const CmHirImport *facade;
        const CmHirImport *surface;
        const CmHirImport *grouped;
        const CmHirImport *module_alias;
        const CmHirImport *root_alias;
        const CmHirImport *glob;

        self_alias = &root_module->imports[0];
        facade = &root_module->imports[1];
        surface = &root_module->imports[2];
        grouped = &consumer_module->imports[0];
        module_alias = &consumer_module->imports[1];
        root_alias = &consumer_module->imports[2];
        glob = &consumer_module->imports[3];
        check(self_alias->kind == CM_HIR_IMPORT_EXTERN_CRATE
            && hir_name_is(&hir, self_alias->tree, "self")
            && self_alias->visibility.kind == CM_HIR_VIS_PRIVATE
            && self_alias->attribute_count == 0u
            && self_alias->binding_count == 1u
            && hir_import_binding_is(&hir, self_alias, 0u,
                CM_HIR_NAMESPACE_TYPE, "local_core",
                root_module->definition)
            && facade->kind == CM_HIR_IMPORT_USE
            && hir_name_is(&hir, facade->tree,
                "crate::defs::Public as Facade")
            && facade->visibility.kind == CM_HIR_VIS_PUBLIC
            && facade->attribute_count == 0u
            && facade->binding_count == 2u
            && hir_import_binding_is(&hir, facade, 0u,
                CM_HIR_NAMESPACE_TYPE, "Facade", public_item->definition)
            && hir_import_binding_is(&hir, facade, 1u,
                CM_HIR_NAMESPACE_VALUE, "Facade", public_item->definition)
            && hir_name_is(&hir, surface->tree,
                "crate::Facade as Surface")
            && surface->visibility.kind == CM_HIR_VIS_PUBLIC
            && surface->attribute_count == 0u
            && surface->binding_count == 2u
            && hir_import_binding_is(&hir, surface, 0u,
                CM_HIR_NAMESPACE_TYPE, "Surface", public_item->definition)
            && hir_import_binding_is(&hir, surface, 1u,
                CM_HIR_NAMESPACE_VALUE, "Surface", public_item->definition),
            "root reexports lost declaration order or resolved targets");
        check(hir_name_is(&hir, grouped->tree,
                "crate::defs::{Direct as Alias, Generated as G}")
            && grouped->visibility.kind == CM_HIR_VIS_PRIVATE
            && grouped->attribute_count == 0u
            && grouped->binding_count == 4u
            && hir_import_binding_is(&hir, grouped, 0u,
                CM_HIR_NAMESPACE_TYPE, "Alias", direct->definition)
            && hir_import_binding_is(&hir, grouped, 1u,
                CM_HIR_NAMESPACE_VALUE, "Alias", direct->definition)
            && hir_import_binding_is(&hir, grouped, 2u,
                CM_HIR_NAMESPACE_TYPE, "G", generated->definition)
            && hir_import_binding_is(&hir, grouped, 3u,
                CM_HIR_NAMESPACE_VALUE, "G", generated->definition)
            && hir_name_is(&hir, module_alias->tree, "crate::defs as d")
            && module_alias->binding_count == 1u
            && hir_import_binding_is(&hir, module_alias, 0u,
                CM_HIR_NAMESPACE_TYPE, "d", defs_module->definition)
            && hir_name_is(&hir, root_alias->tree,
                "crate::{self as root}")
            && root_alias->binding_count == 1u
            && hir_import_binding_is(&hir, root_alias, 0u,
                CM_HIR_NAMESPACE_TYPE, "root", root_module->definition),
            "consumer grouped or module aliases lost structural bindings");
        check(hir_name_is(&hir, glob->tree, "crate::defs::*")
            && glob->visibility.kind == CM_HIR_VIS_PRIVATE
            && glob->attribute_count == 0u
            && glob->binding_count == 6u
            && hir_import_binding_is(&hir, glob, 0u,
                CM_HIR_NAMESPACE_TYPE, "Direct", direct->definition)
            && hir_import_binding_is(&hir, glob, 1u,
                CM_HIR_NAMESPACE_TYPE, "Public", public_item->definition)
            && hir_import_binding_is(&hir, glob, 2u,
                CM_HIR_NAMESPACE_TYPE, "Generated", generated->definition)
            && hir_import_binding_is(&hir, glob, 3u,
                CM_HIR_NAMESPACE_VALUE, "Direct", direct->definition)
            && hir_import_binding_is(&hir, glob, 4u,
                CM_HIR_NAMESPACE_VALUE, "Public", public_item->definition)
            && hir_import_binding_is(&hir, glob, 5u,
                CM_HIR_NAMESPACE_VALUE, "Generated", generated->definition),
            "consumer glob import lost ordered expanded bindings");
    }
    if (direct != NULL && public_item != NULL && generated != NULL
        && consumer != NULL
        && consumer->data.aggregate_item.field_count == 8u) {
        expected[0] = &direct->definition;
        expected[1] = &direct->definition;
        expected[2] = &public_item->definition;
        expected[3] = &public_item->definition;
        expected[4] = &generated->definition;
        expected[5] = &direct->definition;
        expected[6] = &public_item->definition;
        expected[7] = &public_item->definition;
        for (index = 0u; index < 8u; ++index) {
            const CmHirType *field_type;

            field_type = cm_hir_get_type(&hir,
                consumer->data.aggregate_item.fields[index].type);
            check(field_type != NULL
                && field_type->kind == CM_HIR_TYPE_ADT_KIND
                && cm_hir_def_id_equal(
                    field_type->data.named_type.definition,
                    *expected[index]),
                "imported path did not preserve its target definition");
        }
    }
    check(hir.items.len == 4u && hir.definitions.len == 7u,
        "aliases, reexports, extern crates, or use items invented HIR items");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_primitive_import_paths(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmModuleId primitive_graph;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId primitive_hir;
    const CmHirModule *root_module;
    const CmHirModule *primitive_module;
    const CmHirItem *uses;
    const CmHirType *qualified;
    const CmHirType *alias;
    const CmHirType *absolute;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph,
            "tests/hir/fixtures/primitive-imports/lib.rs", &cfg,
            &revision)) {
        check(0, "could not build primitive import HIR fixture");
        return;
    }
    root_graph = CM_MODULE_NONE;
    primitive_graph = CM_MODULE_NONE;
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "primitive", &primitive_graph),
        "primitive import fixture modules are missing");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "primitive_imports";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-primitive-import-lower: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    primitive_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u
        && cm_hir_module_map_lookup_hir(&map, &graph, revision, root_graph,
            &hir, &root_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision,
            primitive_graph, &hir, &primitive_hir)
                == CM_HIR_MODULE_MAP_OK,
        "primitive import graph lowering or module mapping failed");
    root_module = cm_hir_get_module(&hir, root_hir);
    primitive_module = cm_hir_get_module(&hir, primitive_hir);
    uses = find_hir_item(&hir, root_hir, "UsesPrimitives");
    qualified = uses == NULL || uses->data.aggregate_item.field_count != 3u
        ? NULL : cm_hir_get_type(&hir,
            uses->data.aggregate_item.fields[0].type);
    alias = uses == NULL || uses->data.aggregate_item.field_count != 3u
        ? NULL : cm_hir_get_type(&hir,
            uses->data.aggregate_item.fields[1].type);
    absolute = uses == NULL || uses->data.aggregate_item.field_count != 3u
        ? NULL : cm_hir_get_type(&hir,
            uses->data.aggregate_item.fields[2].type);
    check(root_module != NULL && primitive_module != NULL && uses != NULL
        && root_module->import_count == 1u
        && primitive_module->import_count == 2u
        && root_module->imports[0].binding_count == 1u
        && primitive_module->imports[0].binding_count == 1u
        && primitive_module->imports[1].binding_count == 1u,
        "primitive structural imports are missing");
    if (root_module != NULL && root_module->import_count == 1u
        && root_module->imports[0].binding_count == 1u
        && root_module->imports[0].bindings != NULL
        && primitive_module != NULL
        && primitive_module->import_count == 2u
        && primitive_module->imports[0].binding_count == 1u
        && primitive_module->imports[0].bindings != NULL
        && primitive_module->imports[1].binding_count == 1u
        && primitive_module->imports[1].bindings != NULL) {
        const CmHirImportBinding *root_binding;
        const CmHirImportBinding *bool_binding;
        const CmHirImportBinding *u8_binding;

        root_binding = &root_module->imports[0].bindings[0];
        bool_binding = &primitive_module->imports[0].bindings[0];
        u8_binding = &primitive_module->imports[1].bindings[0];
        check(hir_name_is(&hir, root_binding->name, "Byte")
            && root_binding->primitive_kind == CM_HIR_PRIMITIVE_U8
            && cm_hir_def_id_is_none(root_binding->target)
            && hir_name_is(&hir, bool_binding->name, "bool")
            && bool_binding->primitive_kind == CM_HIR_PRIMITIVE_BOOL
            && cm_hir_def_id_is_none(bool_binding->target)
            && hir_name_is(&hir, u8_binding->name, "u8")
            && u8_binding->primitive_kind == CM_HIR_PRIMITIVE_U8
            && cm_hir_def_id_is_none(u8_binding->target),
            "HIR imports lost primitive identity or invented DefIds");
    }
    check(qualified != NULL && qualified->kind == CM_HIR_TYPE_BOOL_KIND
        && alias != NULL && alias->kind == CM_HIR_TYPE_INTEGER_KIND
        && alias->data.integer_type.kind == CM_HIR_INT_U8
        && absolute != NULL && absolute->kind == CM_HIR_TYPE_INTEGER_KIND
        && absolute->data.integer_type.kind == CM_HIR_INT_U8,
        "qualified or aliased primitive paths lowered to the wrong types");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_cross_file_graph(void)
{
    static const char root_text[] =
        "#![cfg_attr(unix, no_core)]\n"
        "#[doc = \"a declaration\"]\n"
        "mod a;\n"
        "#[cfg_attr(unix, doc = \"b declaration\")]\n"
        "mod b;\n"
        "#[allow(non_snake_case)]\n"
        "mod inline_mod { #![allow(dead_code)] "
            "pub struct Inline { pub a: crate::a::A } }\n"
        "use crate::a::A;\n"
        "#[cfg(disabled)] trait Broken { fn impossible(&self); }\n"
        "pub struct Root { pub a: A, pub b: crate::b::B }\n";
    static const char a_text[] =
        "#![doc = \"a module\"]\n"
        "pub struct A { pub b: crate::b::B }\n";
    static const char b_text[] =
        "pub struct B { pub a: crate::a::A }\n";
    char directory[] = "/tmp/cmrustc-hir-graph-XXXXXX";
    char root_path[256];
    char a_path[256];
    char b_path[256];
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision graph_revision;
    CmModuleId root_graph;
    CmModuleId a_graph;
    CmModuleId b_graph;
    CmModuleId inline_graph;
    CmResolveModuleInfo a_information;
    CmResolveModuleInfo b_information;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo inline_information;
    CmResolveEffectiveItem a_item_view;
    CmResolveEffectiveItem b_item_view;
    CmHirContext hir_a;
    CmHirContext hir_b;
    CmHirModuleMap map_a;
    CmHirModuleMap map_b;
    CmHirLowerOptions options;
    CmHirLowerResult result_a;
    CmHirLowerResult result_b;
    CmHirModuleId root_hir;
    CmHirModuleId a_hir;
    CmHirModuleId b_hir;
    CmHirModuleId inline_hir;
    const CmHirItem *root_item;
    const CmHirItem *a_item;
    const CmHirItem *b_item;
    const CmHirItem *inline_item;
    const CmHirType *field_type;
    char *dump_a;
    char *dump_b;
    size_t index;

    check(mkdtemp(directory) != NULL, "could not create graph fixture");
    (void)snprintf(root_path, sizeof(root_path), "%s/lib.rs", directory);
    (void)snprintf(a_path, sizeof(a_path), "%s/a.rs", directory);
    (void)snprintf(b_path, sizeof(b_path), "%s/b.rs", directory);
    check(write_text(root_path, root_text) && write_text(a_path, a_text)
        && write_text(b_path, b_text), "could not write graph fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    if (!build_file_graph(&sources, &graph, root_path, &cfg,
            &graph_revision)) {
        check(0, "could not build cross-file module graph");
        goto cleanup_files;
    }
    check(cm_module_graph_module_count(&graph) == 4u,
        "cfg-disabled declaration changed graph module count");
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "crate", &root_graph)
        && graph_module_named(&graph, "a", &a_graph)
        && graph_module_named(&graph, "b", &b_graph)
        && graph_module_named(&graph, "inline_mod", &inline_graph),
        "graph module names are incomplete");
    check(cm_module_graph_get_module(&graph, root_graph,
            &root_information)
        && cm_module_graph_get_module(&graph, a_graph, &a_information)
        && cm_module_graph_get_module(&graph, b_graph, &b_information)
        && cm_module_graph_get_module(&graph, inline_graph,
            &inline_information)
        && a_information.source != b_information.source
        && root_information.inner_attribute_count == 1u
        && a_information.inner_attribute_count == 1u
        && b_information.inner_attribute_count == 0u
        && inline_information.inner_attribute_count == 1u
        && a_information.effective_item_count == 1u
        && b_information.effective_item_count == 1u
        && cm_module_graph_get_effective_item(&graph, graph_revision,
            a_graph, 0u, &a_item_view) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item(&graph, graph_revision,
            b_graph, 0u, &b_item_view) == CM_RESOLVE_VIEW_OK
        && a_item_view.declaration.item == b_item_view.declaration.item,
        "fixture did not create colliding local item IDs");

    cm_hir_context_init(&hir_a);
    cm_hir_context_init(&hir_b);
    cm_hir_module_map_init(&map_a);
    cm_hir_module_map_init(&map_b);
    cm_hir_lower_options_init(&options);
    options.crate_name = "graph_test";
    result_a = lower_module_graph(&hir_a, &graph, graph_revision,
        &map_a, &options);
    result_b = lower_module_graph(&hir_b, &graph, graph_revision,
        &map_b, &options);
    if (result_a.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower: %s: %s\n",
            cm_hir_lower_error_kind_name(result_a.first_error.kind),
            result_a.first_error.message);
    }
    check(result_a.error_count == 0u && result_b.error_count == 0u,
        "graph lowering failed");
    check(result_a.lowered_item_count == 7u && hir_a.items.len == 4u,
        "use/cfg/module declaration lowering count differs");
    check(hir_a.modules.len == 4u
        && cm_hir_module_map_count(&map_a) == 4u,
        "module map is incomplete");
    check(hir_a.definitions.len == hir_a.modules.len + hir_a.items.len,
        "module declarations allocated duplicate semantic definitions");
    for (index = 0u; index < hir_a.items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir_a.items, index);
        check(item != NULL && item->kind != CM_HIR_ITEM_MODULE,
            "graph lowering inserted a duplicate module item");
    }
    check(cm_hir_module_map_lookup_hir(&map_a, &graph, graph_revision,
            root_graph, &hir_a, &root_hir) == CM_HIR_MODULE_MAP_OK
        && root_hir == result_a.root_module
        && cm_hir_module_map_lookup_hir(&map_a, &graph, graph_revision,
            a_graph, &hir_a, &a_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map_a, &graph, graph_revision,
            b_graph, &hir_a, &b_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map_a, &graph, graph_revision,
            inline_graph, &hir_a, &inline_hir) == CM_HIR_MODULE_MAP_OK,
        "module map lookups failed");
    check(cm_hir_get_module(&hir_a, a_hir)->parent == root_hir
        && cm_hir_get_module(&hir_a, b_hir)->parent == root_hir
        && cm_hir_get_module(&hir_a, inline_hir)->parent == root_hir,
        "mapped HIR hierarchy differs from graph hierarchy");
    {
        const CmHirCrate *crate_value;
        const CmHirModule *root_module;
        const CmHirModule *a_module;
        const CmHirModule *b_module;
        const CmHirModule *inline_module;

        crate_value = cm_hir_get_crate(&hir_a, result_a.crate_id);
        root_module = cm_hir_get_module(&hir_a, root_hir);
        a_module = cm_hir_get_module(&hir_a, a_hir);
        b_module = cm_hir_get_module(&hir_a, b_hir);
        inline_module = cm_hir_get_module(&hir_a, inline_hir);
        check(crate_value != NULL
            && crate_value->inner_attribute_count == 1u
            && hir_name_is(&hir_a,
                crate_value->inner_attributes[0].metadata, "no_core")
            && crate_value->inner_attributes[0].expansion_depth == 1u
            && crate_value->inner_attributes[0].span.source
                == root_information.source
            && root_module != NULL
            && root_module->outer_attribute_count == 0u
            && root_module->inner_attribute_count == 0u
            && a_module != NULL && a_module->outer_attribute_count == 1u
            && hir_name_is(&hir_a,
                a_module->outer_attributes[0].metadata,
                "doc = \"a declaration\"")
            && a_module->outer_attributes[0].span.source
                == root_information.source
            && a_module->inner_attribute_count == 1u
            && hir_name_is(&hir_a,
                a_module->inner_attributes[0].metadata,
                "doc = \"a module\"")
            && a_module->inner_attributes[0].span.source
                == a_information.source
            && b_module != NULL && b_module->outer_attribute_count == 1u
            && hir_name_is(&hir_a,
                b_module->outer_attributes[0].metadata,
                "doc = \"b declaration\"")
            && b_module->outer_attributes[0].expansion_depth == 1u
            && b_module->outer_attributes[0].span.source
                == root_information.source
            && b_module->inner_attribute_count == 0u
            && inline_module != NULL
            && inline_module->outer_attribute_count == 1u
            && hir_name_is(&hir_a,
                inline_module->outer_attributes[0].metadata,
                "allow(non_snake_case)")
            && inline_module->outer_attributes[0].span.source
                == root_information.source
            && inline_module->inner_attribute_count == 1u
            && hir_name_is(&hir_a,
                inline_module->inner_attributes[0].metadata,
                "allow(dead_code)")
            && inline_module->inner_attributes[0].span.source
                == inline_information.source,
            "effective module outer/inner attributes were not retained in HIR");
    }

    root_item = find_hir_item(&hir_a, root_hir, "Root");
    a_item = find_hir_item(&hir_a, a_hir, "A");
    b_item = find_hir_item(&hir_a, b_hir, "B");
    inline_item = find_hir_item(&hir_a, inline_hir, "Inline");
    check(root_item != NULL && a_item != NULL && b_item != NULL
        && inline_item != NULL, "lowered graph declarations are missing");
    field_type = cm_hir_get_type(&hir_a,
        a_item->data.aggregate_item.fields[0].type);
    check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            b_item->definition), "a::A did not resolve sibling b::B");
    field_type = cm_hir_get_type(&hir_a,
        b_item->data.aggregate_item.fields[0].type);
    check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            a_item->definition), "b::B did not resolve sibling a::A");
    field_type = cm_hir_get_type(&hir_a,
        root_item->data.aggregate_item.fields[0].type);
    check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            a_item->definition), "root did not resolve imported a::A");
    field_type = cm_hir_get_type(&hir_a,
        root_item->data.aggregate_item.fields[1].type);
    check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            b_item->definition), "root did not resolve external b::B");
    field_type = cm_hir_get_type(&hir_a,
        inline_item->data.aggregate_item.fields[0].type);
    check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            a_item->definition), "inline module did not resolve a::A");
    check(a_item->span.source == a_information.source
        && b_item->span.source == b_information.source,
        "lowered spans lost source qualification");

    dump_a = dump_hir(&hir_a);
    dump_b = dump_hir(&hir_b);
    if (dump_a != NULL && dump_b != NULL && strcmp(dump_a, dump_b) != 0) {
        fprintf(stderr, "first HIR:\n%ssecond HIR:\n%s", dump_a, dump_b);
    }
    check(dump_a != NULL && dump_b != NULL
        && strcmp(dump_a, dump_b) == 0,
        "repeated graph lowering is not deterministic");
    free(dump_b);
    free(dump_a);
    cm_hir_module_map_destroy(&map_b);
    cm_hir_module_map_destroy(&map_a);
    cm_hir_context_destroy(&hir_b);
    cm_hir_context_destroy(&hir_a);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

cleanup_files:
    (void)unlink(b_path);
    (void)unlink(a_path);
    (void)unlink(root_path);
    (void)rmdir(directory);
}

static void test_include_spliced_item_provenance(void)
{
    const char *root_path;
    CmSourceSet sources;
    CmSourceId root_source;
    CmSourceId included_source;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveModuleInfo root_information;
    CmResolveEffectiveItem included_view;
    CmResolveEffectiveItem function_view;
    CmResolveEffectiveAttribute included_attribute;
    const CmAst *function_ast;
    const CmAstItem *function_declaration;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *before_item;
    const CmHirItem *included_item;
    const CmHirItem *function_item;
    const CmHirItem *after_item;
    const CmHirBody *function_body;
    size_t index;

    root_path = "tests/resolve/fixtures/include-success/lib.rs";
    root_source = 0u;
    included_source = 0u;
    memset(&root_information, 0, sizeof(root_information));
    memset(&included_view, 0, sizeof(included_view));
    memset(&function_view, 0, sizeof(function_view));
    memset(&included_attribute, 0, sizeof(included_attribute));
    function_ast = NULL;
    function_declaration = NULL;
    root_hir = CM_HIR_MODULE_NONE;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_load_file(&sources, root_path, &root_source)
            == CM_SOURCE_OK,
        "could not load include-spliced HIR fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_options.include_expansion = CM_INCLUDE_EXPANSION_SOURCE_FIXTURE;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    for (index = 0u; index < sources.length; ++index) {
        if (strstr(sources.files[index].path,
                "include-success/items.rs") != NULL) {
            included_source = sources.files[index].id;
        }
    }
    check(graph_result.error_count == 0u
        && graph_result.root != CM_MODULE_NONE
        && included_source != 0u && included_source != root_source
        && cm_module_graph_get_module(&graph, graph_result.root,
            &root_information)
        && root_information.effective_item_count == 4u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "Included", &included_view)
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "included_function", &function_view)
        && effective_item_has_source(&included_view, included_source)
        && effective_item_has_source(&function_view, included_source)
        && cm_module_graph_get_effective_item_attribute(&graph,
            graph_result.revision, graph_result.root, included_view.id, 0u,
            &included_attribute) == CM_RESOLVE_VIEW_OK
        && graph_string_is(&graph, included_attribute.metadata,
            "allow(dead_code)")
        && included_attribute.span.source == included_source
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            function_view.declaration, &function_ast)
        && (function_declaration = cm_ast_get_item(function_ast,
            function_view.declaration.item)) != NULL
        && function_declaration->kind == CM_AST_ITEM_FUNCTION
        && function_declaration->data.function_item.body
            != CM_AST_EXPR_NONE,
        "include-spliced graph did not retain exact item provenance");

    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "include_spliced";
    result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower include-spliced: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    before_item = NULL;
    included_item = NULL;
    function_item = NULL;
    after_item = NULL;
    function_body = NULL;
    if (cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir,
            &root_hir) == CM_HIR_MODULE_MAP_OK) {
        before_item = find_hir_item(&hir, root_hir, "Before");
        included_item = find_hir_item(&hir, root_hir, "Included");
        function_item = find_hir_item(&hir, root_hir,
            "included_function");
        after_item = find_hir_item(&hir, root_hir, "After");
    }
    if (function_item != NULL
        && function_item->kind == CM_HIR_ITEM_FUNCTION) {
        function_body = cm_hir_get_body(&hir,
            function_item->data.function_item.body);
    }
    check(import_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 4u
        && hir.crates.len == 1u && hir.modules.len == 1u
        && hir.items.len == 4u && hir.definitions.len == 5u
        && cm_hir_module_map_count(&map) == 1u
        && before_item != NULL && included_item != NULL
        && function_item != NULL && after_item != NULL,
        "include-spliced declarations did not lower as four real HIR items");
    check(before_item != NULL && after_item != NULL
        && before_item->span.source == root_source
        && after_item->span.source == root_source
        && included_item != NULL
        && included_item->span.source == included_source
        && included_item->attribute_count == 1u
        && hir_name_is(&hir, included_item->attributes[0].metadata,
            "allow(dead_code)")
        && included_item->attributes[0].span.source == included_source
        && included_item->attributes[0].source_attribute
            == included_attribute.source_attribute,
        "include-spliced item or attribute lost its included source");
    check(function_item != NULL
        && function_item->span.source == included_source
        && function_body != NULL
        && function_declaration != NULL
        && function_body->state == CM_HIR_BODY_UNLOWERED
        && function_body->source == included_source
        && function_body->source_expression_id
            == function_declaration->data.function_item.body
        && function_body->span.source == included_source,
        "include-spliced function body lost source-qualified identity");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_include_spliced_module_provenance(void)
{
    const char *root_path;
    CmSourceSet sources;
    CmSourceId root_source;
    CmSourceId included_source;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmModuleId child_graph;
    CmResolveModuleInfo child_information;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId child_hir;
    const CmHirModule *child_module;
    size_t index;

    root_path = "tests/resolve/fixtures/include-module/lib.rs";
    root_source = 0u;
    included_source = 0u;
    child_graph = CM_MODULE_NONE;
    memset(&child_information, 0, sizeof(child_information));
    root_hir = CM_HIR_MODULE_NONE;
    child_hir = CM_HIR_MODULE_NONE;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_load_file(&sources, root_path, &root_source)
            == CM_SOURCE_OK,
        "could not load included module HIR fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_options.include_expansion = CM_INCLUDE_EXPANSION_AUTHENTICATED;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    for (index = 0u; index < sources.length; ++index) {
        if (strstr(sources.files[index].path,
                "include-module/items.rs") != NULL) {
            included_source = sources.files[index].id;
        }
    }
    check(graph_result.error_count == 0u
        && included_source != 0u && included_source != root_source
        && graph_module_named(&graph, "Included", &child_graph)
        && cm_module_graph_get_module(&graph, child_graph,
            &child_information)
        && child_information.source == included_source
        && child_information.declaration.source == included_source,
        "included module graph identity is not source-qualified");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "include_module";
    result = lower_module_graph(&hir, &graph, graph_result.revision,
        &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower include-module: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    child_module = NULL;
    if (cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir,
            &root_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, child_graph, &hir,
            &child_hir) == CM_HIR_MODULE_MAP_OK) {
        child_module = cm_hir_get_module(&hir, child_hir);
    }
    check(result.error_count == 0u && result.lowered_item_count == 1u
        && hir.crates.len == 1u && hir.modules.len == 2u
        && hir.items.len == 0u && hir.definitions.len == 2u
        && cm_hir_module_map_count(&map) == 2u
        && child_module != NULL && child_module->parent == root_hir,
        "included module did not lower without a duplicate module item");
    check(child_module != NULL
        && child_module->span.source == included_source
        && child_module->outer_attribute_count == 0u
        && child_module->inner_attribute_count == 0u,
        "included module HIR lost declaration provenance");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_cross_source_method_body_provenance(void)
{
    static const char root_text[] =
        "mod api;\n"
        "mod implementation;\n";
    static const char api_text[] =
        "pub trait Trait { fn invoke(&self) {} }\n";
    static const char implementation_text[] =
        "use crate::api::Trait;\n"
        "struct Concrete;\n"
        "impl Trait for Concrete { fn invoke(&self) {} }\n";
    char directory[] = "/tmp/cmrustc-hir-method-source-XXXXXX";
    char root_path[256];
    char api_path[256];
    char implementation_path[256];
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId api_graph;
    CmModuleId implementation_graph;
    CmResolveModuleInfo api_information;
    CmResolveModuleInfo implementation_information;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;
    const CmHirBody *trait_body;
    const CmHirBody *impl_body;

    if (mkdtemp(directory) == NULL) {
        check(0, "could not create method provenance fixture");
        return;
    }
    (void)snprintf(root_path, sizeof(root_path), "%s/lib.rs", directory);
    (void)snprintf(api_path, sizeof(api_path), "%s/api.rs", directory);
    (void)snprintf(implementation_path, sizeof(implementation_path),
        "%s/implementation.rs", directory);
    if (!write_text(root_path, root_text) || !write_text(api_path, api_text)
        || !write_text(implementation_path, implementation_text)) {
        check(0, "could not write method provenance fixture");
        goto cleanup_files;
    }
    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, root_path, &cfg, &revision)) {
        check(0, "could not build method provenance graph");
        goto cleanup_files;
    }
    api_graph = CM_MODULE_NONE;
    implementation_graph = CM_MODULE_NONE;
    check(graph_module_named(&graph, "api", &api_graph)
        && graph_module_named(&graph, "implementation",
            &implementation_graph)
        && cm_module_graph_get_module(&graph, api_graph, &api_information)
        && cm_module_graph_get_module(&graph, implementation_graph,
            &implementation_information)
        && api_information.source != implementation_information.source,
        "method provenance fixture did not retain distinct sources");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "method_sources";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower method provenance: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "Trait");
    impl_item = find_hir_impl(&hir);
    trait_method = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "invoke");
    impl_method = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "invoke");
    trait_body = trait_method == NULL ? NULL
        : cm_hir_get_body(&hir, trait_method->data.function_item.body);
    impl_body = impl_method == NULL ? NULL
        : cm_hir_get_body(&hir, impl_method->data.function_item.body);
    check(result.error_count == 0u && trait_item != NULL
        && trait_item->kind == CM_HIR_ITEM_TRAIT && impl_item != NULL
        && trait_method != NULL && impl_method != NULL
        && trait_body != NULL && impl_body != NULL
        && cm_hir_def_id_equal(
            impl_method->data.function_item.trait_item_definition,
            trait_method->definition),
        "cross-source trait default or impl override did not lower");
    check(trait_body != NULL && impl_body != NULL
        && trait_body->source == api_information.source
        && impl_body->source == implementation_information.source
        && trait_body->source != impl_body->source
        && trait_body->source_expression_id != CM_AST_EXPR_NONE
        && trait_body->source_expression_id
            == impl_body->source_expression_id
        && trait_body->span.source == trait_body->source
        && impl_body->span.source == impl_body->source,
        "source-qualified method bodies did not disambiguate local expression IDs");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

cleanup_files:
    (void)unlink(implementation_path);
    (void)unlink(api_path);
    (void)unlink(root_path);
    (void)rmdir(directory);
}

static void test_nested_method_effective_children(void)
{
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    static const unsigned char source[] =
        "struct Subject;\n"
        "#[cfg_attr(enabled, doc = \"trait root\")] trait Trait {\n"
        "  #[cfg(disabled)] fn skipped(&self);\n"
        "  #[cfg(enabled)] fn required(&self);\n"
        "  #[cfg_attr(enabled, allow(dead_code))]\n"
        "  #[doc = \"trait provided\"] fn provided(&self) {}\n"
        "}\n"
        "#[allow(dead_code)] impl Trait for Subject {\n"
        "  #[cfg(disabled)] fn skipped(&self) {}\n"
        "  #[cfg(enabled)] fn required(&self) {}\n"
        "  #[cfg_attr(enabled, allow(dead_code))]\n"
        "  #[doc = \"impl provided\"] fn provided(&self) {}\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem trait_view;
    CmResolveEffectiveItem impl_view;
    CmResolveEffectiveItem trait_required_view;
    CmResolveEffectiveItem impl_required_view;
    CmResolveEffectiveItem trait_provided_view;
    CmResolveEffectiveItem impl_provided_view;
    CmResolveEffectiveAttribute trait_allow;
    CmResolveEffectiveAttribute trait_doc;
    CmResolveEffectiveAttribute impl_allow;
    CmResolveEffectiveAttribute impl_doc;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_required;
    const CmHirItem *impl_required;
    const CmHirItem *trait_provided;
    const CmHirItem *impl_provided;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "method-effective/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add nested method effective-child fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    memset(&trait_view, 0, sizeof(trait_view));
    memset(&impl_view, 0, sizeof(impl_view));
    memset(&trait_required_view, 0, sizeof(trait_required_view));
    memset(&impl_required_view, 0, sizeof(impl_required_view));
    memset(&trait_provided_view, 0, sizeof(trait_provided_view));
    memset(&impl_provided_view, 0, sizeof(impl_provided_view));
    memset(&trait_allow, 0, sizeof(trait_allow));
    memset(&trait_doc, 0, sizeof(trait_doc));
    memset(&impl_allow, 0, sizeof(impl_allow));
    memset(&impl_doc, 0, sizeof(impl_doc));
    if (graph_result.error_count == 0u) {
        CmResolveModuleInfo information;
        uint32_t index;

        (void)cm_module_graph_get_module(&graph, graph_result.root,
            &information);
        for (index = 0u; index < information.effective_item_count; ++index) {
            CmResolveEffectiveItem item;

            if (cm_module_graph_get_effective_item(&graph,
                    graph_result.revision, graph_result.root, index, &item)
                    == CM_RESOLVE_VIEW_OK
                && item.item_kind == CM_AST_ITEM_IMPL) {
                impl_view = item;
                break;
            }
        }
    }
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "Trait", &trait_view)
        && trait_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && trait_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && trait_view.child_count == 2u
        && impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && impl_view.child_count == 2u,
        "trait/impl effective-child topology differs");
    check(effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "required",
            &trait_required_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &impl_view, "required", &impl_required_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "provided",
            &trait_provided_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &impl_view, "provided", &impl_provided_view)
        && !effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "skipped",
            &trait_required_view)
        && !effective_child_named(&graph, graph_result.revision,
            graph_result.root, &impl_view, "skipped", &impl_required_view),
        "cfg-disabled method remained in an effective child sequence");
    check(trait_required_view.attribute_count == 0u
        && impl_required_view.attribute_count == 0u
        && trait_provided_view.attribute_count == 2u
        && impl_provided_view.attribute_count == 2u
        && cm_module_graph_get_effective_item_attribute(&graph,
            graph_result.revision, graph_result.root,
            trait_provided_view.id, 0u, &trait_allow) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph,
            graph_result.revision, graph_result.root,
            trait_provided_view.id, 1u, &trait_doc) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph,
            graph_result.revision, graph_result.root,
            impl_provided_view.id, 0u, &impl_allow) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph,
            graph_result.revision, graph_result.root,
            impl_provided_view.id, 1u, &impl_doc) == CM_RESOLVE_VIEW_OK
        && graph_string_is(&graph, trait_allow.metadata,
            "allow(dead_code)")
        && graph_string_is(&graph, trait_doc.metadata,
            "doc = \"trait provided\"")
        && graph_string_is(&graph, impl_allow.metadata,
            "allow(dead_code)")
        && graph_string_is(&graph, impl_doc.metadata,
            "doc = \"impl provided\""),
        "effective child attributes or cfg_attr metadata differ");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower effective children: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "Trait");
    impl_item = find_hir_impl(&hir);
    trait_required = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "required");
    impl_required = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "required");
    trait_provided = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "provided");
    impl_provided = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "provided");
    check(result.error_count == 0u && trait_item != NULL && impl_item != NULL
        && trait_required != NULL && impl_required != NULL
        && trait_provided != NULL && impl_provided != NULL
        && find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "skipped") == NULL
        && find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "skipped") == NULL,
        "HIR did not consume the effective trait/impl child set");
    check(trait_item != NULL && impl_item != NULL
        && trait_item->attribute_count == 1u
        && impl_item->attribute_count == 1u
        && hir_name_is(&hir, trait_item->attributes[0].metadata,
            "doc = \"trait root\"")
        && hir_name_is(&hir, impl_item->attributes[0].metadata,
            "allow(dead_code)")
        && trait_item->attributes[0].expansion_depth == 1u
        && impl_item->attributes[0].expansion_depth == 0u,
        "effective root attributes were not copied structurally into HIR");
    check(trait_provided != NULL && impl_provided != NULL
        && trait_provided->attribute_count == 2u
        && impl_provided->attribute_count == 2u
        && hir_name_is(&hir, trait_provided->attributes[0].metadata,
            "allow(dead_code)")
        && hir_name_is(&hir, trait_provided->attributes[1].metadata,
            "doc = \"trait provided\"")
        && hir_name_is(&hir, impl_provided->attributes[0].metadata,
            "allow(dead_code)")
        && hir_name_is(&hir, impl_provided->attributes[1].metadata,
            "doc = \"impl provided\"")
        && trait_provided->attributes[0].source_attribute
            == trait_allow.source_attribute
        && impl_provided->attributes[1].source_attribute
            == impl_doc.source_attribute,
        "effective child attributes were not copied structurally into HIR");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_method_effective_children(void)
{
    static const unsigned char source[] =
        "macro_rules! make { () => {\n"
        "  trait GeneratedTrait { fn provided(&self) {} }\n"
        "  struct GeneratedSubject;\n"
        "  impl GeneratedTrait for GeneratedSubject {\n"
        "    fn provided(&self) {}\n"
        "  }\n"
        "} }\n"
        "make!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem trait_view;
    CmResolveEffectiveItem impl_view;
    CmResolveEffectiveItem trait_method_view;
    CmResolveEffectiveItem impl_method_view;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;
    const CmHirBody *trait_body;
    const CmHirBody *impl_body;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "generated-method/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generated method effective-child fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    memset(&trait_view, 0, sizeof(trait_view));
    memset(&impl_view, 0, sizeof(impl_view));
    memset(&trait_method_view, 0, sizeof(trait_method_view));
    memset(&impl_method_view, 0, sizeof(impl_method_view));
    if (graph_result.error_count == 0u) {
        CmResolveModuleInfo information;
        uint32_t index;

        (void)cm_module_graph_get_module(&graph, graph_result.root,
            &information);
        for (index = 0u; index < information.effective_item_count; ++index) {
            CmResolveEffectiveItem item;

            if (cm_module_graph_get_effective_item(&graph,
                    graph_result.revision, graph_result.root, index, &item)
                    == CM_RESOLVE_VIEW_OK
                && item.item_kind == CM_AST_ITEM_IMPL) {
                impl_view = item;
                break;
            }
        }
    }
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "GeneratedTrait", &trait_view)
        && trait_view.is_generated && impl_view.is_generated
        && trait_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && trait_view.child_count == 1u && impl_view.child_count == 1u
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "provided",
            &trait_method_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &impl_view, "provided", &impl_method_view),
        "generated trait/impl effective children are missing");
    check(trait_method_view.is_generated && impl_method_view.is_generated
        && trait_method_view.declaration.source == root
        && impl_method_view.declaration.source == root
        && trait_method_view.provenance.source_item.source == 0u
        && trait_method_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && impl_method_view.provenance.source_item.source == 0u
        && impl_method_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && trait_method_view.provenance.macro_invocation.source == root
        && impl_method_view.provenance.macro_invocation.source == root
        && trait_method_view.provenance.macro_definition.source == root
        && impl_method_view.provenance.macro_definition.source == root
        && trait_method_view.provenance.expansion_depth == 1u
        && impl_method_view.provenance.expansion_depth == 1u
        && trait_method_view.span.source == root
        && impl_method_view.span.source == root
        && trait_method_view.span.start == trait_view.span.start
        && impl_method_view.span.start == impl_view.span.start,
        "generated method provenance lost its producing invocation anchor");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generated methods: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "GeneratedTrait");
    impl_item = find_hir_impl(&hir);
    trait_method = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "provided");
    impl_method = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "provided");
    trait_body = trait_method == NULL ? NULL
        : cm_hir_get_body(&hir, trait_method->data.function_item.body);
    impl_body = impl_method == NULL ? NULL
        : cm_hir_get_body(&hir, impl_method->data.function_item.body);
    check(result.error_count == 0u && trait_item != NULL && impl_item != NULL
        && trait_method != NULL && impl_method != NULL
        && trait_body != NULL && impl_body != NULL
        && trait_body->source == root && impl_body->source == root
        && trait_body->source_expression_id != CM_AST_EXPR_NONE
        && impl_body->source_expression_id != CM_AST_EXPR_NONE
        && trait_method->span.source == root && impl_method->span.source == root
        && cm_hir_def_id_equal(
            impl_method->data.function_item.trait_item_definition,
            trait_method->definition),
        "generated trait/impl methods lost body source or semantic linkage");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_as_vec_into_iter_fixture(void)
{
    static const char fixture[] =
        "tests/hir/fixtures/as-vec-into-iter/lib.rs";
    static const CmCfgEntry no_global_oom[] = {
        { "no_global_oom_handling", NULL }
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmModuleId trait_graph;
    CmModuleId into_iter_graph;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo trait_information;
    CmResolveModuleInfo into_iter_information;
    CmResolveEffectiveItem trait_view;
    CmResolveEffectiveItem impl_view;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId trait_hir;
    CmHirModuleId into_iter_hir;
    const CmHirModule *root_module;
    const CmHirModule *trait_module;
    const CmHirModule *into_iter_module;
    const CmHirImport *root_trait_import;
    const CmHirImport *root_into_iter_import;
    const CmHirImport *child_trait_import;
    const CmHirItem *trait_item;
    const CmHirItem *into_iter_item;
    const CmHirItem *impl_item;
    const CmHirItem *trait_associated;
    const CmHirItem *impl_associated;
    const CmHirItem *trait_method;
    const CmHirItem *impl_method;
    const CmHirBody *impl_body;
    const CmHirType *impl_self;
    const CmHirType *impl_self_argument;
    const CmHirType *trait_receiver;
    const CmHirType *trait_receiver_self;
    const CmHirType *trait_return;
    const CmHirType *trait_return_inner;
    const CmHirType *trait_return_argument;
    const CmHirType *impl_receiver;
    const CmHirType *impl_receiver_self;
    const CmHirType *impl_return;
    const CmHirType *impl_return_inner;
    const CmHirType *impl_return_argument;
    char *hir_dump;
    uint32_t index;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        check(0, "could not build active AsVecIntoIter fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    trait_graph = CM_MODULE_NONE;
    into_iter_graph = CM_MODULE_NONE;
    memset(&root_information, 0, sizeof(root_information));
    memset(&trait_information, 0, sizeof(trait_information));
    memset(&into_iter_information, 0, sizeof(into_iter_information));
    memset(&trait_view, 0, sizeof(trait_view));
    memset(&impl_view, 0, sizeof(impl_view));
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "in_place_collect", &trait_graph)
        && graph_module_named(&graph, "into_iter", &into_iter_graph)
        && cm_module_graph_get_module(&graph, root_graph,
            &root_information)
        && cm_module_graph_get_module(&graph, trait_graph,
            &trait_information)
        && cm_module_graph_get_module(&graph, into_iter_graph,
            &into_iter_information)
        && root_information.child_count == 2u
        && root_information.import_count == 2u,
        "active AsVecIntoIter module or reexport topology differs");
    (void)effective_item_named(&graph, revision, trait_graph,
        "AsVecIntoIter", &trait_view);
    for (index = 0u; index < into_iter_information.effective_item_count;
         ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, revision,
                into_iter_graph, index, &item) == CM_RESOLVE_VIEW_OK
            && item.item_kind == CM_AST_ITEM_IMPL) {
            impl_view = item;
            break;
        }
    }
    check(trait_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && trait_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && trait_view.child_count == 2u
        && impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && impl_view.child_count == 2u,
        "real AsVecIntoIter effective children are incomplete");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "as_vec_into_iter";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower AsVecIntoIter: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    trait_hir = CM_HIR_MODULE_NONE;
    into_iter_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u
        && cm_hir_module_map_lookup_hir(&map, &graph, revision, root_graph,
            &hir, &root_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision,
            trait_graph, &hir, &trait_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision,
            into_iter_graph, &hir, &into_iter_hir) == CM_HIR_MODULE_MAP_OK,
        "active AsVecIntoIter fixture did not lower or map modules");
    root_module = cm_hir_get_module(&hir, root_hir);
    trait_module = cm_hir_get_module(&hir, trait_hir);
    into_iter_module = cm_hir_get_module(&hir, into_iter_hir);
    root_trait_import = root_module != NULL
            && root_module->import_count == 2u
        ? &root_module->imports[0] : NULL;
    root_into_iter_import = root_module != NULL
            && root_module->import_count == 2u
        ? &root_module->imports[1] : NULL;
    child_trait_import = into_iter_module != NULL
            && into_iter_module->import_count == 1u
        ? &into_iter_module->imports[0] : NULL;
    trait_item = find_hir_item(&hir, trait_hir, "AsVecIntoIter");
    into_iter_item = find_hir_item(&hir, into_iter_hir, "IntoIter");
    impl_item = find_hir_impl(&hir);
    trait_associated = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    impl_associated = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    trait_method = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "as_into_iter");
    impl_method = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "as_into_iter");
    impl_body = impl_method == NULL ? NULL
        : cm_hir_get_body(&hir, impl_method->data.function_item.body);
    check(root_module != NULL && trait_module != NULL
        && into_iter_module != NULL
        && root_module->import_count == 2u
        && trait_module->import_count == 0u
        && into_iter_module->import_count == 1u
        && root_trait_import != NULL && root_into_iter_import != NULL
        && child_trait_import != NULL,
        "AsVecIntoIter structural root or child imports are missing");
    check(root_trait_import != NULL && root_into_iter_import != NULL
        && child_trait_import != NULL && trait_item != NULL
        && into_iter_item != NULL
        && hir_name_is(&hir, root_trait_import->tree,
            "self::in_place_collect::AsVecIntoIter")
        && root_trait_import->visibility.kind == CM_HIR_VIS_CRATE
        && root_trait_import->attribute_count == 0u
        && root_trait_import->binding_count == 1u
        && root_trait_import->bindings != NULL
        && hir_name_is(&hir, root_trait_import->bindings[0].name,
            "AsVecIntoIter")
        && root_trait_import->bindings[0].namespace_kind
            == CM_HIR_NAMESPACE_TYPE
        && cm_hir_def_id_equal(root_trait_import->bindings[0].target,
            trait_item->definition)
        && !root_trait_import->bindings[0].is_anonymous
        && hir_name_is(&hir, root_into_iter_import->tree,
            "self::into_iter::IntoIter")
        && root_into_iter_import->visibility.kind == CM_HIR_VIS_PUBLIC
        && root_into_iter_import->attribute_count == 1u
        && root_into_iter_import->attributes != NULL
        && hir_name_is(&hir,
            root_into_iter_import->attributes[0].metadata,
            "stable(feature = \"rust1\", since = \"1.0.0\")")
        && root_into_iter_import->binding_count == 1u
        && root_into_iter_import->bindings != NULL
        && hir_name_is(&hir, root_into_iter_import->bindings[0].name,
            "IntoIter")
        && root_into_iter_import->bindings[0].namespace_kind
            == CM_HIR_NAMESPACE_TYPE
        && cm_hir_def_id_equal(root_into_iter_import->bindings[0].target,
            into_iter_item->definition)
        && !root_into_iter_import->bindings[0].is_anonymous
        && hir_name_is(&hir, child_trait_import->tree,
            "super::AsVecIntoIter")
        && child_trait_import->visibility.kind == CM_HIR_VIS_PRIVATE
        && child_trait_import->attribute_count == 0u
        && child_trait_import->binding_count == 1u
        && child_trait_import->bindings != NULL
        && hir_name_is(&hir, child_trait_import->bindings[0].name,
            "AsVecIntoIter")
        && child_trait_import->bindings[0].namespace_kind
            == CM_HIR_NAMESPACE_TYPE
        && cm_hir_def_id_equal(child_trait_import->bindings[0].target,
            trait_item->definition)
        && !child_trait_import->bindings[0].is_anonymous,
        "AsVecIntoIter imports lost visibility, metadata, or bindings");
    hir_dump = dump_hir(&hir);
    check(hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(hir_dump,
            "visibility=public kind=use "
            "tree=\"self::into_iter::IntoIter\" "
            "attrs=1 bindings=1") != NULL
        && strstr(hir_dump,
            "meta=\"stable(feature = \\\"rust1\\\", since = "
            "\\\"1.0.0\\\")\"") != NULL
        && strstr(hir_dump,
            "namespace=type name=\"IntoIter\" target=") != NULL,
        "AsVecIntoIter hir-v27 dump omitted the attributed reexport");
    free(hir_dump);
    check(trait_item != NULL && into_iter_item != NULL && impl_item != NULL
        && trait_associated != NULL && impl_associated != NULL
        && trait_method != NULL && impl_method != NULL
        && trait_item->visibility.kind == CM_HIR_VIS_CRATE
        && trait_item->data.trait_item.safety == CM_HIR_UNSAFE
        && trait_item->attribute_count == 1u
        && hir_name_is(&hir, trait_item->attributes[0].metadata,
            "rustc_specialization_trait")
        && into_iter_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && into_iter_item->generic_parameter_count == 1u
        && impl_item->data.impl_item.safety == CM_HIR_UNSAFE
        && impl_item->generic_parameter_count == 1u
        && impl_item->data.impl_item.has_trait
        && cm_hir_def_id_equal(impl_item->data.impl_item.trait_type.definition,
            trait_item->definition),
        "AsVecIntoIter unsafe headers or child declarations differ");
    check(trait_associated != NULL && impl_associated != NULL
        && trait_method != NULL && impl_method != NULL
        && trait_associated->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_equal(impl_associated->data.type_alias_item
                .trait_item_definition,
            trait_associated->definition)
        && trait_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && impl_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && cm_hir_def_id_equal(impl_method->data.function_item
                .trait_item_definition,
            trait_method->definition)
        && impl_body != NULL && impl_body->state == CM_HIR_BODY_UNLOWERED
        && impl_body->source == into_iter_information.source
        && impl_body->source_expression_id != CM_AST_EXPR_NONE
        && impl_body->span.source == into_iter_information.source
        && cm_hir_def_id_equal(impl_body->owner, impl_method->definition),
        "AsVecIntoIter associated identity or method body provenance differs");

    impl_self = impl_item == NULL ? NULL
        : cm_hir_get_type(&hir, impl_item->data.impl_item.self_type);
    impl_self_argument = impl_self != NULL
            && impl_self->kind == CM_HIR_TYPE_ADT_KIND
            && impl_self->data.named_type.argument_count == 1u
            && impl_self->data.named_type.arguments != NULL
            && impl_self->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE
        ? cm_hir_get_type(&hir,
            impl_self->data.named_type.arguments[0].data.type) : NULL;
    check(impl_self != NULL && impl_self->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_self->data.named_type.definition,
            into_iter_item->definition)
        && impl_self_argument != NULL
        && impl_self_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && impl_self_argument->data.parameter_type.parameter
            == impl_item->generic_parameter_start,
        "AsVecIntoIter generic impl self type was not substituted");
    if (impl_associated != NULL) {
        const CmHirType *target;

        target = cm_hir_get_type(&hir,
            impl_associated->data.type_alias_item.target);
        check(target != NULL && target->kind == CM_HIR_TYPE_PARAMETER_KIND
            && target->data.parameter_type.parameter
                == impl_item->generic_parameter_start,
            "AsVecIntoIter Item target is not the impl type parameter");
    }

    trait_receiver = trait_method == NULL ? NULL
        : cm_hir_get_type(&hir, trait_method->data.function_item.signature
            .parameters[0].type);
    trait_receiver_self = trait_receiver != NULL
            && trait_receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        ? cm_hir_get_type(&hir, trait_receiver->data.reference_type.pointee)
        : NULL;
    trait_return = trait_method == NULL ? NULL
        : cm_hir_get_type(&hir,
            trait_method->data.function_item.signature.return_type);
    trait_return_inner = trait_return != NULL
            && trait_return->kind == CM_HIR_TYPE_REFERENCE_KIND
        ? cm_hir_get_type(&hir, trait_return->data.reference_type.pointee)
        : NULL;
    trait_return_argument = trait_return_inner != NULL
            && trait_return_inner->kind == CM_HIR_TYPE_ADT_KIND
            && trait_return_inner->data.named_type.argument_count == 1u
            && trait_return_inner->data.named_type.arguments != NULL
            && trait_return_inner->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE
        ? cm_hir_get_type(&hir,
            trait_return_inner->data.named_type.arguments[0].data.type)
        : NULL;
    check(trait_method != NULL
        && trait_method->data.function_item.signature.parameter_count == 1u
        && trait_receiver != NULL
        && trait_receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        && trait_receiver->data.reference_type.mutability == CM_HIR_MUTABLE
        && trait_receiver_self != NULL
        && trait_receiver_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(trait_receiver_self->data.self_type.owner,
            trait_item->definition)
        && trait_return != NULL
        && trait_return->kind == CM_HIR_TYPE_REFERENCE_KIND
        && trait_return->data.reference_type.mutability == CM_HIR_MUTABLE
        && trait_return_inner != NULL
        && trait_return_inner->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(trait_return_inner->data.named_type.definition,
            into_iter_item->definition)
        && trait_return_argument != NULL
        && trait_return_argument->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(trait_return_argument->data.projection_type
                .trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(trait_return_argument->data.projection_type
                .associated_type.definition,
            trait_associated->definition),
        "trait AsVecIntoIter receiver or Self::Item return differs");

    impl_receiver = impl_method == NULL ? NULL
        : cm_hir_get_type(&hir, impl_method->data.function_item.signature
            .parameters[0].type);
    impl_receiver_self = impl_receiver != NULL
            && impl_receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        ? cm_hir_get_type(&hir, impl_receiver->data.reference_type.pointee)
        : NULL;
    impl_return = impl_method == NULL ? NULL
        : cm_hir_get_type(&hir,
            impl_method->data.function_item.signature.return_type);
    impl_return_inner = impl_return != NULL
            && impl_return->kind == CM_HIR_TYPE_REFERENCE_KIND
        ? cm_hir_get_type(&hir, impl_return->data.reference_type.pointee)
        : NULL;
    impl_return_argument = impl_return_inner != NULL
            && impl_return_inner->kind == CM_HIR_TYPE_ADT_KIND
            && impl_return_inner->data.named_type.argument_count == 1u
            && impl_return_inner->data.named_type.arguments != NULL
            && impl_return_inner->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE
        ? cm_hir_get_type(&hir,
            impl_return_inner->data.named_type.arguments[0].data.type)
        : NULL;
    check(impl_method != NULL
        && impl_method->data.function_item.signature.parameter_count == 1u
        && impl_receiver != NULL
        && impl_receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        && impl_receiver->data.reference_type.mutability == CM_HIR_MUTABLE
        && impl_receiver_self != NULL
        && impl_receiver_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(impl_receiver_self->data.self_type.owner,
            impl_item->definition)
        && impl_return != NULL
        && impl_return->kind == CM_HIR_TYPE_REFERENCE_KIND
        && impl_return->data.reference_type.mutability == CM_HIR_MUTABLE
        && impl_return_inner != NULL
        && impl_return_inner->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_return_inner->data.named_type.definition,
            into_iter_item->definition)
        && impl_return_argument != NULL
        && impl_return_argument->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(impl_return_argument->data.projection_type
                .trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(impl_return_argument->data.projection_type
                .associated_type.definition,
            trait_associated->definition),
        "impl AsVecIntoIter receiver or Self::Item return differs");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    cm_cfg_set_init(&cfg);
    cfg.environment.entries = no_global_oom;
    cfg.environment.entry_count = CM_ARRAY_LEN(no_global_oom);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        check(0, "could not build no-global-oom AsVecIntoIter fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    trait_graph = CM_MODULE_NONE;
    into_iter_graph = CM_MODULE_NONE;
    memset(&root_information, 0, sizeof(root_information));
    memset(&into_iter_information, 0, sizeof(into_iter_information));
    check(cm_module_graph_get_root(&graph, &root_graph)
        && !graph_module_named(&graph, "in_place_collect", &trait_graph)
        && graph_module_named(&graph, "into_iter", &into_iter_graph)
        && cm_module_graph_get_module(&graph, root_graph,
            &root_information)
        && cm_module_graph_get_module(&graph, into_iter_graph,
            &into_iter_information)
        && root_information.child_count == 1u
        && root_information.import_count == 1u
        && into_iter_information.effective_item_count == 1u,
        "no-global-oom cfg retained a guarded module, reexport, or impl");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "as_vec_into_iter_no_oom";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower AsVecIntoIter no-oom: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    into_iter_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u
        && cm_hir_module_map_lookup_hir(&map, &graph, revision, root_graph,
            &hir, &root_hir) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph, revision,
            into_iter_graph, &hir, &into_iter_hir) == CM_HIR_MODULE_MAP_OK,
        "no-global-oom AsVecIntoIter fixture did not lower");
    root_module = cm_hir_get_module(&hir, root_hir);
    into_iter_module = cm_hir_get_module(&hir, into_iter_hir);
    into_iter_item = find_hir_item(&hir, into_iter_hir, "IntoIter");
    check(into_iter_item != NULL
        && into_iter_item->generic_parameter_count == 1u
        && find_hir_item_anywhere(&hir, "AsVecIntoIter") == NULL
        && find_hir_impl(&hir) == NULL,
        "no-global-oom HIR retained the guarded trait or impl");
    root_into_iter_import = root_module != NULL
            && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    check(root_module != NULL && into_iter_module != NULL
        && root_module->import_count == 1u
        && into_iter_module->import_count == 0u
        && root_into_iter_import != NULL && into_iter_item != NULL
        && hir_name_is(&hir, root_into_iter_import->tree,
            "self::into_iter::IntoIter")
        && root_into_iter_import->visibility.kind == CM_HIR_VIS_PUBLIC
        && root_into_iter_import->attribute_count == 1u
        && root_into_iter_import->attributes != NULL
        && hir_name_is(&hir,
            root_into_iter_import->attributes[0].metadata,
            "stable(feature = \"rust1\", since = \"1.0.0\")")
        && root_into_iter_import->binding_count == 1u
        && root_into_iter_import->bindings != NULL
        && root_into_iter_import->bindings[0].namespace_kind
            == CM_HIR_NAMESPACE_TYPE
        && hir_name_is(&hir, root_into_iter_import->bindings[0].name,
            "IntoIter")
        && cm_hir_def_id_equal(root_into_iter_import->bindings[0].target,
            into_iter_item->definition),
        "no-global-oom HIR imports did not remove only cfg-disabled uses");
    hir_dump = dump_hir(&hir);
    check(hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(hir_dump,
            "tree=\"self::into_iter::IntoIter\"") != NULL
        && strstr(hir_dump,
            "tree=\"self::in_place_collect::AsVecIntoIter\"") == NULL
        && strstr(hir_dump, "tree=\"super::AsVecIntoIter\"") == NULL,
        "no-global-oom hir-v27 dump retained a cfg-disabled import");
    free(hir_dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_future_ready_pending_fixture(void)
{
    static const char fixture[] =
        "tests/hir/fixtures/future-ready/lib.rs";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId future_graph;
    CmModuleId ready_graph;
    CmModuleId pending_graph;
    CmResolveModuleInfo future_information;
    CmResolveModuleInfo ready_information;
    CmResolveModuleInfo pending_information;
    CmResolveEffectiveItem trait_view;
    CmResolveEffectiveItem impl_view;
    CmResolveEffectiveItem pending_struct_view;
    CmResolveEffectiveItem pending_impl_view;
    CmResolveEffectiveItem trait_output_view;
    CmResolveEffectiveItem trait_poll_view;
    CmResolveEffectiveItem impl_output_view;
    CmResolveEffectiveItem impl_poll_view;
    CmResolveEffectiveItem pending_output_view;
    CmResolveEffectiveItem pending_poll_view;
    CmResolveEffectiveAttribute trait_output_stable;
    CmResolveEffectiveAttribute trait_output_lang;
    CmResolveEffectiveAttribute trait_poll_lang;
    CmResolveEffectiveAttribute trait_poll_stable;
    CmResolveEffectiveAttribute impl_poll_inline;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *pin_item;
    const CmHirItem *context_item;
    const CmHirItem *poll_item;
    const CmHirItem *ready_item;
    const CmHirItem *phantom_item;
    const CmHirItem *pending_item;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *pending_impl_item;
    const CmHirItem *trait_output;
    const CmHirItem *impl_output;
    const CmHirItem *trait_poll;
    const CmHirItem *impl_poll;
    const CmHirItem *pending_output;
    const CmHirItem *pending_poll;
    const CmHirBody *impl_body;
    const CmHirBody *pending_body;
    const CmHirType *impl_self;
    const CmHirType *impl_self_argument;
    const CmHirType *trait_receiver;
    const CmHirType *trait_receiver_argument;
    const CmHirType *trait_receiver_self;
    const CmHirType *impl_receiver;
    const CmHirType *impl_receiver_argument;
    const CmHirType *impl_receiver_self;
    const CmHirType *trait_return;
    const CmHirType *trait_return_argument;
    const CmHirType *impl_return;
    const CmHirType *impl_return_argument;
    const CmHirType *pending_field;
    const CmHirType *pending_field_argument;
    const CmHirType *pending_fn_return;
    char *hir_dump;
    uint32_t index;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        CmResolveError error;
        char detail[256];

        memset(&error, 0, sizeof(error));
        detail[0] = '\0';
        if (cm_module_graph_get_error(&graph, 0u, &error)) {
            (void)cm_module_graph_copy_string(&graph, error.detail_a,
                detail, sizeof(detail));
            fprintf(stderr, "hir-graph-lower Future/Ready graph: %s: %s\n",
                cm_resolve_error_kind_name(error.kind), detail);
        }
        check(0, "could not build Rust 1.90 Future/Ready fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    future_graph = CM_MODULE_NONE;
    ready_graph = CM_MODULE_NONE;
    pending_graph = CM_MODULE_NONE;
    memset(&future_information, 0, sizeof(future_information));
    memset(&ready_information, 0, sizeof(ready_information));
    memset(&pending_information, 0, sizeof(pending_information));
    memset(&trait_view, 0, sizeof(trait_view));
    memset(&impl_view, 0, sizeof(impl_view));
    memset(&pending_struct_view, 0, sizeof(pending_struct_view));
    memset(&pending_impl_view, 0, sizeof(pending_impl_view));
    memset(&trait_output_view, 0, sizeof(trait_output_view));
    memset(&trait_poll_view, 0, sizeof(trait_poll_view));
    memset(&impl_output_view, 0, sizeof(impl_output_view));
    memset(&impl_poll_view, 0, sizeof(impl_poll_view));
    memset(&pending_output_view, 0, sizeof(pending_output_view));
    memset(&pending_poll_view, 0, sizeof(pending_poll_view));
    memset(&trait_output_stable, 0, sizeof(trait_output_stable));
    memset(&trait_output_lang, 0, sizeof(trait_output_lang));
    memset(&trait_poll_lang, 0, sizeof(trait_poll_lang));
    memset(&trait_poll_stable, 0, sizeof(trait_poll_stable));
    memset(&impl_poll_inline, 0, sizeof(impl_poll_inline));
    check(graph_module_named(&graph, "future", &future_graph)
        && graph_module_named(&graph, "ready", &ready_graph)
        && graph_module_named(&graph, "pending", &pending_graph)
        && cm_module_graph_get_module(&graph, future_graph,
            &future_information)
        && cm_module_graph_get_module(&graph, ready_graph,
            &ready_information)
        && cm_module_graph_get_module(&graph, pending_graph,
            &pending_information)
        && future_information.source != ready_information.source
        && pending_information.source != ready_information.source
        && pending_information.source != future_information.source,
        "Future, Ready, and Pending fixture modules lost source identity");
    (void)effective_item_named(&graph, revision, future_graph,
        "Future", &trait_view);
    (void)effective_item_named(&graph, revision, pending_graph,
        "Pending", &pending_struct_view);
    for (index = 0u; index < ready_information.effective_item_count;
         ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, revision,
                ready_graph, index, &item) == CM_RESOLVE_VIEW_OK
            && item.item_kind == CM_AST_ITEM_IMPL) {
            impl_view = item;
            break;
        }
    }
    for (index = 0u; index < pending_information.effective_item_count;
         ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, revision,
                pending_graph, index, &item) == CM_RESOLVE_VIEW_OK
            && item.item_kind == CM_AST_ITEM_IMPL) {
            pending_impl_view = item;
            break;
        }
    }
    check(trait_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && trait_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && trait_view.child_count == 2u && trait_view.attribute_count == 6u
        && impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && impl_view.child_count == 2u && impl_view.attribute_count == 1u
        && pending_struct_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && pending_struct_view.attribute_count == 2u
        && pending_impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && pending_impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && pending_impl_view.child_count == 2u
        && pending_impl_view.attribute_count == 1u
        && effective_child_named(&graph, revision, future_graph,
            &trait_view, "Output", &trait_output_view)
        && effective_child_named(&graph, revision, future_graph,
            &trait_view, "poll", &trait_poll_view)
        && effective_child_named(&graph, revision, ready_graph,
            &impl_view, "Output", &impl_output_view)
        && effective_child_named(&graph, revision, ready_graph,
            &impl_view, "poll", &impl_poll_view),
        "Future/Ready effective trait or impl children are incomplete");
    check(effective_child_named(&graph, revision, pending_graph,
            &pending_impl_view, "Output", &pending_output_view)
        && effective_child_named(&graph, revision, pending_graph,
            &pending_impl_view, "poll", &pending_poll_view)
        && pending_output_view.attribute_count == 0u
        && pending_poll_view.attribute_count == 0u
        && pending_output_view.declaration.source
            == pending_information.source
        && pending_poll_view.declaration.source == pending_information.source
        && !pending_output_view.is_generated && !pending_poll_view.is_generated,
        "Pending effective impl children or provenance are incomplete");
    check(trait_output_view.attribute_count == 2u
        && trait_poll_view.attribute_count == 2u
        && impl_output_view.attribute_count == 0u
        && impl_poll_view.attribute_count == 1u
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            future_graph, trait_output_view.id, 0u,
            &trait_output_stable) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            future_graph, trait_output_view.id, 1u,
            &trait_output_lang) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            future_graph, trait_poll_view.id, 0u,
            &trait_poll_lang) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            future_graph, trait_poll_view.id, 1u,
            &trait_poll_stable) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ready_graph, impl_poll_view.id, 0u,
            &impl_poll_inline) == CM_RESOLVE_VIEW_OK
        && graph_string_is(&graph, trait_output_stable.metadata,
            "stable(feature = \"futures_api\", since = \"1.36.0\")")
        && graph_string_is(&graph, trait_output_lang.metadata,
            "lang = \"future_output\"")
        && graph_string_is(&graph, trait_poll_lang.metadata,
            "lang = \"poll\"")
        && graph_string_is(&graph, trait_poll_stable.metadata,
            "stable(feature = \"futures_api\", since = \"1.36.0\")")
        && graph_string_is(&graph, impl_poll_inline.metadata, "inline"),
        "Future/Ready effective associated or method attributes differ");
    check(trait_output_view.declaration.source == future_information.source
        && trait_output_view.provenance.source_item.source
            == trait_output_view.declaration.source
        && trait_output_view.provenance.source_item.item
            == trait_output_view.declaration.item
        && trait_poll_view.declaration.source == future_information.source
        && trait_poll_view.provenance.source_item.source
            == trait_poll_view.declaration.source
        && trait_poll_view.provenance.source_item.item
            == trait_poll_view.declaration.item
        && impl_output_view.declaration.source == ready_information.source
        && impl_output_view.provenance.source_item.source
            == impl_output_view.declaration.source
        && impl_output_view.provenance.source_item.item
            == impl_output_view.declaration.item
        && impl_poll_view.declaration.source == ready_information.source
        && impl_poll_view.provenance.source_item.source
            == impl_poll_view.declaration.source
        && impl_poll_view.provenance.source_item.item
            == impl_poll_view.declaration.item
        && trait_output_view.provenance.expansion_depth == 0u
        && trait_poll_view.provenance.expansion_depth == 0u
        && impl_output_view.provenance.expansion_depth == 0u
        && impl_poll_view.provenance.expansion_depth == 0u
        && !trait_output_view.is_generated && !trait_poll_view.is_generated
        && !impl_output_view.is_generated && !impl_poll_view.is_generated
        && trait_output_view.span.source == future_information.source
        && trait_poll_view.span.source == future_information.source
        && impl_output_view.span.source == ready_information.source
        && impl_poll_view.span.source == ready_information.source,
        "Future/Ready effective children lost source provenance");
    check(trait_output_stable.owner.source
            == trait_output_view.declaration.source
        && trait_output_stable.owner.item
            == trait_output_view.declaration.item
        && trait_output_lang.owner.source
            == trait_output_view.declaration.source
        && trait_output_lang.owner.item == trait_output_view.declaration.item
        && trait_poll_lang.owner.source == trait_poll_view.declaration.source
        && trait_poll_lang.owner.item == trait_poll_view.declaration.item
        && trait_poll_stable.owner.source
            == trait_poll_view.declaration.source
        && trait_poll_stable.owner.item == trait_poll_view.declaration.item
        && impl_poll_inline.owner.source == impl_poll_view.declaration.source
        && impl_poll_inline.owner.item == impl_poll_view.declaration.item
        && trait_output_stable.span.source == future_information.source
        && trait_output_lang.span.source == future_information.source
        && trait_poll_lang.span.source == future_information.source
        && trait_poll_stable.span.source == future_information.source
        && impl_poll_inline.span.source == ready_information.source,
        "Future/Ready attribute provenance differs from its declaration");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "future_ready";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower Future/Ready: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    pin_item = find_hir_item_anywhere(&hir, "Pin");
    context_item = find_hir_item_anywhere(&hir, "Context");
    poll_item = find_hir_item_anywhere(&hir, "Poll");
    ready_item = find_hir_item_anywhere(&hir, "Ready");
    phantom_item = find_hir_item_anywhere(&hir, "PhantomData");
    pending_item = find_hir_item_anywhere(&hir, "Pending");
    trait_item = find_hir_item_anywhere(&hir, "Future");
    impl_item = ready_item == NULL ? NULL
        : find_hir_impl_for(&hir, ready_item->definition);
    pending_impl_item = pending_item == NULL ? NULL
        : find_hir_impl_for(&hir, pending_item->definition);
    trait_output = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    impl_output = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    trait_poll = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "poll");
    impl_poll = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "poll");
    pending_output = pending_impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, pending_impl_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    pending_poll = pending_impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, pending_impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "poll");
    impl_body = impl_poll == NULL ? NULL
        : cm_hir_get_body(&hir, impl_poll->data.function_item.body);
    pending_body = pending_poll == NULL ? NULL
        : cm_hir_get_body(&hir, pending_poll->data.function_item.body);
    check(result.error_count == 0u && pin_item != NULL
        && context_item != NULL && poll_item != NULL && ready_item != NULL
        && phantom_item != NULL && pending_item != NULL
        && trait_item != NULL && impl_item != NULL
        && pending_impl_item != NULL && trait_output != NULL
        && impl_output != NULL && trait_poll != NULL && impl_poll != NULL
        && pending_output != NULL && pending_poll != NULL,
        "Rust 1.90 Future/Ready/Pending fixture did not lower completely");
    if (context_item != NULL && trait_poll != NULL && impl_poll != NULL
        && trait_poll->data.function_item.signature.parameter_count == 2u
        && trait_poll->data.function_item.signature.parameters != NULL
        && impl_poll->data.function_item.signature.parameter_count == 2u
        && impl_poll->data.function_item.signature.parameters != NULL) {
        const CmHirGenericParam *context_lifetime;
        const CmHirType *trait_context_reference;
        const CmHirType *trait_context;
        const CmHirType *impl_context_reference;
        const CmHirType *impl_context;

        context_lifetime = cm_hir_get_generic_param(&hir,
            context_item->generic_parameter_start);
        trait_context_reference = cm_hir_get_type(&hir,
            trait_poll->data.function_item.signature.parameters[1].type);
        trait_context = hir_reference_pointee(&hir,
            trait_context_reference);
        impl_context_reference = cm_hir_get_type(&hir,
            impl_poll->data.function_item.signature.parameters[1].type);
        impl_context = hir_reference_pointee(&hir,
            impl_context_reference);
        check(context_item->generic_parameter_count == 1u
            && context_lifetime != NULL
            && context_lifetime->kind == CM_HIR_GENERIC_LIFETIME
            && trait_context != NULL
            && trait_context->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(trait_context->data.named_type.definition,
                context_item->definition)
            && trait_context->data.named_type.argument_count == 1u
            && trait_context->data.named_type.arguments != NULL
            && trait_context->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_LIFETIME
            && impl_context != NULL
            && impl_context->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(impl_context->data.named_type.definition,
                context_item->definition)
            && impl_context->data.named_type.argument_count == 1u
            && impl_context->data.named_type.arguments != NULL
            && impl_context->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_LIFETIME,
            "Future/Ready Context<'_> lifetime argument was not retained");
    } else {
        check(0, "Future/Ready Context<'_> parameters are missing");
    }
    check(trait_item != NULL && impl_item != NULL
        && trait_item->attribute_count == 6u
        && hir_name_is(&hir, trait_item->attributes[0].metadata,
            "doc(notable_trait)")
        && hir_name_is(&hir, trait_item->attributes[1].metadata,
            "doc(search_unbox)")
        && hir_name_is(&hir, trait_item->attributes[2].metadata,
            "must_use = \"futures do nothing unless you `.await` or poll "
            "them\"")
        && hir_name_is(&hir, trait_item->attributes[3].metadata,
            "stable(feature = \"futures_api\", since = \"1.36.0\")")
        && hir_name_is(&hir, trait_item->attributes[4].metadata,
            "lang = \"future_trait\"")
        && hir_name_is(&hir, trait_item->attributes[5].metadata,
            "diagnostic::on_unimplemented(\n"
            "    label = \"`{Self}` is not a future\",\n"
            "    message = \"`{Self}` is not a future\"\n"
            ")")
        && impl_item->attribute_count == 1u
        && hir_name_is(&hir, impl_item->attributes[0].metadata,
            "stable(feature = \"future_readiness_fns\", since = "
            "\"1.48.0\")"),
        "Future trait or Ready impl attributes differ from Rust 1.90");
    check(trait_output != NULL && impl_output != NULL
        && trait_poll != NULL && impl_poll != NULL
        && trait_output->attribute_count == 2u
        && hir_name_is(&hir, trait_output->attributes[0].metadata,
            "stable(feature = \"futures_api\", since = \"1.36.0\")")
        && hir_name_is(&hir, trait_output->attributes[1].metadata,
            "lang = \"future_output\"")
        && trait_output->attributes[0].source_attribute
            == trait_output_stable.source_attribute
        && trait_output->attributes[1].source_attribute
            == trait_output_lang.source_attribute
        && trait_poll->attribute_count == 2u
        && hir_name_is(&hir, trait_poll->attributes[0].metadata,
            "lang = \"poll\"")
        && hir_name_is(&hir, trait_poll->attributes[1].metadata,
            "stable(feature = \"futures_api\", since = \"1.36.0\")")
        && trait_poll->attributes[0].source_attribute
            == trait_poll_lang.source_attribute
        && trait_poll->attributes[1].source_attribute
            == trait_poll_stable.source_attribute
        && impl_output->attribute_count == 0u
        && impl_poll->attribute_count == 1u
        && hir_name_is(&hir, impl_poll->attributes[0].metadata, "inline")
        && impl_poll->attributes[0].source_attribute
            == impl_poll_inline.source_attribute
        && trait_output->span.source == future_information.source
        && trait_poll->span.source == future_information.source
        && impl_output->span.source == ready_information.source
        && impl_poll->span.source == ready_information.source,
        "Future/Ready HIR child attributes or provenance differ");
    check(trait_output != NULL && impl_output != NULL
        && trait_poll != NULL && impl_poll != NULL
        && trait_output->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_equal(impl_output->data.type_alias_item
                .trait_item_definition,
            trait_output->definition)
        && cm_hir_def_id_equal(impl_poll->data.function_item
                .trait_item_definition,
            trait_poll->definition),
        "Ready associated type or poll method linkage is not exact");

    impl_self = impl_item == NULL ? NULL
        : cm_hir_get_type(&hir, impl_item->data.impl_item.self_type);
    impl_self_argument = hir_named_type_argument(&hir, impl_self, 0u);
    check(impl_item != NULL && ready_item != NULL && trait_item != NULL
        && impl_item->generic_parameter_count == 1u
        && impl_item->data.impl_item.has_trait
        && cm_hir_def_id_equal(impl_item->data.impl_item
                .trait_type.definition,
            trait_item->definition)
        && impl_self != NULL && impl_self->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_self->data.named_type.definition,
            ready_item->definition)
        && impl_self_argument != NULL
        && impl_self_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && impl_self_argument->data.parameter_type.parameter
            == impl_item->generic_parameter_start,
        "Ready<T> impl header does not retain its generic ownership");
    if (impl_output != NULL && impl_item != NULL) {
        const CmHirType *target;

        target = cm_hir_get_type(&hir,
            impl_output->data.type_alias_item.target);
        check(target != NULL && target->kind == CM_HIR_TYPE_PARAMETER_KIND
            && target->data.parameter_type.parameter
                == impl_item->generic_parameter_start,
            "Ready Future::Output is not the impl type parameter");
    }
    if (pending_output != NULL && pending_impl_item != NULL) {
        const CmHirType *target;
        const CmHirType *self_type;
        const CmHirType *self_argument;

        target = cm_hir_get_type(&hir,
            pending_output->data.type_alias_item.target);
        self_type = cm_hir_get_type(&hir,
            pending_impl_item->data.impl_item.self_type);
        self_argument = hir_named_type_argument(&hir, self_type, 0u);
        check(pending_impl_item->generic_parameter_count == 1u
            && pending_impl_item->data.impl_item.has_trait
            && cm_hir_def_id_equal(pending_impl_item->data.impl_item
                    .trait_type.definition,
                trait_item->definition)
            && self_type != NULL && self_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(self_type->data.named_type.definition,
                pending_item->definition)
            && self_argument != NULL
            && self_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
            && self_argument->data.parameter_type.parameter
                == pending_impl_item->generic_parameter_start
            && target != NULL && target->kind == CM_HIR_TYPE_PARAMETER_KIND
            && target->data.parameter_type.parameter
                == pending_impl_item->generic_parameter_start
            && cm_hir_def_id_equal(pending_output->data.type_alias_item
                    .trait_item_definition,
                trait_output->definition)
            && cm_hir_def_id_equal(pending_poll->data.function_item
                    .trait_item_definition,
                trait_poll->definition),
            "Pending<T> impl header, Output, or trait linkage differs");
    }

    pending_field = pending_item == NULL
        || pending_item->data.aggregate_item.field_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            pending_item->data.aggregate_item.fields[0].type);
    pending_field_argument = hir_named_type_argument(&hir, pending_field, 0u);
    pending_fn_return = pending_field_argument == NULL
        || pending_field_argument->kind != CM_HIR_TYPE_FN_POINTER_KIND
        ? NULL : cm_hir_get_type(&hir,
            pending_field_argument->data.fn_pointer_type.return_type);
    check(pending_item != NULL && phantom_item != NULL
        && pending_item->attribute_count == 2u
        && hir_name_is(&hir, pending_item->attributes[0].metadata,
            "stable(feature = \"future_readiness_fns\", since = "
            "\"1.48.0\")")
        && hir_name_is(&hir, pending_item->attributes[1].metadata,
            "must_use = \"futures do nothing unless you `.await` or poll "
            "them\"")
        && pending_item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
        && pending_item->data.aggregate_item.field_count == 1u
        && hir_name_is(&hir,
            pending_item->data.aggregate_item.fields[0].name, "_data")
        && pending_field != NULL && pending_field->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(pending_field->data.named_type.definition,
            phantom_item->definition)
        && pending_field_argument != NULL
        && pending_field_argument->kind == CM_HIR_TYPE_FN_POINTER_KIND
        && pending_field_argument->data.fn_pointer_type.parameter_count == 0u
        && pending_fn_return != NULL
        && pending_fn_return->kind == CM_HIR_TYPE_PARAMETER_KIND
        && pending_fn_return->data.parameter_type.parameter
            == pending_item->generic_parameter_start,
        "Pending _data PhantomData<fn() -> T> structure differs");

    trait_receiver = trait_poll == NULL ? NULL
        : cm_hir_get_type(&hir, trait_poll->data.function_item.signature
            .parameters[0].type);
    trait_receiver_argument = hir_named_type_argument(&hir,
        trait_receiver, 0u);
    trait_receiver_self = hir_reference_pointee(&hir,
        trait_receiver_argument);
    impl_receiver = impl_poll == NULL ? NULL
        : cm_hir_get_type(&hir, impl_poll->data.function_item.signature
            .parameters[0].type);
    impl_receiver_argument = hir_named_type_argument(&hir,
        impl_receiver, 0u);
    impl_receiver_self = hir_reference_pointee(&hir,
        impl_receiver_argument);
    check(trait_poll != NULL && impl_poll != NULL && pin_item != NULL
        && trait_poll->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_CUSTOM
        && impl_poll->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_CUSTOM
        && trait_poll->data.function_item.signature.parameter_count == 2u
        && impl_poll->data.function_item.signature.parameter_count == 2u
        && trait_receiver != NULL
        && trait_receiver->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(trait_receiver->data.named_type.definition,
            pin_item->definition)
        && trait_receiver_argument != NULL
        && trait_receiver_argument->kind == CM_HIR_TYPE_REFERENCE_KIND
        && trait_receiver_argument->data.reference_type.mutability
            == CM_HIR_MUTABLE
        && trait_receiver_self != NULL
        && trait_receiver_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(trait_receiver_self->data.self_type.owner,
            trait_item->definition)
        && impl_receiver != NULL
        && impl_receiver->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_receiver->data.named_type.definition,
            pin_item->definition)
        && impl_receiver_argument != NULL
        && impl_receiver_argument->kind == CM_HIR_TYPE_REFERENCE_KIND
        && impl_receiver_argument->data.reference_type.mutability
            == CM_HIR_MUTABLE
        && impl_receiver_self != NULL
        && impl_receiver_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(impl_receiver_self->data.self_type.owner,
            impl_item->definition),
        "Future/Ready custom Pin<&mut Self> receiver ownership differs");
    if (pending_poll != NULL && pending_impl_item != NULL) {
        const CmHirType *receiver_type;
        const CmHirType *receiver_argument;
        const CmHirType *receiver_self;
        const CmHirType *return_type;
        const CmHirType *return_argument;

        receiver_type = cm_hir_get_type(&hir,
            pending_poll->data.function_item.signature.parameters[0].type);
        receiver_argument = hir_named_type_argument(&hir, receiver_type, 0u);
        receiver_self = hir_reference_pointee(&hir, receiver_argument);
        return_type = cm_hir_get_type(&hir,
            pending_poll->data.function_item.signature.return_type);
        return_argument = hir_named_type_argument(&hir, return_type, 0u);
        check(pending_impl_item->attribute_count == 1u
            && hir_name_is(&hir,
                pending_impl_item->attributes[0].metadata,
                "stable(feature = \"future_readiness_fns\", since = "
                "\"1.48.0\")")
            && pending_poll->attribute_count == 0u
            && pending_poll->data.function_item.signature.receiver
                == CM_HIR_RECEIVER_CUSTOM
            && pending_poll->data.function_item.signature.parameter_count
                == 2u
            && pending_poll->data.function_item.signature.parameters[0]
                .binding_kind == CM_HIR_BINDING_NAMED
            && hir_name_is(&hir, pending_poll->data.function_item.signature
                    .parameters[0].name,
                "self")
            && pending_poll->data.function_item.signature.parameters[1]
                .binding_kind == CM_HIR_BINDING_DISCARD
            && pending_poll->data.function_item.signature.parameters[1].name
                == CM_INTERN_ID_NONE
            && receiver_type != NULL
            && receiver_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(receiver_type->data.named_type.definition,
                pin_item->definition)
            && receiver_argument != NULL
            && receiver_argument->kind == CM_HIR_TYPE_REFERENCE_KIND
            && receiver_argument->data.reference_type.mutability
                == CM_HIR_MUTABLE
            && receiver_self != NULL
            && receiver_self->kind == CM_HIR_TYPE_SELF_KIND
            && cm_hir_def_id_equal(receiver_self->data.self_type.owner,
                pending_impl_item->definition)
            && return_type != NULL && return_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(return_type->data.named_type.definition,
                poll_item->definition)
            && return_argument != NULL
            && return_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
            && return_argument->data.parameter_type.parameter
                == pending_impl_item->generic_parameter_start,
            "Pending poll signature, receiver, or return type differs");
    }

    trait_return = trait_poll == NULL ? NULL
        : cm_hir_get_type(&hir,
            trait_poll->data.function_item.signature.return_type);
    trait_return_argument = hir_named_type_argument(&hir, trait_return, 0u);
    impl_return = impl_poll == NULL ? NULL
        : cm_hir_get_type(&hir,
            impl_poll->data.function_item.signature.return_type);
    impl_return_argument = hir_named_type_argument(&hir, impl_return, 0u);
    check(trait_return != NULL && poll_item != NULL
        && trait_return->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(trait_return->data.named_type.definition,
            poll_item->definition)
        && trait_return_argument != NULL
        && trait_return_argument->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(trait_return_argument->data.projection_type
                .trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(trait_return_argument->data.projection_type
                .associated_type.definition,
            trait_output->definition)
        && impl_return != NULL
        && impl_return->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_return->data.named_type.definition,
            poll_item->definition)
        && impl_return_argument != NULL
        && impl_return_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && impl_return_argument->data.parameter_type.parameter
            == impl_item->generic_parameter_start,
        "Future Poll<Self::Output> or Ready Poll<T> return type differs");
    check(impl_body != NULL && impl_body->state == CM_HIR_BODY_UNLOWERED
        && impl_body->source == ready_information.source
        && impl_body->source_expression_id != CM_AST_EXPR_NONE
        && impl_body->span.source == ready_information.source
        && cm_hir_def_id_equal(impl_body->owner, impl_poll->definition)
        && impl_body->parameter_count == 2u && impl_body->local_count == 2u
        && impl_body->locals != NULL
        && impl_body->locals[0].parameter_index == 0u
        && hir_name_is(&hir, impl_body->locals[0].name, "self")
        && impl_body->locals[0].mutability == CM_HIR_MUTABLE
        && impl_body->locals[0].type
            == impl_poll->data.function_item.signature.parameters[0].type
        && hir_name_is(&hir, impl_body->locals[1].name, "_cx")
        && impl_body->locals[1].parameter_index == 1u
        && impl_body->locals[1].mutability == CM_HIR_IMMUTABLE
        && impl_body->locals[1].type
            == impl_poll->data.function_item.signature.parameters[1].type,
        "Ready::poll mutable receiver local or body provenance differs");
    check(trait_poll != NULL && impl_poll != NULL
        && trait_poll->data.function_item.signature.parameters[0]
            .binding_kind == CM_HIR_BINDING_NAMED
        && trait_poll->data.function_item.signature.parameters[1]
            .binding_kind == CM_HIR_BINDING_NAMED
        && hir_name_is(&hir,
            trait_poll->data.function_item.signature.parameters[1].name,
            "cx")
        && impl_poll->data.function_item.signature.parameters[0]
            .binding_kind == CM_HIR_BINDING_NAMED
        && impl_poll->data.function_item.signature.parameters[1]
            .binding_kind == CM_HIR_BINDING_NAMED
        && hir_name_is(&hir,
            impl_poll->data.function_item.signature.parameters[1].name,
            "_cx"),
        "Future cx or Ready _cx was misclassified as discard");
    check(pending_body != NULL
        && pending_body->state == CM_HIR_BODY_UNLOWERED
        && pending_body->source == pending_information.source
        && pending_body->source_expression_id != CM_AST_EXPR_NONE
        && pending_body->span.source == pending_information.source
        && cm_hir_def_id_equal(pending_body->owner,
            pending_poll->definition)
        && pending_body->parameter_count == 2u
        && pending_body->local_count == 1u
        && pending_body->locals != NULL
        && pending_body->locals[0].parameter_index == 0u
        && hir_name_is(&hir, pending_body->locals[0].name, "self")
        && pending_body->locals[0].mutability == CM_HIR_IMMUTABLE
        && pending_body->locals[0].type
            == pending_poll->data.function_item.signature.parameters[0].type,
        "Pending::poll discard parameter created a local or lost provenance");
    hir_dump = dump_hir(&hir);
    check(hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(hir_dump, "binding=discard name=none") != NULL
        && strstr(hir_dump, "binding=named name=\"_cx\"") != NULL
        && strstr(hir_dump, "binding=named name=\"cx\"") != NULL
        && strstr(hir_dump, "locals=1 params=2") != NULL
        && strstr(hir_dump,
            "origin=parameter[0] name=\"self\" mutability=immutable")
            != NULL,
        "Future/Ready/Pending hir-v27 dump lost binding distinctions");
    free(hir_dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_try_family_fixture(void)
{
    static const char fixture[] = "tests/hir/fixtures/try-family/lib.rs";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmModuleId convert_graph;
    CmModuleId control_graph;
    CmModuleId ops_graph;
    CmModuleId never_graph;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo convert_information;
    CmResolveModuleInfo control_information;
    CmResolveModuleInfo ops_information;
    CmResolveModuleInfo never_information;
    CmResolveEffectiveItem try_view;
    CmResolveEffectiveItem control_impl_view;
    CmResolveEffectiveItem never_impl_view;
    CmResolveEffectiveItem try_output_view;
    CmResolveEffectiveItem try_residual_view;
    CmResolveEffectiveItem try_from_output_view;
    CmResolveEffectiveItem try_branch_view;
    CmResolveEffectiveItem control_output_view;
    CmResolveEffectiveItem control_residual_view;
    CmResolveEffectiveItem control_from_output_view;
    CmResolveEffectiveItem control_branch_view;
    CmResolveEffectiveItem never_output_view;
    CmResolveEffectiveItem never_residual_view;
    CmResolveEffectiveItem never_from_output_view;
    CmResolveEffectiveItem never_branch_view;
    CmResolveEffectiveAttribute try_attributes[6];
    CmResolveEffectiveAttribute try_output_attribute;
    CmResolveEffectiveAttribute try_residual_attribute;
    CmResolveEffectiveAttribute try_from_output_attributes[2];
    CmResolveEffectiveAttribute try_branch_attributes[2];
    CmResolveEffectiveAttribute control_impl_attribute;
    CmResolveEffectiveAttribute control_from_output_inline;
    CmResolveEffectiveAttribute control_branch_inline;
    CmResolveEffectiveAttribute never_from_output_inline;
    CmResolveEffectiveAttribute never_branch_inline;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *try_item;
    const CmHirItem *from_residual_item;
    const CmHirItem *control_flow_item;
    const CmHirItem *infallible_item;
    const CmHirItem *never_item;
    const CmHirItem *never_residual_item;
    const CmHirItem *control_impl;
    const CmHirItem *never_impl;
    const CmHirItem *try_output;
    const CmHirItem *try_residual;
    const CmHirItem *try_from_output;
    const CmHirItem *try_branch;
    const CmHirItem *control_output;
    const CmHirItem *control_residual;
    const CmHirItem *control_from_output;
    const CmHirItem *control_branch;
    const CmHirItem *never_output;
    const CmHirItem *never_residual;
    const CmHirItem *never_from_output;
    const CmHirItem *never_branch;
    const CmHirGenericParam *control_b;
    const CmHirGenericParam *control_c;
    const CmHirGenericParam *never_t;
    const CmHirType *type;
    const CmHirType *argument;
    const CmHirType *argument_two;
    const CmHirBody *body;
    char *hir_dump;
    uint32_t index;
    int attributes_ok;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        CmResolveError error;
        char detail[256];

        memset(&error, 0, sizeof(error));
        detail[0] = '\0';
        if (cm_module_graph_get_error(&graph, 0u, &error)) {
            (void)cm_module_graph_copy_string(&graph, error.detail_a,
                detail, sizeof(detail));
            fprintf(stderr, "hir-graph-lower Try-family graph: %s: %s\n",
                cm_resolve_error_kind_name(error.kind), detail);
        }
        check(0, "could not build Rust 1.90 Try-family fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    convert_graph = CM_MODULE_NONE;
    control_graph = CM_MODULE_NONE;
    ops_graph = CM_MODULE_NONE;
    never_graph = CM_MODULE_NONE;
    memset(&root_information, 0, sizeof(root_information));
    memset(&convert_information, 0, sizeof(convert_information));
    memset(&control_information, 0, sizeof(control_information));
    memset(&ops_information, 0, sizeof(ops_information));
    memset(&never_information, 0, sizeof(never_information));
    memset(&try_view, 0, sizeof(try_view));
    memset(&control_impl_view, 0, sizeof(control_impl_view));
    memset(&never_impl_view, 0, sizeof(never_impl_view));
    memset(&try_output_view, 0, sizeof(try_output_view));
    memset(&try_residual_view, 0, sizeof(try_residual_view));
    memset(&try_from_output_view, 0, sizeof(try_from_output_view));
    memset(&try_branch_view, 0, sizeof(try_branch_view));
    memset(&control_output_view, 0, sizeof(control_output_view));
    memset(&control_residual_view, 0, sizeof(control_residual_view));
    memset(&control_from_output_view, 0,
        sizeof(control_from_output_view));
    memset(&control_branch_view, 0, sizeof(control_branch_view));
    memset(&never_output_view, 0, sizeof(never_output_view));
    memset(&never_residual_view, 0, sizeof(never_residual_view));
    memset(&never_from_output_view, 0, sizeof(never_from_output_view));
    memset(&never_branch_view, 0, sizeof(never_branch_view));
    memset(try_attributes, 0, sizeof(try_attributes));
    memset(&try_output_attribute, 0, sizeof(try_output_attribute));
    memset(&try_residual_attribute, 0, sizeof(try_residual_attribute));
    memset(try_from_output_attributes, 0,
        sizeof(try_from_output_attributes));
    memset(try_branch_attributes, 0, sizeof(try_branch_attributes));
    memset(&control_impl_attribute, 0, sizeof(control_impl_attribute));
    memset(&control_from_output_inline, 0,
        sizeof(control_from_output_inline));
    memset(&control_branch_inline, 0, sizeof(control_branch_inline));
    memset(&never_from_output_inline, 0,
        sizeof(never_from_output_inline));
    memset(&never_branch_inline, 0, sizeof(never_branch_inline));
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "convert", &convert_graph)
        && graph_module_named(&graph, "control_flow", &control_graph)
        && graph_module_named(&graph, "ops", &ops_graph)
        && graph_module_named(&graph, "never_short_circuit", &never_graph)
        && cm_module_graph_get_module(&graph, root_graph, &root_information)
        && cm_module_graph_get_module(&graph, convert_graph,
            &convert_information)
        && cm_module_graph_get_module(&graph, control_graph,
            &control_information)
        && cm_module_graph_get_module(&graph, ops_graph, &ops_information)
        && cm_module_graph_get_module(&graph, never_graph,
            &never_information)
        && root_information.child_count == 4u
        && root_information.import_count == 0u
        && convert_information.import_count == 0u
        && control_information.import_count == 2u
        && ops_information.import_count == 1u
        && never_information.import_count == 2u
        && convert_information.source != control_information.source
        && control_information.source != ops_information.source
        && ops_information.source != never_information.source,
        "Try-family module topology or source identity differs");
    (void)effective_item_named(&graph, revision, ops_graph, "Try",
        &try_view);
    for (index = 0u; index < control_information.effective_item_count;
         ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, revision,
                control_graph, index, &item) == CM_RESOLVE_VIEW_OK
            && item.item_kind == CM_AST_ITEM_IMPL) {
            control_impl_view = item;
            break;
        }
    }
    for (index = 0u; index < never_information.effective_item_count;
         ++index) {
        CmResolveEffectiveItem item;

        memset(&item, 0, sizeof(item));
        if (cm_module_graph_get_effective_item(&graph, revision,
                never_graph, index, &item) == CM_RESOLVE_VIEW_OK
            && item.item_kind == CM_AST_ITEM_IMPL) {
            never_impl_view = item;
            break;
        }
    }
    check(try_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && try_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && try_view.child_count == 4u && try_view.attribute_count == 6u
        && control_impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && control_impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && control_impl_view.child_count == 4u
        && control_impl_view.attribute_count == 1u
        && never_impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && never_impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && never_impl_view.child_count == 4u
        && never_impl_view.attribute_count == 0u
        && effective_child_named(&graph, revision, ops_graph, &try_view,
            "Output", &try_output_view)
        && effective_child_named(&graph, revision, ops_graph, &try_view,
            "Residual", &try_residual_view)
        && effective_child_named(&graph, revision, ops_graph, &try_view,
            "from_output", &try_from_output_view)
        && effective_child_named(&graph, revision, ops_graph, &try_view,
            "branch", &try_branch_view)
        && effective_child_named(&graph, revision, control_graph,
            &control_impl_view, "Output", &control_output_view)
        && effective_child_named(&graph, revision, control_graph,
            &control_impl_view, "Residual", &control_residual_view)
        && effective_child_named(&graph, revision, control_graph,
            &control_impl_view, "from_output", &control_from_output_view)
        && effective_child_named(&graph, revision, control_graph,
            &control_impl_view, "branch", &control_branch_view)
        && effective_child_named(&graph, revision, never_graph,
            &never_impl_view, "Output", &never_output_view)
        && effective_child_named(&graph, revision, never_graph,
            &never_impl_view, "Residual", &never_residual_view)
        && effective_child_named(&graph, revision, never_graph,
            &never_impl_view, "from_output", &never_from_output_view)
        && effective_child_named(&graph, revision, never_graph,
            &never_impl_view, "branch", &never_branch_view),
        "Try-family effective roots or child counts differ");
    check(effective_item_has_source(&try_view, ops_information.source)
        && effective_item_has_source(&try_output_view,
            ops_information.source)
        && effective_item_has_source(&try_residual_view,
            ops_information.source)
        && effective_item_has_source(&try_from_output_view,
            ops_information.source)
        && effective_item_has_source(&try_branch_view,
            ops_information.source)
        && effective_item_has_source(&control_impl_view,
            control_information.source)
        && effective_item_has_source(&control_output_view,
            control_information.source)
        && effective_item_has_source(&control_residual_view,
            control_information.source)
        && effective_item_has_source(&control_from_output_view,
            control_information.source)
        && effective_item_has_source(&control_branch_view,
            control_information.source)
        && effective_item_has_source(&never_impl_view,
            never_information.source)
        && effective_item_has_source(&never_output_view,
            never_information.source)
        && effective_item_has_source(&never_residual_view,
            never_information.source)
        && effective_item_has_source(&never_from_output_view,
            never_information.source)
        && effective_item_has_source(&never_branch_view,
            never_information.source),
        "Try-family effective declarations lost source provenance");

    attributes_ok = 1;
    for (index = 0u; index < 6u; ++index) {
        attributes_ok = attributes_ok
            && cm_module_graph_get_effective_item_attribute(&graph, revision,
                ops_graph, try_view.id, index, &try_attributes[index])
                == CM_RESOLVE_VIEW_OK
            && try_attributes[index].owner.source
                == try_view.declaration.source
            && try_attributes[index].owner.item == try_view.declaration.item
            && try_attributes[index].span.source == ops_information.source;
    }
    attributes_ok = attributes_ok
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_output_view.id, 0u, &try_output_attribute)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_residual_view.id, 0u, &try_residual_attribute)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_from_output_view.id, 0u,
            &try_from_output_attributes[0]) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_from_output_view.id, 1u,
            &try_from_output_attributes[1]) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_branch_view.id, 0u,
            &try_branch_attributes[0]) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            ops_graph, try_branch_view.id, 1u,
            &try_branch_attributes[1]) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            control_graph, control_impl_view.id, 0u,
            &control_impl_attribute) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            control_graph, control_from_output_view.id, 0u,
            &control_from_output_inline) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            control_graph, control_branch_view.id, 0u,
            &control_branch_inline) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            never_graph, never_from_output_view.id, 0u,
            &never_from_output_inline) == CM_RESOLVE_VIEW_OK
        && cm_module_graph_get_effective_item_attribute(&graph, revision,
            never_graph, never_branch_view.id, 0u,
            &never_branch_inline) == CM_RESOLVE_VIEW_OK;
    check(attributes_ok && try_output_view.attribute_count == 1u
        && try_residual_view.attribute_count == 1u
        && try_from_output_view.attribute_count == 2u
        && try_branch_view.attribute_count == 2u
        && control_output_view.attribute_count == 0u
        && control_residual_view.attribute_count == 0u
        && control_from_output_view.attribute_count == 1u
        && control_branch_view.attribute_count == 1u
        && never_output_view.attribute_count == 0u
        && never_residual_view.attribute_count == 0u
        && never_from_output_view.attribute_count == 1u
        && never_branch_view.attribute_count == 1u
        && graph_string_is(&graph, try_attributes[0].metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, try_attributes[1].metadata,
            "rustc_on_unimplemented(\n"
            "    on(\n"
            "        all(from_desugaring = \"TryBlock\"),\n"
            "        message = \"a `try` block must return `Result` or "
            "`Option` (or another type that implements `{This}`)\",\n"
            "        label = \"could not wrap the final value of the block "
            "as `{Self}` doesn't implement `Try`\",\n"
            "    ),\n"
            "    on(\n"
            "        all(from_desugaring = \"QuestionMark\"),\n"
            "        message = \"the `?` operator can only be applied to "
            "values that implement `{This}`\",\n"
            "        label = \"the `?` operator cannot be applied to type "
            "`{Self}`\"\n"
            "    )\n"
            ")")
        && graph_string_is(&graph, try_attributes[2].metadata,
            "doc(alias = \"?\")")
        && graph_string_is(&graph, try_attributes[3].metadata,
            "lang = \"Try\"")
        && graph_string_is(&graph, try_attributes[4].metadata, "const_trait")
        && graph_string_is(&graph, try_attributes[5].metadata,
            "rustc_const_unstable(feature = \"const_try\", issue = "
            "\"74935\")")
        && graph_string_is(&graph, try_output_attribute.metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, try_residual_attribute.metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, try_from_output_attributes[0].metadata,
            "lang = \"from_output\"")
        && graph_string_is(&graph, try_from_output_attributes[1].metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, try_branch_attributes[0].metadata,
            "lang = \"branch\"")
        && graph_string_is(&graph, try_branch_attributes[1].metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, control_impl_attribute.metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && graph_string_is(&graph, control_from_output_inline.metadata,
            "inline")
        && graph_string_is(&graph, control_branch_inline.metadata, "inline")
        && graph_string_is(&graph, never_from_output_inline.metadata,
            "inline")
        && graph_string_is(&graph, never_branch_inline.metadata, "inline"),
        "Try-family effective attributes differ from the fixture");
    check(try_output_attribute.owner.source == ops_information.source
        && try_output_attribute.owner.item
            == try_output_view.declaration.item
        && try_output_attribute.span.source == ops_information.source
        && try_residual_attribute.owner.source == ops_information.source
        && try_residual_attribute.owner.item
            == try_residual_view.declaration.item
        && try_residual_attribute.span.source == ops_information.source
        && try_from_output_attributes[0].owner.item
            == try_from_output_view.declaration.item
        && try_from_output_attributes[1].owner.item
            == try_from_output_view.declaration.item
        && try_branch_attributes[0].owner.item
            == try_branch_view.declaration.item
        && try_branch_attributes[1].owner.item
            == try_branch_view.declaration.item
        && try_from_output_attributes[0].span.source
            == ops_information.source
        && try_from_output_attributes[1].span.source
            == ops_information.source
        && try_branch_attributes[0].span.source == ops_information.source
        && try_branch_attributes[1].span.source == ops_information.source
        && control_impl_attribute.owner.item
            == control_impl_view.declaration.item
        && control_impl_attribute.span.source == control_information.source
        && control_from_output_inline.owner.item
            == control_from_output_view.declaration.item
        && control_branch_inline.owner.item
            == control_branch_view.declaration.item
        && control_from_output_inline.span.source
            == control_information.source
        && control_branch_inline.span.source == control_information.source
        && never_from_output_inline.owner.item
            == never_from_output_view.declaration.item
        && never_branch_inline.owner.item
            == never_branch_view.declaration.item
        && never_from_output_inline.span.source == never_information.source
        && never_branch_inline.span.source == never_information.source,
        "Try-family effective attribute provenance differs");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "try_family";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower Try-family: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    try_item = find_hir_item_anywhere(&hir, "Try");
    from_residual_item = find_hir_item_anywhere(&hir, "FromResidual");
    control_flow_item = find_hir_item_anywhere(&hir, "ControlFlow");
    infallible_item = find_hir_item_anywhere(&hir, "Infallible");
    never_item = find_hir_item_anywhere(&hir, "NeverShortCircuit");
    never_residual_item = find_hir_item_anywhere(&hir,
        "NeverShortCircuitResidual");
    control_impl = control_flow_item == NULL ? NULL
        : find_hir_impl_for(&hir, control_flow_item->definition);
    never_impl = never_item == NULL ? NULL
        : find_hir_impl_for(&hir, never_item->definition);
    try_output = try_item == NULL ? NULL
        : find_hir_associated_item(&hir, try_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    try_residual = try_item == NULL ? NULL
        : find_hir_associated_item(&hir, try_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Residual");
    try_from_output = try_item == NULL ? NULL
        : find_hir_associated_item(&hir, try_item->definition,
            CM_HIR_ITEM_FUNCTION, "from_output");
    try_branch = try_item == NULL ? NULL
        : find_hir_associated_item(&hir, try_item->definition,
            CM_HIR_ITEM_FUNCTION, "branch");
    control_output = control_impl == NULL ? NULL
        : find_hir_associated_item(&hir, control_impl->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    control_residual = control_impl == NULL ? NULL
        : find_hir_associated_item(&hir, control_impl->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Residual");
    control_from_output = control_impl == NULL ? NULL
        : find_hir_associated_item(&hir, control_impl->definition,
            CM_HIR_ITEM_FUNCTION, "from_output");
    control_branch = control_impl == NULL ? NULL
        : find_hir_associated_item(&hir, control_impl->definition,
            CM_HIR_ITEM_FUNCTION, "branch");
    never_output = never_impl == NULL ? NULL
        : find_hir_associated_item(&hir, never_impl->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    never_residual = never_impl == NULL ? NULL
        : find_hir_associated_item(&hir, never_impl->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Residual");
    never_from_output = never_impl == NULL ? NULL
        : find_hir_associated_item(&hir, never_impl->definition,
            CM_HIR_ITEM_FUNCTION, "from_output");
    never_branch = never_impl == NULL ? NULL
        : find_hir_associated_item(&hir, never_impl->definition,
            CM_HIR_ITEM_FUNCTION, "branch");
    check(result.error_count == 0u && result.lowered_item_count == 24u
        && cm_hir_module_map_count(&map) == 5u
        && try_item != NULL && from_residual_item != NULL
        && control_flow_item != NULL && infallible_item != NULL
        && never_item != NULL && never_residual_item != NULL
        && control_impl != NULL && never_impl != NULL
        && try_output != NULL && try_residual != NULL
        && try_from_output != NULL && try_branch != NULL
        && control_output != NULL && control_residual != NULL
        && control_from_output != NULL && control_branch != NULL
        && never_output != NULL && never_residual != NULL
        && never_from_output != NULL && never_branch != NULL,
        "Rust 1.90 Try-family fixture did not lower completely");
    check(try_item != NULL && from_residual_item != NULL
        && try_item->kind == CM_HIR_ITEM_TRAIT
        && try_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && try_item->generic_parameter_count == 0u
        && try_item->attribute_count == 6u
        && try_item->data.trait_item.supertrait_count == 1u
        && try_item->data.trait_item.supertraits != NULL
        && try_item->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(try_item->data.trait_item.supertraits[0]
                .trait_type.definition,
            from_residual_item->definition)
        && try_item->data.trait_item.supertraits[0]
            .trait_type.argument_count == 0u
        && try_item->data.trait_item.supertraits[0]
            .trait_type.arguments == NULL
        && try_item->data.trait_item.supertraits[0].span.source
            == ops_information.source
        && from_residual_item->kind == CM_HIR_ITEM_TRAIT
        && from_residual_item->generic_parameter_count == 0u
        && from_residual_item->data.trait_item.supertrait_count == 0u,
        "Try's const-if-const forward supertrait edge is not exact");
    check(try_item != NULL
        && hir_name_is(&hir, try_item->attributes[0].metadata,
            "unstable(feature = \"try_trait_v2\", issue = \"84277\", "
            "old_name = \"try_trait\")")
        && hir_name_is(&hir, try_item->attributes[2].metadata,
            "doc(alias = \"?\")")
        && hir_name_is(&hir, try_item->attributes[3].metadata,
            "lang = \"Try\"")
        && hir_name_is(&hir, try_item->attributes[4].metadata, "const_trait")
        && hir_name_is(&hir, try_item->attributes[5].metadata,
            "rustc_const_unstable(feature = \"const_try\", issue = "
            "\"74935\")")
        && try_item->span.source == ops_information.source,
        "Try HIR attributes or source provenance differ");
    attributes_ok = 1;
    for (index = 0u; index < 6u; ++index) {
        attributes_ok = attributes_ok
            && try_item->attributes[index].source_attribute
                == try_attributes[index].source_attribute
            && graph_hir_string_equal(&graph,
                try_attributes[index].metadata, &hir,
                try_item->attributes[index].metadata);
    }
    check(attributes_ok,
        "Try HIR attributes do not exactly match the effective graph");
    check(try_output != NULL && try_residual != NULL
        && try_from_output != NULL && try_branch != NULL
        && try_output->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && try_residual->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && try_output->attribute_count == 1u
        && try_residual->attribute_count == 1u
        && try_from_output->attribute_count == 2u
        && try_branch->attribute_count == 2u
        && try_output->attributes[0].source_attribute
            == try_output_attribute.source_attribute
        && try_residual->attributes[0].source_attribute
            == try_residual_attribute.source_attribute
        && try_from_output->attributes[0].source_attribute
            == try_from_output_attributes[0].source_attribute
        && try_from_output->attributes[1].source_attribute
            == try_from_output_attributes[1].source_attribute
        && try_branch->attributes[0].source_attribute
            == try_branch_attributes[0].source_attribute
        && try_branch->attributes[1].source_attribute
            == try_branch_attributes[1].source_attribute
        && try_output->span.source == ops_information.source
        && try_residual->span.source == ops_information.source
        && try_from_output->span.source == ops_information.source
        && try_branch->span.source == ops_information.source,
        "Try associated declarations lost attributes or provenance");

    check(try_from_output != NULL && try_branch != NULL
        && hir_function_is_ordinary(&hir, try_from_output)
        && hir_function_is_ordinary(&hir, try_branch)
        && try_from_output->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_NONE
        && try_from_output->data.function_item.signature.parameter_count == 1u
        && try_from_output->data.function_item.signature.parameters != NULL
        && hir_name_is(&hir, try_from_output->data.function_item.signature
                .parameters[0].name,
            "output")
        && hir_type_is_projection(&hir, try_from_output->data.function_item
                .signature.parameters[0].type,
            try_item->definition, try_item->definition,
            try_output->definition)
        && hir_type_is_self(&hir, try_from_output->data.function_item
                .signature.return_type,
            try_item->definition)
        && try_from_output->data.function_item.body == CM_HIR_BODY_NONE
        && try_branch->data.function_item.body == CM_HIR_BODY_NONE
        && try_branch->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_VALUE
        && try_branch->data.function_item.signature.parameter_count == 1u
        && try_branch->data.function_item.signature.parameters != NULL
        && hir_type_is_self(&hir, try_branch->data.function_item.signature
                .parameters[0].type,
            try_item->definition),
        "Try method parameters, receiver, or Self return differ");
    type = try_branch == NULL ? NULL : cm_hir_get_type(&hir,
        try_branch->data.function_item.signature.return_type);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            control_flow_item->definition)
        && type->data.named_type.argument_count == 2u
        && type->data.named_type.arguments != NULL
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && type->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && hir_type_is_projection(&hir,
            type->data.named_type.arguments[0].data.type,
            try_item->definition, try_item->definition,
            try_residual->definition)
        && hir_type_is_projection(&hir,
            type->data.named_type.arguments[1].data.type,
            try_item->definition, try_item->definition,
            try_output->definition),
        "Try::branch return projections differ");

    control_b = control_impl == NULL ? NULL : cm_hir_get_generic_param(&hir,
        control_impl->generic_parameter_start);
    control_c = control_impl == NULL ? NULL : cm_hir_get_generic_param(&hir,
        control_impl->generic_parameter_start + 1u);
    check(control_impl != NULL && try_item != NULL
        && control_impl->generic_parameter_count == 2u
        && control_b != NULL && control_c != NULL
        && control_b->kind == CM_HIR_GENERIC_TYPE
        && control_c->kind == CM_HIR_GENERIC_TYPE
        && hir_name_is(&hir, control_b->name, "B")
        && hir_name_is(&hir, control_c->name, "C")
        && control_b->index == 0u && control_c->index == 1u
        && cm_hir_def_id_equal(control_b->owner, control_impl->definition)
        && cm_hir_def_id_equal(control_c->owner, control_impl->definition)
        && control_impl->data.impl_item.has_trait
        && control_impl->data.impl_item.trait_type.argument_count == 0u
        && cm_hir_def_id_equal(control_impl->data.impl_item
                .trait_type.definition,
            try_item->definition)
        && control_impl->attribute_count == 1u
        && control_impl->attributes[0].source_attribute
            == control_impl_attribute.source_attribute,
        "ControlFlow Try impl generic or trait header differs");
    type = control_impl == NULL ? NULL : cm_hir_get_type(&hir,
        control_impl->data.impl_item.self_type);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            control_flow_item->definition)
        && type->data.named_type.argument_count == 2u
        && type->data.named_type.arguments != NULL
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && type->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && hir_type_is_parameter(&hir,
            type->data.named_type.arguments[0].data.type,
            control_impl->generic_parameter_start)
        && hir_type_is_parameter(&hir,
            type->data.named_type.arguments[1].data.type,
            control_impl->generic_parameter_start + 1u),
        "ControlFlow<B, C> impl self type is not exact");
    check(control_output != NULL && control_residual != NULL
        && control_from_output != NULL && control_branch != NULL
        && cm_hir_def_id_equal(control_output->data.type_alias_item
                .trait_item_definition,
            try_output->definition)
        && cm_hir_def_id_equal(control_residual->data.type_alias_item
                .trait_item_definition,
            try_residual->definition)
        && cm_hir_def_id_equal(control_from_output->data.function_item
                .trait_item_definition,
            try_from_output->definition)
        && cm_hir_def_id_equal(control_branch->data.function_item
                .trait_item_definition,
            try_branch->definition)
        && hir_type_is_parameter(&hir,
            control_output->data.type_alias_item.target,
            control_impl->generic_parameter_start + 1u),
        "ControlFlow associated items are not linked to Try exactly");
    type = control_residual == NULL ? NULL : cm_hir_get_type(&hir,
        control_residual->data.type_alias_item.target);
    argument = hir_named_type_argument(&hir, type, 0u);
    argument_two = hir_named_type_argument(&hir, type, 1u);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            control_flow_item->definition)
        && type->data.named_type.argument_count == 2u
        && argument != NULL
        && argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && argument->data.parameter_type.parameter
            == control_impl->generic_parameter_start
        && argument_two != NULL
        && argument_two->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(argument_two->data.named_type.definition,
            infallible_item->definition),
        "ControlFlow residual target is not ControlFlow<B, Infallible>");
    check(control_from_output != NULL && control_branch != NULL
        && hir_function_is_ordinary(&hir, control_from_output)
        && hir_function_is_ordinary(&hir, control_branch)
        && control_from_output->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_NONE
        && control_from_output->data.function_item.signature.parameter_count
            == 1u
        && hir_name_is(&hir, control_from_output->data.function_item.signature
                .parameters[0].name,
            "output")
        && hir_type_is_projection(&hir, control_from_output->data.function_item
                .signature.parameters[0].type,
            control_impl->definition, try_item->definition,
            try_output->definition)
        && hir_type_is_self(&hir, control_from_output->data.function_item
                .signature.return_type,
            control_impl->definition)
        && control_branch->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_VALUE
        && control_branch->data.function_item.signature.parameter_count == 1u
        && hir_type_is_self(&hir, control_branch->data.function_item.signature
                .parameters[0].type,
            control_impl->definition),
        "ControlFlow Try method signatures differ");
    type = control_branch == NULL ? NULL : cm_hir_get_type(&hir,
        control_branch->data.function_item.signature.return_type);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            control_flow_item->definition)
        && type->data.named_type.argument_count == 2u
        && type->data.named_type.arguments != NULL
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && type->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && hir_type_is_projection(&hir,
            type->data.named_type.arguments[0].data.type,
            control_impl->definition, try_item->definition,
            try_residual->definition)
        && hir_type_is_projection(&hir,
            type->data.named_type.arguments[1].data.type,
            control_impl->definition, try_item->definition,
            try_output->definition),
        "ControlFlow::branch return projections differ");

    never_t = never_impl == NULL ? NULL : cm_hir_get_generic_param(&hir,
        never_impl->generic_parameter_start);
    type = never_impl == NULL ? NULL : cm_hir_get_type(&hir,
        never_impl->data.impl_item.self_type);
    argument = hir_named_type_argument(&hir, type, 0u);
    check(never_impl != NULL && never_t != NULL && try_item != NULL
        && never_impl->generic_parameter_count == 1u
        && never_t->kind == CM_HIR_GENERIC_TYPE
        && never_t->index == 0u && hir_name_is(&hir, never_t->name, "T")
        && cm_hir_def_id_equal(never_t->owner, never_impl->definition)
        && never_impl->data.impl_item.has_trait
        && never_impl->data.impl_item.trait_type.argument_count == 0u
        && cm_hir_def_id_equal(never_impl->data.impl_item
                .trait_type.definition,
            try_item->definition)
        && never_impl->attribute_count == 0u
        && type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            never_item->definition)
        && argument != NULL
        && argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && argument->data.parameter_type.parameter
            == never_impl->generic_parameter_start,
        "NeverShortCircuit<T> Try impl header differs");
    check(never_output != NULL && never_residual != NULL
        && never_from_output != NULL && never_branch != NULL
        && cm_hir_def_id_equal(never_output->data.type_alias_item
                .trait_item_definition,
            try_output->definition)
        && cm_hir_def_id_equal(never_residual->data.type_alias_item
                .trait_item_definition,
            try_residual->definition)
        && cm_hir_def_id_equal(never_from_output->data.function_item
                .trait_item_definition,
            try_from_output->definition)
        && cm_hir_def_id_equal(never_branch->data.function_item
                .trait_item_definition,
            try_branch->definition)
        && hir_type_is_parameter(&hir,
            never_output->data.type_alias_item.target,
            never_impl->generic_parameter_start),
        "NeverShortCircuit associated items are not linked to Try exactly");
    type = never_residual == NULL ? NULL : cm_hir_get_type(&hir,
        never_residual->data.type_alias_item.target);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            never_residual_item->definition),
        "NeverShortCircuit residual target differs");
    check(never_from_output != NULL && never_branch != NULL
        && hir_function_is_ordinary(&hir, never_from_output)
        && hir_function_is_ordinary(&hir, never_branch)
        && never_from_output->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_NONE
        && never_from_output->data.function_item.signature.parameter_count
            == 1u
        && hir_name_is(&hir, never_from_output->data.function_item.signature
                .parameters[0].name,
            "x")
        && hir_type_is_parameter(&hir, never_from_output->data.function_item
                .signature.parameters[0].type,
            never_impl->generic_parameter_start)
        && hir_type_is_self(&hir, never_from_output->data.function_item
                .signature.return_type,
            never_impl->definition)
        && never_branch->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_VALUE
        && never_branch->data.function_item.signature.parameter_count == 1u
        && hir_type_is_self(&hir, never_branch->data.function_item.signature
                .parameters[0].type,
            never_impl->definition),
        "NeverShortCircuit Try method signatures differ");
    type = never_branch == NULL ? NULL : cm_hir_get_type(&hir,
        never_branch->data.function_item.signature.return_type);
    argument = hir_named_type_argument(&hir, type, 0u);
    argument_two = hir_named_type_argument(&hir, type, 1u);
    check(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            control_flow_item->definition)
        && type->data.named_type.argument_count == 2u
        && type->data.named_type.arguments != NULL
        && argument != NULL && argument->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(argument->data.named_type.definition,
            never_residual_item->definition)
        && argument_two != NULL
        && argument_two->kind == CM_HIR_TYPE_PARAMETER_KIND
        && argument_two->data.parameter_type.parameter
            == never_impl->generic_parameter_start,
        "NeverShortCircuit::branch return type differs");

    check(control_from_output != NULL && control_branch != NULL
        && never_from_output != NULL && never_branch != NULL
        && !cm_hir_def_id_equal(control_from_output->definition,
            control_branch->definition)
        && !cm_hir_def_id_equal(control_from_output->definition,
            never_from_output->definition)
        && !cm_hir_def_id_equal(control_from_output->definition,
            never_branch->definition)
        && !cm_hir_def_id_equal(control_branch->definition,
            never_from_output->definition)
        && !cm_hir_def_id_equal(control_branch->definition,
            never_branch->definition)
        && !cm_hir_def_id_equal(never_from_output->definition,
            never_branch->definition),
        "Try impl methods did not receive four distinct DefIds");
    body = control_from_output == NULL ? NULL : cm_hir_get_body(&hir,
        control_from_output->data.function_item.body);
    check(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == control_information.source
        && body->span.source == control_information.source
        && body->source_expression_id != CM_AST_EXPR_NONE
        && cm_hir_def_id_equal(body->owner,
            control_from_output->definition)
        && control_from_output->attribute_count == 1u
        && control_from_output->attributes[0].source_attribute
            == control_from_output_inline.source_attribute,
        "ControlFlow::from_output body or inline provenance differs");
    body = control_branch == NULL ? NULL : cm_hir_get_body(&hir,
        control_branch->data.function_item.body);
    check(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == control_information.source
        && body->span.source == control_information.source
        && body->source_expression_id != CM_AST_EXPR_NONE
        && cm_hir_def_id_equal(body->owner, control_branch->definition)
        && control_branch->attribute_count == 1u
        && control_branch->attributes[0].source_attribute
            == control_branch_inline.source_attribute,
        "ControlFlow::branch body or inline provenance differs");
    body = never_from_output == NULL ? NULL : cm_hir_get_body(&hir,
        never_from_output->data.function_item.body);
    check(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == never_information.source
        && body->span.source == never_information.source
        && body->source_expression_id != CM_AST_EXPR_NONE
        && cm_hir_def_id_equal(body->owner, never_from_output->definition)
        && never_from_output->attribute_count == 1u
        && never_from_output->attributes[0].source_attribute
            == never_from_output_inline.source_attribute,
        "NeverShortCircuit::from_output body or inline provenance differs");
    body = never_branch == NULL ? NULL : cm_hir_get_body(&hir,
        never_branch->data.function_item.body);
    check(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == never_information.source
        && body->span.source == never_information.source
        && body->source_expression_id != CM_AST_EXPR_NONE
        && cm_hir_def_id_equal(body->owner, never_branch->definition)
        && never_branch->attribute_count == 1u
        && never_branch->attributes[0].source_attribute
            == never_branch_inline.source_attribute,
        "NeverShortCircuit::branch body or inline provenance differs");

    hir_dump = dump_hir(&hir);
    check(hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(hir_dump, "modifier=const-if-const trait=") != NULL
        && strstr(hir_dump, "name=\"FromResidual\"") != NULL
        && strstr(hir_dump, "name=\"from_output\"") != NULL
        && strstr(hir_dump, "name=\"branch\"") != NULL
        && strstr(hir_dump, "meta=\"inline\"") != NULL,
        "Try-family hir-v27 dump omitted its supertrait or methods");
    free(hir_dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_into_iterator_deref_fixture(void)
{
    static const char fixture[] =
        "tests/hir/fixtures/into-iterator-deref/lib.rs";
    static const char *const into_attributes_expected[] = {
        "rustc_diagnostic_item = \"IntoIterator\"",
        "rustc_on_unimplemented(\n"
        "    on(\n"
        "        Self = \"core::ops::range::RangeTo<Idx>\",\n"
        "        label = \"if you meant to iterate until a value, add a "
            "starting value\",\n"
        "        note = \"`..end` is a `RangeTo`, which cannot be iterated "
            "on; you might have meant to have a \\\n"
        "              bounded `Range`: `0..end`\"\n"
        "    ),\n"
        "    on(\n"
        "        Self = \"core::ops::range::RangeToInclusive<Idx>\",\n"
        "        label = \"if you meant to iterate until a value (including "
            "it), add a starting value\",\n"
        "        note = \"`..=end` is a `RangeToInclusive`, which cannot be "
            "iterated on; you might have meant \\\n"
        "              to have a bounded `RangeInclusive`: `0..=end`\"\n"
        "    ),\n"
        "    on(\n"
        "        Self = \"[]\",\n"
        "        label = \"`{Self}` is not an iterator; try calling "
            "`.into_iter()` or `.iter()`\"\n"
        "    ),\n"
        "    on(Self = \"&[]\", label = \"`{Self}` is not an iterator; try "
            "calling `.iter()`\"),\n"
        "    on(\n"
        "        Self = \"alloc::vec::Vec<T, A>\",\n"
        "        label = \"`{Self}` is not an iterator; try calling "
            "`.into_iter()` or `.iter()`\"\n"
        "    ),\n"
        "    on(Self = \"&str\", label = \"`{Self}` is not an iterator; try "
            "calling `.chars()` or `.bytes()`\"),\n"
        "    on(\n"
        "        Self = \"alloc::string::String\",\n"
        "        label = \"`{Self}` is not an iterator; try calling "
            "`.chars()` or `.bytes()`\"\n"
        "    ),\n"
        "    on(\n"
        "        Self = \"{integral}\",\n"
        "        note = \"if you want to iterate between `start` until a "
            "value `end`, use the exclusive range \\\n"
        "              syntax `start..end` or the inclusive range syntax "
            "`start..=end`\"\n"
        "    ),\n"
        "    on(\n"
        "        Self = \"{float}\",\n"
        "        note = \"if you want to iterate between `start` until a "
            "value `end`, use the exclusive range \\\n"
        "              syntax `start..end` or the inclusive range syntax "
            "`start..=end`\"\n"
        "    ),\n"
        "    label = \"`{Self}` is not an iterator\",\n"
        "    message = \"`{Self}` is not an iterator\"\n"
        ")",
        "rustc_skip_during_method_dispatch(array, boxed_slice)",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const stable_attribute[] = {
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const into_method_attributes_expected[] = {
        "lang = \"into_iter\"",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const deref_attributes_expected[] = {
        "lang = \"deref\"",
        "doc(alias = \"*\")",
        "doc(alias = \"&*\")",
        "stable(feature = \"rust1\", since = \"1.0.0\")",
        "rustc_diagnostic_item = \"Deref\"",
        "const_trait",
        "rustc_const_unstable(feature = \"const_deref\", issue = \"88955\")"
    };
    static const char *const deref_target_attributes_expected[] = {
        "stable(feature = \"rust1\", since = \"1.0.0\")",
        "rustc_diagnostic_item = \"deref_target\"",
        "lang = \"deref_target\""
    };
    static const char *const deref_method_attributes_expected[] = {
        "must_use",
        "stable(feature = \"rust1\", since = \"1.0.0\")",
        "rustc_diagnostic_item = \"deref_method\""
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmModuleId marker_graph;
    CmModuleId iter_graph;
    CmModuleId ops_graph;
    CmResolveModuleInfo root_information;
    CmResolveModuleInfo marker_information;
    CmResolveModuleInfo iter_information;
    CmResolveModuleInfo ops_information;
    CmResolveEffectiveItem into_view;
    CmResolveEffectiveItem into_item_view;
    CmResolveEffectiveItem into_iter_view;
    CmResolveEffectiveItem into_method_view;
    CmResolveEffectiveItem deref_view;
    CmResolveEffectiveItem deref_target_view;
    CmResolveEffectiveItem deref_method_view;
    CmResolveEffectiveAttribute into_attributes[4];
    CmResolveEffectiveAttribute into_item_attributes[1];
    CmResolveEffectiveAttribute into_iter_attributes[1];
    CmResolveEffectiveAttribute into_method_attributes[2];
    CmResolveEffectiveAttribute deref_attributes[7];
    CmResolveEffectiveAttribute deref_target_attributes[3];
    CmResolveEffectiveAttribute deref_method_attributes[3];
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *sized_item;
    const CmHirItem *pointee_sized_item;
    const CmHirItem *iterator_item;
    const CmHirItem *iterator_associated;
    const CmHirItem *into_item;
    const CmHirItem *into_associated_item;
    const CmHirItem *into_associated_iter;
    const CmHirItem *into_method;
    const CmHirItem *deref_item;
    const CmHirItem *deref_target;
    const CmHirItem *deref_method;
    const CmHirAssociatedTypeBound *into_bound;
    const CmHirAssociatedTypeBound *deref_bound;
    const CmHirAssociatedTypeEquality *into_equality;
    const CmHirType *type;
    const CmHirType *pointee;
    char *hir_dump;
    char *first_bound_dump;
    char *equality_dump;
    char *second_bound_dump;
    char *required_modifier_dump;
    char *relaxed_modifier_dump;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        check(0, "could not build Rust 1.90 IntoIterator/Deref fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    marker_graph = CM_MODULE_NONE;
    iter_graph = CM_MODULE_NONE;
    ops_graph = CM_MODULE_NONE;
    memset(&root_information, 0, sizeof(root_information));
    memset(&marker_information, 0, sizeof(marker_information));
    memset(&iter_information, 0, sizeof(iter_information));
    memset(&ops_information, 0, sizeof(ops_information));
    memset(&into_view, 0, sizeof(into_view));
    memset(&into_item_view, 0, sizeof(into_item_view));
    memset(&into_iter_view, 0, sizeof(into_iter_view));
    memset(&into_method_view, 0, sizeof(into_method_view));
    memset(&deref_view, 0, sizeof(deref_view));
    memset(&deref_target_view, 0, sizeof(deref_target_view));
    memset(&deref_method_view, 0, sizeof(deref_method_view));
    check(cm_module_graph_get_root(&graph, &root_graph)
        && graph_module_named(&graph, "marker", &marker_graph)
        && graph_module_named(&graph, "iter", &iter_graph)
        && graph_module_named(&graph, "ops", &ops_graph)
        && cm_module_graph_get_module(&graph, root_graph, &root_information)
        && cm_module_graph_get_module(&graph, marker_graph,
            &marker_information)
        && cm_module_graph_get_module(&graph, iter_graph, &iter_information)
        && cm_module_graph_get_module(&graph, ops_graph, &ops_information)
        && root_information.child_count == 3u
        && root_information.import_count == 0u
        && marker_information.import_count == 0u
        && iter_information.import_count == 0u
        && ops_information.import_count == 2u
        && marker_information.source != iter_information.source
        && iter_information.source != ops_information.source,
        "IntoIterator/Deref module topology or source identity differs");
    (void)effective_item_named(&graph, revision, iter_graph, "IntoIterator",
        &into_view);
    (void)effective_item_named(&graph, revision, ops_graph, "Deref",
        &deref_view);
    check(into_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && into_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && into_view.child_count == 3u
        && effective_child_named(&graph, revision, iter_graph, &into_view,
            "Item", &into_item_view)
        && effective_child_named(&graph, revision, iter_graph, &into_view,
            "IntoIter", &into_iter_view)
        && effective_child_named(&graph, revision, iter_graph, &into_view,
            "into_iter", &into_method_view)
        && deref_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && deref_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && deref_view.child_count == 2u
        && effective_child_named(&graph, revision, ops_graph, &deref_view,
            "Target", &deref_target_view)
        && effective_child_named(&graph, revision, ops_graph, &deref_view,
            "deref", &deref_method_view),
        "IntoIterator/Deref effective declarations differ");
    check(effective_item_has_source(&into_view, iter_information.source)
        && effective_item_has_source(&into_item_view,
            iter_information.source)
        && effective_item_has_source(&into_iter_view,
            iter_information.source)
        && effective_item_has_source(&into_method_view,
            iter_information.source)
        && effective_item_has_source(&deref_view, ops_information.source)
        && effective_item_has_source(&deref_target_view,
            ops_information.source)
        && effective_item_has_source(&deref_method_view,
            ops_information.source),
        "IntoIterator/Deref effective items lost source provenance");
    check(effective_attributes_are(&graph, revision, iter_graph, &into_view,
            into_attributes_expected, 4u, into_attributes)
        && effective_attributes_are(&graph, revision, iter_graph,
            &into_item_view, stable_attribute, 1u, into_item_attributes)
        && effective_attributes_are(&graph, revision, iter_graph,
            &into_iter_view, stable_attribute, 1u, into_iter_attributes)
        && effective_attributes_are(&graph, revision, iter_graph,
            &into_method_view, into_method_attributes_expected, 2u,
            into_method_attributes)
        && effective_attributes_are(&graph, revision, ops_graph, &deref_view,
            deref_attributes_expected, 7u, deref_attributes)
        && effective_attributes_are(&graph, revision, ops_graph,
            &deref_target_view, deref_target_attributes_expected, 3u,
            deref_target_attributes)
        && effective_attributes_are(&graph, revision, ops_graph,
            &deref_method_view, deref_method_attributes_expected, 3u,
            deref_method_attributes),
        "IntoIterator/Deref effective attributes or provenance differ");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "into_iterator_deref";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower IntoIterator/Deref: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    sized_item = find_hir_item_anywhere(&hir, "Sized");
    pointee_sized_item = find_hir_item_anywhere(&hir, "PointeeSized");
    iterator_item = find_hir_item_anywhere(&hir, "Iterator");
    into_item = find_hir_item_anywhere(&hir, "IntoIterator");
    deref_item = find_hir_item_anywhere(&hir, "Deref");
    iterator_associated = iterator_item == NULL ? NULL
        : find_hir_associated_item(&hir, iterator_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    into_associated_item = into_item == NULL ? NULL
        : find_hir_associated_item(&hir, into_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    into_associated_iter = into_item == NULL ? NULL
        : find_hir_associated_item(&hir, into_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "IntoIter");
    into_method = into_item == NULL ? NULL
        : find_hir_associated_item(&hir, into_item->definition,
            CM_HIR_ITEM_FUNCTION, "into_iter");
    deref_target = deref_item == NULL ? NULL
        : find_hir_associated_item(&hir, deref_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Target");
    deref_method = deref_item == NULL ? NULL
        : find_hir_associated_item(&hir, deref_item->definition,
            CM_HIR_ITEM_FUNCTION, "deref");
    check(result.error_count == 0u && result.lowered_item_count == 14u
        && cm_hir_module_map_count(&map) == 4u && sized_item != NULL
        && pointee_sized_item != NULL && iterator_item != NULL
        && iterator_associated != NULL && into_item != NULL
        && into_associated_item != NULL && into_associated_iter != NULL
        && into_method != NULL && deref_item != NULL && deref_target != NULL
        && deref_method != NULL,
        "Rust 1.90 IntoIterator/Deref fixture did not lower completely");
    check(hir_attributes_match_graph(&graph, &hir, into_item,
            into_attributes, 4u)
        && hir_attributes_match_graph(&graph, &hir, into_associated_item,
            into_item_attributes, 1u)
        && hir_attributes_match_graph(&graph, &hir, into_associated_iter,
            into_iter_attributes, 1u)
        && hir_attributes_match_graph(&graph, &hir, into_method,
            into_method_attributes, 2u)
        && hir_attributes_match_graph(&graph, &hir, deref_item,
            deref_attributes, 7u)
        && hir_attributes_match_graph(&graph, &hir, deref_target,
            deref_target_attributes, 3u)
        && hir_attributes_match_graph(&graph, &hir, deref_method,
            deref_method_attributes, 3u)
        && into_item->span.source == iter_information.source
        && into_associated_item->span.source == iter_information.source
        && into_associated_iter->span.source == iter_information.source
        && into_method->span.source == iter_information.source
        && deref_item->span.source == ops_information.source
        && deref_target->span.source == ops_information.source
        && deref_method->span.source == ops_information.source,
        "IntoIterator/Deref HIR attributes or provenance differ");
    check(deref_item != NULL && pointee_sized_item != NULL
        && deref_item->data.trait_item.supertrait_count == 1u
        && deref_item->data.trait_item.supertraits != NULL
        && deref_item->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_REQUIRED
        && cm_hir_def_id_equal(deref_item->data.trait_item.supertraits[0]
                .trait_type.definition,
            pointee_sized_item->definition)
        && deref_item->data.trait_item.supertraits[0]
            .trait_type.argument_count == 0u
        && deref_item->data.trait_item.supertraits[0].span.source
            == ops_information.source,
        "Deref did not retain its exact PointeeSized supertrait");
    into_bound = into_associated_iter != NULL
            && into_associated_iter->data.type_alias_item.bound_count == 1u
            && into_associated_iter->data.type_alias_item.bounds != NULL
        ? &into_associated_iter->data.type_alias_item.bounds[0] : NULL;
    into_equality = into_bound != NULL && into_bound->equality_count == 1u
            && into_bound->equalities != NULL
        ? &into_bound->equalities[0] : NULL;
    deref_bound = deref_target != NULL
            && deref_target->data.type_alias_item.bound_count == 1u
            && deref_target->data.type_alias_item.bounds != NULL
        ? &deref_target->data.type_alias_item.bounds[0] : NULL;
    check(iterator_associated != NULL && into_associated_item != NULL
        && into_associated_iter != NULL && iterator_item != NULL
        && iterator_associated->data.type_alias_item.target
            == CM_HIR_TYPE_NONE
        && iterator_associated->data.type_alias_item.bound_count == 0u
        && into_associated_item->data.type_alias_item.target
            == CM_HIR_TYPE_NONE
        && into_associated_item->data.type_alias_item.bound_count == 0u
        && into_associated_iter->data.type_alias_item.target
            == CM_HIR_TYPE_NONE
        && into_bound != NULL
        && into_bound->modifier == CM_HIR_ASSOC_BOUND_REQUIRED
        && cm_hir_def_id_equal(into_bound->trait_type.definition,
            iterator_item->definition)
        && into_bound->trait_type.argument_count == 0u
        && into_bound->trait_type.arguments == NULL
        && into_bound->span.source == iter_information.source
        && into_bound->span.start < into_bound->span.end
        && into_equality != NULL
        && cm_hir_def_id_equal(into_equality->associated_type,
            iterator_associated->definition)
        && into_equality->span.source == iter_information.source
        && into_equality->span.start < into_equality->span.end
        && into_bound->span.start <= into_equality->span.start
        && into_equality->span.end <= into_bound->span.end
        && hir_type_is_projection(&hir, into_equality->value,
            into_item->definition, into_item->definition,
            into_associated_item->definition),
        "IntoIterator::IntoIter bound/equality identities differ");
    check(deref_target != NULL && sized_item != NULL && deref_bound != NULL
        && deref_target->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && deref_bound->modifier == CM_HIR_ASSOC_BOUND_RELAXED
        && cm_hir_def_id_equal(deref_bound->trait_type.definition,
            sized_item->definition)
        && deref_bound->trait_type.argument_count == 0u
        && deref_bound->trait_type.arguments == NULL
        && deref_bound->equality_count == 0u
        && deref_bound->equalities == NULL
        && deref_bound->span.source == ops_information.source
        && deref_bound->span.start < deref_bound->span.end,
        "Deref::Target did not retain its exact ?Sized bound");
    check(into_method != NULL && hir_function_is_ordinary(&hir, into_method)
        && into_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_VALUE
        && into_method->data.function_item.signature.parameter_count == 1u
        && into_method->data.function_item.signature.parameters != NULL
        && hir_type_is_self(&hir,
            into_method->data.function_item.signature.parameters[0].type,
            into_item->definition)
        && hir_type_is_projection(&hir,
            into_method->data.function_item.signature.return_type,
            into_item->definition, into_item->definition,
            into_associated_iter->definition)
        && into_method->data.function_item.body == CM_HIR_BODY_NONE,
        "IntoIterator::into_iter signature or body state differs");
    type = deref_method == NULL ? NULL : cm_hir_get_type(&hir,
        deref_method->data.function_item.signature.parameters[0].type);
    pointee = hir_reference_pointee(&hir, type);
    check(deref_method != NULL && hir_function_is_ordinary(&hir, deref_method)
        && deref_method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && deref_method->data.function_item.signature.parameter_count == 1u
        && type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && hir_type_is_self(&hir, type->data.reference_type.pointee,
            deref_item->definition)
        && pointee != NULL
        && cm_hir_def_id_equal(pointee->data.self_type.owner,
            deref_item->definition)
        && deref_method->data.function_item.body == CM_HIR_BODY_NONE,
        "Deref::deref receiver or body state differs");
    type = deref_method == NULL ? NULL : cm_hir_get_type(&hir,
        deref_method->data.function_item.signature.return_type);
    pointee = hir_reference_pointee(&hir, type);
    check(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && pointee != NULL && pointee->kind == CM_HIR_TYPE_PROJECTION_KIND
        && hir_type_is_projection(&hir, type->data.reference_type.pointee,
            deref_item->definition, deref_item->definition,
            deref_target->definition),
        "Deref::deref return projection owner differs");

    hir_dump = dump_hir(&hir);
    first_bound_dump = hir_dump == NULL ? NULL
        : strstr(hir_dump, "associated-type-bound item#");
    equality_dump = hir_dump == NULL ? NULL
        : strstr(hir_dump, "associated-type-equality item#");
    second_bound_dump = first_bound_dump == NULL ? NULL
        : strstr(first_bound_dump + 1, "associated-type-bound item#");
    required_modifier_dump = first_bound_dump == NULL ? NULL
        : strstr(first_bound_dump, "modifier=required trait=");
    relaxed_modifier_dump = second_bound_dump == NULL ? NULL
        : strstr(second_bound_dump, "modifier=relaxed trait=");
    check(hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && first_bound_dump != NULL && equality_dump != NULL
        && second_bound_dump != NULL && first_bound_dump < equality_dump
        && equality_dump < second_bound_dump
        && required_modifier_dump != NULL
        && required_modifier_dump < equality_dump
        && relaxed_modifier_dump != NULL,
        "IntoIterator/Deref hir-v27 bound/equality dump order differs");
    free(hir_dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_iterator_methods_fixture(void)
{
    static const char fixture[] =
        "tests/hir/fixtures/iterator-methods/lib.rs";
    static const char *const child_names[] = {
        "Item", "next", "size_hint", "advance_by", "nth", "count",
        "last", "step_by", "enumerate", "peekable", "skip", "take",
        "fuse", "by_ref"
    };
    static const char *const method_names[] = {
        "next", "size_hint", "advance_by", "nth", "count", "last",
        "step_by", "enumerate", "peekable", "skip", "take", "fuse",
        "by_ref"
    };
    static const char *const dump_item_names[] = {
        "Sized", "Iterator", "Item", "next", "size_hint", "advance_by",
        "nth", "count", "last", "step_by", "enumerate", "peekable",
        "skip", "take", "fuse", "by_ref", "Option", "Result", "NonZero",
        "StepBy", "Enumerate", "Peekable", "Skip", "Take", "Fuse"
    };
    static const CmHirReceiverKind receiver_kinds[] = {
        CM_HIR_RECEIVER_REF_MUTABLE,
        CM_HIR_RECEIVER_REF_SHARED,
        CM_HIR_RECEIVER_REF_MUTABLE,
        CM_HIR_RECEIVER_REF_MUTABLE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_VALUE,
        CM_HIR_RECEIVER_REF_MUTABLE
    };
    static const unsigned char has_usize_parameter[] = {
        0u, 0u, 1u, 1u, 0u, 0u, 1u, 0u, 0u, 1u, 1u, 0u, 0u
    };
    static const char *const usize_parameter_names[] = {
        NULL, NULL, "n", "n", NULL, NULL, "step", NULL, NULL, "n", "n",
        NULL, NULL
    };
    static const unsigned char has_sized_predicate[] = {
        0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u
    };
    static const char *const trait_attributes_expected[] = {
        "stable(feature = \"rust1\", since = \"1.0.0\")",
        "rustc_on_unimplemented(\n"
        "    on(\n"
        "        Self = \"core::ops::range::RangeTo<Idx>\",\n"
        "        note = \"you might have meant to use a bounded `Range`\"\n"
        "    ),\n"
        "    on(\n"
        "        Self = \"core::ops::range::RangeToInclusive<Idx>\",\n"
        "        note = \"you might have meant to use a bounded "
            "`RangeInclusive`\"\n"
        "    ),\n"
        "    label = \"`{Self}` is not an iterator\",\n"
        "    message = \"`{Self}` is not an iterator\"\n"
        ")",
        "doc(notable_trait)",
        "lang = \"iterator\"",
        "rustc_diagnostic_item = \"Iterator\"",
        "must_use = \"iterators are lazy and do nothing unless consumed\""
    };
    static const char *const item_attributes_expected[] = {
        "rustc_diagnostic_item = \"IteratorItem\"",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const next_attributes_expected[] = {
        "lang = \"next\"",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const inline_stable_attributes_expected[] = {
        "inline",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const advance_by_attributes_expected[] = {
        "inline",
        "unstable(feature = \"iter_advance_by\", reason = \"recently "
            "added\", issue = \"77404\")"
    };
    static const char *const step_by_attributes_expected[] = {
        "inline",
        "stable(feature = \"iterator_step_by\", since = \"1.28.0\")"
    };
    static const char *const enumerate_attributes_expected[] = {
        "inline",
        "stable(feature = \"rust1\", since = \"1.0.0\")",
        "rustc_diagnostic_item = \"enumerate_method\""
    };
    static const char *const take_attributes_expected[] = {
        "doc(alias = \"limit\")",
        "inline",
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    static const char *const stable_attributes_expected[] = {
        "stable(feature = \"rust1\", since = \"1.0.0\")"
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmResolveModuleInfo root_information;
    const CmAst *root_ast;
    CmResolveEffectiveItem iterator_view;
    CmResolveEffectiveItem child_views[14];
    CmResolveEffectiveAttribute trait_attributes[6];
    CmResolveEffectiveAttribute item_attributes[2];
    CmResolveEffectiveAttribute method_attributes[13][3];
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *sized_item;
    const CmHirItem *option_item;
    const CmHirItem *result_item;
    const CmHirItem *nonzero_item;
    const CmHirItem *adapter_items[6];
    const CmHirItem *iterator_item;
    const CmHirItem *associated_item;
    const CmHirItem *methods[13];
    const CmHirFunctionSignature *signature;
    const CmHirType *type;
    const CmHirType *argument;
    const CmHirType *second_argument;
    const CmHirType *nested_argument;
    const CmSourceFile *fixture_source;
    const char *const *expected_attributes;
    uint32_t expected_attribute_count;
    uint32_t expected_dump_attribute_count;
    char *hir_dump;
    const char *dump_positions[25];
    const char *dump_end;
    char marker[96];
    size_t index;
    size_t predicate_total;
    int graph_children_ok;
    int graph_sources_ok;
    int graph_attributes_ok;
    int hir_items_ok;
    int signatures_ok;
    int predicates_ok;
    int returns_ok;
    int dump_order_ok;
    int written;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        check(0, "could not build Rust 1.90 Iterator-methods fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    memset(&root_information, 0, sizeof(root_information));
    memset(&iterator_view, 0, sizeof(iterator_view));
    memset(child_views, 0, sizeof(child_views));
    root_ast = NULL;
    check(cm_module_graph_module_count(&graph) == 1u
        && cm_module_graph_get_root(&graph, &root_graph)
        && cm_module_graph_get_module(&graph, root_graph, &root_information)
        && root_information.child_count == 0u
        && root_information.import_count == 0u
        && cm_module_graph_borrow_ast(&graph, root_graph, &root_ast),
        "Iterator fixture was not a one-module source graph");
    (void)effective_item_named(&graph, revision, root_graph, "Iterator",
        &iterator_view);
    graph_children_ok = iterator_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && iterator_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && iterator_view.child_count == 14u && root_ast != NULL;
    for (index = 0u; index < 14u && graph_children_ok; ++index) {
        const CmAstItem *declaration;

        graph_children_ok =
            cm_module_graph_get_effective_child(&graph, revision, root_graph,
                iterator_view.id, (uint32_t)index, &child_views[index])
                == CM_RESOLVE_VIEW_OK;
        declaration = graph_children_ok ? cm_ast_get_item(root_ast,
            child_views[index].declaration.item) : NULL;
        graph_children_ok = declaration != NULL
            && ast_name_is(root_ast, declaration->name, child_names[index]);
    }
    check(graph_children_ok,
        "Iterator effective children were not Item plus 13 ordered methods");
    graph_sources_ok = effective_item_has_source(&iterator_view,
        root_information.source);
    for (index = 0u; index < 14u && graph_sources_ok; ++index) {
        graph_sources_ok = effective_item_has_source(&child_views[index],
            root_information.source);
    }
    check(graph_sources_ok,
        "Iterator effective declarations lost fixture source provenance");

    graph_attributes_ok = effective_attributes_are(&graph, revision,
        root_graph, &iterator_view, trait_attributes_expected, 6u,
        trait_attributes)
        && effective_attributes_are(&graph, revision, root_graph,
            &child_views[0], item_attributes_expected, 2u, item_attributes);
    for (index = 0u; index < 13u && graph_attributes_ok; ++index) {
        if (index == 0u) {
            expected_attributes = next_attributes_expected;
            expected_attribute_count = 2u;
        } else if (index == 2u) {
            expected_attributes = advance_by_attributes_expected;
            expected_attribute_count = 2u;
        } else if (index == 6u) {
            expected_attributes = step_by_attributes_expected;
            expected_attribute_count = 2u;
        } else if (index == 7u) {
            expected_attributes = enumerate_attributes_expected;
            expected_attribute_count = 3u;
        } else if (index == 10u) {
            expected_attributes = take_attributes_expected;
            expected_attribute_count = 3u;
        } else if (index == 12u) {
            expected_attributes = stable_attributes_expected;
            expected_attribute_count = 1u;
        } else {
            expected_attributes = inline_stable_attributes_expected;
            expected_attribute_count = 2u;
        }
        graph_attributes_ok = effective_attributes_are(&graph, revision,
            root_graph, &child_views[index + 1u], expected_attributes,
            expected_attribute_count, method_attributes[index]);
    }
    check(graph_attributes_ok,
        "Iterator effective attributes or attribute provenance differ");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "iterator_methods";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower Iterator methods: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    sized_item = find_hir_item_anywhere(&hir, "Sized");
    option_item = find_hir_item_anywhere(&hir, "Option");
    result_item = find_hir_item_anywhere(&hir, "Result");
    nonzero_item = find_hir_item_anywhere(&hir, "NonZero");
    adapter_items[0] = find_hir_item_anywhere(&hir, "StepBy");
    adapter_items[1] = find_hir_item_anywhere(&hir, "Enumerate");
    adapter_items[2] = find_hir_item_anywhere(&hir, "Peekable");
    adapter_items[3] = find_hir_item_anywhere(&hir, "Skip");
    adapter_items[4] = find_hir_item_anywhere(&hir, "Take");
    adapter_items[5] = find_hir_item_anywhere(&hir, "Fuse");
    iterator_item = find_hir_item_anywhere(&hir, "Iterator");
    associated_item = iterator_item == NULL ? NULL
        : find_hir_associated_item(&hir, iterator_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    hir_items_ok = result.error_count == 0u
        && cm_hir_module_map_count(&map) == 1u && sized_item != NULL
        && option_item != NULL && result_item != NULL && nonzero_item != NULL
        && iterator_item != NULL && associated_item != NULL;
    for (index = 0u; index < 6u && hir_items_ok; ++index) {
        hir_items_ok = adapter_items[index] != NULL;
    }
    for (index = 0u; index < 13u; ++index) {
        methods[index] = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, method_names[index]);
        if (methods[index] == NULL) hir_items_ok = 0;
    }
    check(hir_items_ok,
        "Rust 1.90 Iterator-methods fixture did not lower completely");

    hir_items_ok = hir_items_ok
        && hir_attributes_match_graph(&graph, &hir, iterator_item,
            trait_attributes, 6u)
        && hir_attributes_match_graph(&graph, &hir, associated_item,
            item_attributes, 2u)
        && iterator_item->span.source == root_information.source
        && associated_item->span.source == root_information.source
        && associated_item->data.type_alias_item.target == CM_HIR_TYPE_NONE;
    for (index = 0u; index < 13u && hir_items_ok; ++index) {
        expected_attribute_count = child_views[index + 1u].attribute_count;
        hir_items_ok = hir_attributes_match_graph(&graph, &hir,
                methods[index], method_attributes[index],
                expected_attribute_count)
            && methods[index]->span.source == root_information.source;
    }
    check(hir_items_ok,
        "Iterator HIR attributes or fixture provenance differ");

    signatures_ok = iterator_item != NULL;
    predicates_ok = iterator_item != NULL && sized_item != NULL;
    predicate_total = 0u;
    for (index = 0u; index < 13u && signatures_ok; ++index) {
        signature = methods[index] == NULL ? NULL
            : &methods[index]->data.function_item.signature;
        signatures_ok = hir_function_is_ordinary(&hir, methods[index])
            && signature != NULL
            && signature->receiver == receiver_kinds[index]
            && signature->parameter_count
                == 1u + (uint32_t)has_usize_parameter[index]
            && signature->parameters != NULL;
        if (!signatures_ok) break;
        type = cm_hir_get_type(&hir, signature->parameters[0].type);
        if (receiver_kinds[index] == CM_HIR_RECEIVER_VALUE) {
            signatures_ok = hir_type_is_self(&hir,
                signature->parameters[0].type, iterator_item->definition);
        } else {
            signatures_ok = type != NULL
                && type->kind == CM_HIR_TYPE_REFERENCE_KIND
                && type->data.reference_type.mutability
                    == (receiver_kinds[index] == CM_HIR_RECEIVER_REF_MUTABLE
                        ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE)
                && hir_type_is_self(&hir, type->data.reference_type.pointee,
                    iterator_item->definition);
        }
        if (signatures_ok && has_usize_parameter[index] != 0u) {
            signatures_ok = hir_name_is(&hir, signature->parameters[1].name,
                    usize_parameter_names[index])
                && hir_type_is_usize(&hir, signature->parameters[1].type);
        }
        signatures_ok = signatures_ok
            && (index == 0u
                ? methods[index]->data.function_item.body
                    == CM_HIR_BODY_NONE
                : methods[index]->data.function_item.body
                    != CM_HIR_BODY_NONE);
    }
    check(signatures_ok,
        "Iterator method receivers, usize parameters, or body states differ");

    fixture_source = cm_source_get(&sources, root_information.source);
    for (index = 0u; index < 13u && predicates_ok; ++index) {
        const CmHirTraitPredicate *predicate;

        predicates_ok = methods[index] != NULL
            && methods[index]->predicate_count
                == (uint32_t)has_sized_predicate[index]
            && (has_sized_predicate[index] != 0u
                || methods[index]->predicates == NULL);
        if (!predicates_ok || has_sized_predicate[index] == 0u) continue;
        predicate = &methods[index]->predicates[0];
        predicate_total += 1u;
        predicates_ok = hir_type_is_self(&hir, predicate->subject,
                iterator_item->definition)
            && cm_hir_def_id_equal(predicate->trait_type.definition,
                sized_item->definition)
            && predicate->trait_type.argument_count == 0u
            && predicate->trait_type.arguments == NULL
            && fixture_source != NULL
            && predicate->span.source == root_information.source
            && source_span_is(&sources, predicate->span, "Self: Sized");
    }
    check(predicates_ok && predicate_total == 9u,
        "Iterator Self: Sized predicates lost identity, span, or placement");

    returns_ok = iterator_item != NULL && associated_item != NULL
        && option_item != NULL && result_item != NULL && nonzero_item != NULL;
    for (index = 0u; index < 3u && returns_ok; ++index) {
        static const size_t option_method_indices[] = { 0u, 3u, 5u };
        size_t method_index;

        method_index = option_method_indices[index];
        type = cm_hir_get_type(&hir, methods[method_index]
            ->data.function_item.signature.return_type);
        argument = hir_named_type_argument(&hir, type, 0u);
        returns_ok = type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition,
                option_item->definition)
            && type->data.named_type.argument_count == 1u
            && argument != NULL && argument->kind == CM_HIR_TYPE_PROJECTION_KIND
            && hir_type_is_projection(&hir,
                type->data.named_type.arguments[0].data.type,
                iterator_item->definition, iterator_item->definition,
                associated_item->definition);
    }
    type = returns_ok ? cm_hir_get_type(&hir,
        methods[1]->data.function_item.signature.return_type) : NULL;
    argument = type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
            && type->data.tuple_type.element_count == 2u
            && type->data.tuple_type.elements != NULL
        ? cm_hir_get_type(&hir, type->data.tuple_type.elements[1]) : NULL;
    second_argument = hir_named_type_argument(&hir, argument, 0u);
    returns_ok = returns_ok && type != NULL
        && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 2u
        && hir_type_is_usize(&hir, type->data.tuple_type.elements[0])
        && argument != NULL && argument->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(argument->data.named_type.definition,
            option_item->definition)
        && argument->data.named_type.argument_count == 1u
        && second_argument != NULL
        && second_argument->kind == CM_HIR_TYPE_INTEGER_KIND
        && hir_type_is_usize(&hir,
            argument->data.named_type.arguments[0].data.type);
    type = returns_ok ? cm_hir_get_type(&hir,
        methods[2]->data.function_item.signature.return_type) : NULL;
    argument = hir_named_type_argument(&hir, type, 0u);
    second_argument = hir_named_type_argument(&hir, type, 1u);
    nested_argument = hir_named_type_argument(&hir, second_argument, 0u);
    returns_ok = returns_ok && type != NULL
        && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition,
            result_item->definition)
        && type->data.named_type.argument_count == 2u
        && argument != NULL && argument->kind == CM_HIR_TYPE_UNIT_KIND
        && second_argument != NULL
        && second_argument->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(second_argument->data.named_type.definition,
            nonzero_item->definition)
        && second_argument->data.named_type.argument_count == 1u
        && nested_argument != NULL
        && hir_type_is_usize(&hir,
            second_argument->data.named_type.arguments[0].data.type)
        && hir_type_is_usize(&hir,
            methods[4]->data.function_item.signature.return_type);
    for (index = 0u; index < 6u && returns_ok; ++index) {
        type = cm_hir_get_type(&hir,
            methods[index + 6u]->data.function_item.signature.return_type);
        argument = hir_named_type_argument(&hir, type, 0u);
        returns_ok = type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition,
                adapter_items[index]->definition)
            && type->data.named_type.argument_count == 1u
            && argument != NULL && argument->kind == CM_HIR_TYPE_SELF_KIND
            && hir_type_is_self(&hir,
                type->data.named_type.arguments[0].data.type,
                iterator_item->definition);
    }
    type = returns_ok ? cm_hir_get_type(&hir,
        methods[12]->data.function_item.signature.return_type) : NULL;
    returns_ok = returns_ok && type != NULL
        && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_MUTABLE
        && hir_type_is_self(&hir, type->data.reference_type.pointee,
            iterator_item->definition);
    check(returns_ok, "Iterator method return types differ");

    hir_dump = dump_hir(&hir);
    dump_order_ok = hir_dump != NULL
        && strncmp(hir_dump, "hir-v27\n", strlen("hir-v27\n")) == 0;
    dump_end = hir_dump == NULL ? NULL : hir_dump + strlen(hir_dump);
    for (index = 0u; index < 25u && dump_order_ok; ++index) {
        written = snprintf(marker, sizeof(marker), " name=\"%s\"",
            dump_item_names[index]);
        dump_positions[index] = written < 0
                || (size_t)written >= sizeof(marker)
            ? NULL : strstr(index == 0u ? hir_dump
                : dump_positions[index - 1u] + 1, marker);
        dump_order_ok = dump_positions[index] != NULL;
    }
    for (index = 0u; index < 15u && dump_order_ok; ++index) {
        const char *block_begin;
        const char *block_end;

        block_begin = dump_positions[index + 1u];
        block_end = dump_positions[index + 2u];
        if (index == 0u) {
            expected_dump_attribute_count = 6u;
        } else if (index == 1u) {
            expected_dump_attribute_count = 2u;
        } else {
            expected_dump_attribute_count =
                child_views[index - 1u].attribute_count;
        }
        dump_order_ok = text_count_between(block_begin, block_end,
                "item-attr item#") == expected_dump_attribute_count
            && text_count_between(block_begin, block_end,
                "trait-predicate item#")
                == (index >= 2u
                    ? (size_t)has_sized_predicate[index - 2u] : 0u);
    }
    dump_order_ok = dump_order_ok
        && text_count_between(hir_dump, dump_end, "trait-predicate item#")
            == 9u;
    check(dump_order_ok,
        "Iterator hir-v27 dump lost item, attribute, or predicate order");
    free(hir_dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_rustc_iterator_generic_methods_fixture(void)
{
    static const char fixture[] =
        "tests/hir/fixtures/iterator-generic-methods/lib.rs";
    static const char *const method_names[] = {
        "next", "size_hint", "count", "last", "advance_by", "nth",
        "step_by", "chain", "zip", "intersperse", "intersperse_with",
        "map", "for_each", "filter", "filter_map", "enumerate",
        "peekable", "skip_while", "take_while", "map_while", "skip",
        "take", "scan", "flat_map", "flatten", "fuse", "inspect",
        "by_ref", "collect", "collect_into", "partition",
        "is_partitioned", "try_fold", "try_for_each", "fold", "reduce",
        "all", "any", "find", "find_map", "position", "rposition",
        "max", "min", "max_by_key", "max_by", "min_by_key", "min_by",
        "rev", "unzip", "cycle", "sum", "product", "cmp", "cmp_by",
        "partial_cmp", "partial_cmp_by", "eq", "eq_by", "ne", "lt",
        "le", "gt", "ge", "is_sorted", "is_sorted_by",
        "is_sorted_by_key", "__iterator_get_unchecked"
    };
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphRevision revision;
    CmModuleId root_graph;
    CmResolveEffectiveItem iterator_view;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *iterator_item;
    const CmHirItem *associated_item;
    size_t method_index;
    size_t generic_count;
    int methods_ok;

    cm_cfg_set_init(&cfg);
    if (!build_file_graph(&sources, &graph, fixture, &cfg, &revision)) {
        check(0, "could not build generic Iterator-methods fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    root_graph = CM_MODULE_NONE;
    memset(&iterator_view, 0, sizeof(iterator_view));
    check(cm_module_graph_get_root(&graph, &root_graph)
        && effective_item_named(&graph, revision, root_graph, "Iterator",
            &iterator_view)
        && iterator_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && iterator_view.child_count == 69u,
        "generic Iterator fixture did not retain Item plus 68 methods");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "iterator_generic_methods";
    result = lower_module_graph(&hir, &graph, revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generic Iterator methods: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    iterator_item = find_hir_item_anywhere(&hir, "Iterator");
    associated_item = iterator_item == NULL ? NULL
        : find_hir_associated_item(&hir, iterator_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    methods_ok = result.error_count == 0u && iterator_item != NULL
        && associated_item != NULL;
    generic_count = 0u;
    for (method_index = 0u;
         method_index < sizeof(method_names) / sizeof(method_names[0]);
         ++method_index) {
        const CmHirItem *method;

        method = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, method_names[method_index]);
        methods_ok = methods_ok && method != NULL
            && method->span.source != 0u
            && (method_index == 0u
                ? method->data.function_item.body == CM_HIR_BODY_NONE
                : method->data.function_item.body != CM_HIR_BODY_NONE);
        if (method != NULL && method->generic_parameter_count != 0u) {
            generic_count += 1u;
        }
    }
    check(methods_ok && generic_count == 47u,
        "generic Iterator fixture did not lower all 68 exact declarations");

    {
        const CmHirItem *partial_eq;
        const CmHirItem *partial_ord;
        const CmHirItem *fn_once;
        const CmHirItem *fn_mut;
        const CmHirItem *fn_output;
        const CmHirGenericParam *partial_eq_rhs;
        const CmHirGenericParam *partial_ord_rhs;
        const CmHirGenericParam *fn_once_args;
        const CmHirGenericParam *fn_mut_args;
        const CmHirType *partial_ord_super_arg;
        const CmHirType *fn_mut_super_arg;

        partial_eq = find_hir_item_anywhere(&hir, "PartialEq");
        partial_ord = find_hir_item_anywhere(&hir, "PartialOrd");
        fn_once = find_hir_item_anywhere(&hir, "FnOnce");
        fn_mut = find_hir_item_anywhere(&hir, "FnMut");
        fn_output = fn_once == NULL ? NULL
            : find_hir_associated_item(&hir, fn_once->definition,
                CM_HIR_ITEM_TYPE_ALIAS, "Output");
        partial_eq_rhs = partial_eq == NULL
            || partial_eq->generic_parameter_count != 1u ? NULL
            : cm_hir_get_generic_param(&hir,
                partial_eq->generic_parameter_start);
        partial_ord_rhs = partial_ord == NULL
            || partial_ord->generic_parameter_count != 1u ? NULL
            : cm_hir_get_generic_param(&hir,
                partial_ord->generic_parameter_start);
        fn_once_args = fn_once == NULL
            || fn_once->generic_parameter_count != 1u ? NULL
            : cm_hir_get_generic_param(&hir,
                fn_once->generic_parameter_start);
        fn_mut_args = fn_mut == NULL
            || fn_mut->generic_parameter_count != 1u ? NULL
            : cm_hir_get_generic_param(&hir,
                fn_mut->generic_parameter_start);
        partial_ord_super_arg = partial_ord == NULL
                || partial_ord->data.trait_item.supertrait_count != 1u
                || partial_ord->data.trait_item.supertraits == NULL
            ? NULL : hir_generic_type_argument(&hir,
                partial_ord->data.trait_item.supertraits[0]
                    .trait_type.arguments,
                partial_ord->data.trait_item.supertraits[0]
                    .trait_type.argument_count,
                0u);
        fn_mut_super_arg = fn_mut == NULL
                || fn_mut->data.trait_item.supertrait_count != 1u
                || fn_mut->data.trait_item.supertraits == NULL
            ? NULL : hir_generic_type_argument(&hir,
                fn_mut->data.trait_item.supertraits[0].trait_type.arguments,
                fn_mut->data.trait_item.supertraits[0]
                    .trait_type.argument_count,
                0u);
        check(partial_eq != NULL && partial_eq_rhs != NULL
            && partial_eq_rhs->kind == CM_HIR_GENERIC_TYPE
            && partial_eq_rhs->has_default
            && partial_eq_rhs->default_argument.kind
                == CM_HIR_GENERIC_ARG_TYPE
            && hir_type_is_self(&hir,
                partial_eq_rhs->default_argument.data.type,
                partial_eq->definition),
            "PartialEq did not retain Rhs = Self");
        check(partial_ord != NULL && partial_eq != NULL
            && partial_ord_rhs != NULL
            && partial_ord_rhs->kind == CM_HIR_GENERIC_TYPE
            && partial_ord_rhs->has_default
            && partial_ord_rhs->default_argument.kind
                == CM_HIR_GENERIC_ARG_TYPE
            && hir_type_is_self(&hir,
                partial_ord_rhs->default_argument.data.type,
                partial_ord->definition)
            && cm_hir_def_id_equal(partial_ord->data.trait_item
                    .supertraits[0].trait_type.definition,
                partial_eq->definition)
            && partial_ord_super_arg != NULL
            && partial_ord_super_arg->kind == CM_HIR_TYPE_PARAMETER_KIND
            && hir_type_is_parameter(&hir,
                partial_ord->data.trait_item.supertraits[0]
                    .trait_type.arguments[0].data.type,
                partial_ord->generic_parameter_start)
            && source_span_is(&sources,
                partial_ord->data.trait_item.supertraits[0].span,
                "PartialEq<Rhs>"),
            "PartialOrd default or PartialEq<Rhs> supertrait is not exact");
        check(fn_once != NULL && fn_output != NULL && fn_once_args != NULL
            && fn_once_args->kind == CM_HIR_GENERIC_TYPE
            && !fn_once_args->has_default
            && fn_output->data.type_alias_item.target == CM_HIR_TYPE_NONE,
            "FnOnce<Args>::Output declaration is not exact");
        check(fn_mut != NULL && fn_once != NULL && fn_mut_args != NULL
            && fn_mut_args->kind == CM_HIR_GENERIC_TYPE
            && !fn_mut_args->has_default
            && fn_mut->data.trait_item.supertrait_count == 1u
            && fn_mut->data.trait_item.supertraits != NULL
            && cm_hir_def_id_equal(fn_mut->data.trait_item.supertraits[0]
                    .trait_type.definition,
                fn_once->definition)
            && fn_mut_super_arg != NULL
            && fn_mut_super_arg->kind == CM_HIR_TYPE_PARAMETER_KIND
            && hir_type_is_parameter(&hir,
                fn_mut->data.trait_item.supertraits[0]
                    .trait_type.arguments[0].data.type,
                fn_mut->generic_parameter_start)
            && source_span_is(&sources,
                fn_mut->data.trait_item.supertraits[0].span,
                "FnOnce<Args>"),
            "FnMut<Args>: FnOnce<Args> supertrait is not exact");
    }

    {
        const CmHirItem *sized;
        const CmHirItem *from_iterator;
        const CmHirItem *into_iterator;
        const CmHirItem *into_item;
        const CmHirItem *into_iter;
        const CmHirItem *chain_type;
        const CmHirItem *collect;
        const CmHirItem *chain;
        const CmHirTraitPredicate *predicate;
        const CmHirType *argument;
        const CmHirType *return_type;
        const CmHirType *return_projection;

        sized = find_hir_item_anywhere(&hir, "Sized");
        from_iterator = find_hir_item_anywhere(&hir, "FromIterator");
        into_iterator = find_hir_item_anywhere(&hir, "IntoIterator");
        into_item = into_iterator == NULL ? NULL
            : find_hir_associated_item(&hir, into_iterator->definition,
                CM_HIR_ITEM_TYPE_ALIAS, "Item");
        into_iter = into_iterator == NULL ? NULL
            : find_hir_associated_item(&hir, into_iterator->definition,
                CM_HIR_ITEM_TYPE_ALIAS, "IntoIter");
        chain_type = find_hir_item_anywhere(&hir, "Chain");
        collect = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, "collect");
        chain = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, "chain");
        predicate = collect == NULL || collect->predicate_count != 2u
            || collect->predicates == NULL ? NULL : &collect->predicates[0];
        argument = predicate == NULL ? NULL : hir_generic_type_argument(&hir,
            predicate->trait_type.arguments,
            predicate->trait_type.argument_count, 0u);
        check(sized != NULL && from_iterator != NULL && collect != NULL
            && collect->generic_parameter_count == 1u
            && predicate != NULL
            && hir_type_is_parameter(&hir, predicate->subject,
                collect->generic_parameter_start)
            && cm_hir_def_id_equal(predicate->trait_type.definition,
                from_iterator->definition)
            && predicate->trait_type.argument_count == 1u
            && argument != NULL
            && argument->kind == CM_HIR_TYPE_PROJECTION_KIND
            && hir_type_is_projection(&hir,
                predicate->trait_type.arguments[0].data.type,
                iterator_item->definition, iterator_item->definition,
                associated_item->definition)
            && predicate->equality_count == 0u
            && source_span_is(&sources, predicate->span,
                "FromIterator<Self::Item>")
            && hir_type_is_self(&hir, collect->predicates[1].subject,
                iterator_item->definition)
            && cm_hir_def_id_equal(collect->predicates[1]
                    .trait_type.definition,
                sized->definition)
            && source_span_is(&sources, collect->predicates[1].span,
                "Self: Sized"),
            "collect did not retain inline-before-where predicate order");

        predicate = chain == NULL || chain->predicate_count != 2u
            || chain->predicates == NULL ? NULL : &chain->predicates[1];
        return_type = chain == NULL ? NULL : cm_hir_get_type(&hir,
            chain->data.function_item.signature.return_type);
        return_projection = return_type == NULL
                || return_type->kind != CM_HIR_TYPE_ADT_KIND
                || return_type->data.named_type.argument_count != 2u
            ? NULL : hir_generic_type_argument(&hir,
                return_type->data.named_type.arguments,
                return_type->data.named_type.argument_count, 1u);
        check(into_iterator != NULL && into_item != NULL && into_iter != NULL
            && chain_type != NULL && chain != NULL
            && chain->generic_parameter_count == 1u
            && predicate != NULL
            && hir_type_is_parameter(&hir, predicate->subject,
                chain->generic_parameter_start)
            && cm_hir_def_id_equal(predicate->trait_type.definition,
                into_iterator->definition)
            && predicate->equality_count == 1u
            && predicate->equalities != NULL
            && cm_hir_def_id_equal(predicate->equalities[0].associated_type,
                into_item->definition)
            && hir_type_is_projection(&hir, predicate->equalities[0].value,
                iterator_item->definition, iterator_item->definition,
                associated_item->definition)
            && source_span_is(&sources, predicate->span,
                "U: IntoIterator<Item = Self::Item>")
            && source_span_is(&sources, predicate->equalities[0].span,
                "Item = Self::Item")
            && return_type != NULL
            && return_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(return_type->data.named_type.definition,
                chain_type->definition)
            && hir_type_is_self(&hir,
                return_type->data.named_type.arguments[0].data.type,
                iterator_item->definition)
            && return_projection != NULL
            && return_projection->kind == CM_HIR_TYPE_PROJECTION_KIND
            && hir_type_is_parameter(&hir,
                return_projection->data.projection_type.self_type,
                chain->generic_parameter_start)
            && cm_hir_def_id_equal(return_projection->data.projection_type
                    .trait_type.definition,
                into_iterator->definition)
            && cm_hir_def_id_equal(return_projection->data.projection_type
                    .associated_type.definition,
                into_iter->definition),
            "chain did not resolve U::IntoIter from its predicate");
    }

    {
        const CmHirItem *sized;
        const CmHirItem *fn_once;
        const CmHirItem *fn_mut;
        const CmHirItem *fn_output;
        const CmHirItem *map_method;
        const CmHirTraitPredicate *predicate;
        const CmHirAssociatedTypeEquality *equality;
        const CmHirType *tuple;

        sized = find_hir_item_anywhere(&hir, "Sized");
        fn_once = find_hir_item_anywhere(&hir, "FnOnce");
        fn_mut = find_hir_item_anywhere(&hir, "FnMut");
        fn_output = fn_once == NULL ? NULL
            : find_hir_associated_item(&hir, fn_once->definition,
                CM_HIR_ITEM_TYPE_ALIAS, "Output");
        map_method = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, "map");
        predicate = map_method == NULL || map_method->predicate_count != 2u
            || map_method->predicates == NULL ? NULL
            : &map_method->predicates[1];
        equality = predicate == NULL || predicate->equality_count != 1u
            || predicate->equalities == NULL ? NULL
            : &predicate->equalities[0];
        tuple = predicate == NULL ? NULL : hir_generic_type_argument(&hir,
            predicate->trait_type.arguments,
            predicate->trait_type.argument_count, 0u);
        check(sized != NULL && fn_once != NULL && fn_mut != NULL
            && fn_output != NULL && map_method != NULL
            && map_method->generic_parameter_count == 2u
            && cm_hir_def_id_equal(map_method->predicates[0]
                    .trait_type.definition,
                sized->definition)
            && hir_type_is_self(&hir, map_method->predicates[0].subject,
                iterator_item->definition)
            && predicate != NULL
            && hir_type_is_parameter(&hir, predicate->subject,
                map_method->generic_parameter_start + 1u)
            && cm_hir_def_id_equal(predicate->trait_type.definition,
                fn_mut->definition)
            && predicate->trait_type.argument_count == 1u
            && tuple != NULL && tuple->kind == CM_HIR_TYPE_TUPLE_KIND
            && tuple->data.tuple_type.element_count == 1u
            && tuple->data.tuple_type.elements != NULL
            && hir_type_is_projection(&hir,
                tuple->data.tuple_type.elements[0],
                iterator_item->definition, iterator_item->definition,
                associated_item->definition)
            && source_span_is(&sources, tuple->span, "(Self::Item)")
            && equality != NULL
            && cm_hir_def_id_equal(equality->associated_type,
                fn_output->definition)
            && hir_type_is_parameter(&hir, equality->value,
                map_method->generic_parameter_start)
            && source_span_is(&sources, predicate->span,
                "F: FnMut(Self::Item) -> B")
            && source_span_is(&sources, equality->span, "-> B"),
            "map callable predicate lost tuple or inherited Output identity");
    }

    {
        const CmHirItem *partial_ord;
        const CmHirItem *is_sorted;
        const CmHirTraitPredicate *predicate;
        const CmHirType *default_argument;

        partial_ord = find_hir_item_anywhere(&hir, "PartialOrd");
        is_sorted = iterator_item == NULL ? NULL
            : find_hir_associated_item(&hir, iterator_item->definition,
                CM_HIR_ITEM_FUNCTION, "is_sorted");
        predicate = is_sorted == NULL || is_sorted->predicate_count != 2u
            || is_sorted->predicates == NULL ? NULL
            : &is_sorted->predicates[1];
        default_argument = predicate == NULL ? NULL
            : hir_generic_type_argument(&hir,
                predicate->trait_type.arguments,
                predicate->trait_type.argument_count, 0u);
        check(partial_ord != NULL && predicate != NULL
            && cm_hir_def_id_equal(predicate->trait_type.definition,
                partial_ord->definition)
            && predicate->trait_type.argument_count == 1u
            && default_argument != NULL
            && predicate->trait_type.arguments[0].data.type
                == predicate->subject
            && default_argument->kind == CM_HIR_TYPE_PROJECTION_KIND
            && hir_type_is_projection(&hir, predicate->subject,
                iterator_item->definition, iterator_item->definition,
                associated_item->definition)
            && source_span_is(&sources, predicate->span,
                "Self::Item: PartialOrd"),
            "is_sorted did not materialize PartialOrd's Self default");
    }

    {
        char *dump_a;
        char *dump_b;
        const char *cursor;
        const char *dump_end;
        char marker[96];
        size_t item_index;
        size_t predicate_total;
        size_t equality_total;
        int dump_ok;
        int written;

        predicate_total = 0u;
        equality_total = 0u;
        for (item_index = 0u; item_index < hir.items.len; ++item_index) {
            const CmHirItem *item;
            uint32_t predicate_index;

            item = (const CmHirItem *)cm_vec_at_const(&hir.items,
                item_index);
            if (item == NULL) continue;
            predicate_total += item->predicate_count;
            for (predicate_index = 0u;
                 predicate_index < item->predicate_count;
                 ++predicate_index) {
                equality_total += item->predicates[predicate_index]
                    .equality_count;
            }
        }
        dump_a = dump_hir(&hir);
        dump_b = dump_hir(&hir);
        dump_ok = dump_a != NULL && dump_b != NULL
            && strcmp(dump_a, dump_b) == 0
            && strncmp(dump_a, "hir-v27\n", strlen("hir-v27\n")) == 0;
        cursor = dump_ok ? strstr(dump_a, " name=\"Iterator\"") : NULL;
        cursor = cursor == NULL ? NULL : strstr(cursor + 1, " name=\"Item\"");
        for (method_index = 0u;
             method_index < sizeof(method_names) / sizeof(method_names[0])
                && cursor != NULL;
             ++method_index) {
            written = snprintf(marker, sizeof(marker), " name=\"%s\"",
                method_names[method_index]);
            cursor = written < 0 || (size_t)written >= sizeof(marker)
                ? NULL : strstr(cursor + 1, marker);
        }
        dump_end = dump_a == NULL ? NULL : dump_a + strlen(dump_a);
        dump_ok = dump_ok && cursor != NULL
            && text_count_between(dump_a, dump_end,
                "trait-predicate item#") == predicate_total
            && text_count_between(dump_a, dump_end,
                "trait-predicate-equality item#") == equality_total
            && predicate_total != 0u && equality_total != 0u;
        check(dump_ok,
            "generic Iterator hir-v27 dump is incomplete or nondeterministic");
        free(dump_a);
        free(dump_b);
    }
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_method_completeness_is_transactional(void)
{
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    static const unsigned char source[] =
        "trait Trait { fn provided(&self) {} "
            "#[cfg(enabled)] fn required(&self); }\n"
        "struct Subject;\n"
        "use self::Subject as SubjectAlias;\n"
        "impl Trait for Subject { "
            "#[cfg(disabled)] fn required(&self) {} }\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveModuleInfo root_information;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmInternId sentinel_name;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_root;
    CmSpan sentinel_span;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "method-completeness/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add method completeness fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    memset(&root_information, 0, sizeof(root_information));
    check(graph_result.error_count == 0u
        && cm_module_graph_get_module(&graph, graph_result.root,
            &root_information)
        && root_information.import_count == 1u,
        "method completeness graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    sentinel_name = cm_hir_intern(&hir, "method-sentinel");
    sentinel_span.source = 993u;
    sentinel_span.start = 7u;
    sentinel_span.end = 8u;
    check(cm_hir_create_crate(&hir, sentinel_name, CM_HIR_EDITION_2024,
        sentinel_span, &sentinel_crate, &sentinel_root) == CM_HIR_OK,
        "could not create method completeness sentinel");
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_IMPL
        && strstr(result.first_error.message,
            "missing a required trait method") != NULL
        && result.crate_id == CM_HIR_CRATE_NONE
        && result.root_module == CM_HIR_MODULE_NONE
        && result.lowered_item_count == 0u
        && hir.crates.len == 1u && hir.modules.len == 1u
        && hir.items.len == 0u && hir.bodies.len == 0u
        && hir.types.len == 0u && hir.generic_parameters.len == 0u
        && hir.definitions.len == 1u
        && cm_interner_length(&hir.strings) == 1u
        && hir_name_is(&hir, sentinel_name, "method-sentinel")
        && cm_hir_get_crate(&hir, sentinel_crate) != NULL
        && cm_hir_get_module(&hir, sentinel_root) != NULL
        && cm_hir_module_map_count(&map) == 0u,
        "method completeness failure did not rewind imports, bodies, and HIR state");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_preflight_rejections(void)
{
    static const char *const rejected_sources[] = {
        "extern \"C\" { fn foreign(); }\n",
        "unsafe extern \"system\" { safe fn foreign(); }\n",
        "#[link(name = \"native\")] unsafe extern \"C\" { "
            "fn foreign(); }\n",
        "unsafe extern \"C\" { safe fn foreign<T>(); }\n",
        "unsafe extern \"C\" { static FOREIGN: u8; }\n",
        "unsafe extern \"C\" { type Generic<T>; }\n",
        "unsafe extern \"C\" { type Bounded: Sized; }\n",
        "unsafe extern \"C\" { type Alias = u8; }\n",
        "unsafe extern \"C\" { pub type Visible; }\n",
        "static mut SOURCE: u8 = 1;\n",
        "macro_rules! make { () => { static SOURCE: u8 = 1; } } "
            "make!();\n",
        "trait Values {} struct Number; impl Values for Number { "
            "const VALUE: u8 = 1; }\n"
    };
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    size_t index;

    for (index = 0u; index < sizeof(rejected_sources)
            / sizeof(rejected_sources[0]); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "reject/lib.rs",
            (const unsigned char *)rejected_sources[index],
            strlen(rejected_sources[index]), &root) == CM_SOURCE_OK,
            "could not add rejected graph source");
        cm_module_graph_options_init(&graph_options);
        cm_cfg_set_init(&cfg);
        cfg.environment.entries = enabled;
        cfg.environment.entry_count = 1u;
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        check(graph_result.error_count == 0u,
            "rejected fixture did not build a graph");
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph,
            graph_result.revision, &map, &options);
        if (result.error_count != 1u) {
            fprintf(stderr, "accepted rejected graph source: %s",
                rejected_sources[index]);
        }
        check(result.error_count == 1u
            && result.first_error.kind
                == (index + 1u == sizeof(rejected_sources)
                        / sizeof(rejected_sources[0])
                    ? CM_HIR_LOWER_INVALID_IMPL
                    : CM_HIR_LOWER_UNSUPPORTED_ITEM),
            "unsupported active graph construct was accepted");
        check(hir.crates.len == 0u && hir.modules.len == 0u
            && hir.items.len == 0u && hir.bodies.len == 0u
            && hir.expressions.len == 0u && hir.definitions.len == 0u
            && hir.strings.entries.len == 0u
            && cm_hir_module_map_count(&map) == 0u,
            "preflight rejection mutated HIR or the module map");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_safe_extern_c_declarations(void)
{
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    static const char *const block_attributes[] = {
        "allow(improper_ctypes)"
    };
    static const char *const cbrt_attributes[] = {
        "link_name = \"llvm.test.cbrt\""
    };
    static const char *const cbrtf_attributes[] = {
        "link_name = \"llvm.test.cbrtf\""
    };
    static const unsigned char source[] =
        "#[allow(improper_ctypes)]\n"
        "unsafe extern \"C\" {\n"
        "    #[cfg(enabled)] #[link_name = \"llvm.test.cbrt\"]\n"
        "    pub(crate) safe fn cbrt(n: f64) -> f64;\n"
        "    #[cfg(disabled)] pub(crate) safe fn hidden(n: f64) -> f64;\n"
        "    #[link_name = \"llvm.test.cbrtf\"]\n"
        "    pub(crate) fn cbrtf(n: f32) -> f32;\n"
        "}\n"
        "use self::cbrt as cube_root;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveModuleInfo root_information;
    CmResolveEffectiveItem block_view;
    CmResolveEffectiveItem cbrt_view;
    CmResolveEffectiveItem cbrtf_view;
    CmResolveEffectiveAttribute block_attribute;
    CmResolveEffectiveAttribute cbrt_attribute;
    CmResolveEffectiveAttribute cbrtf_attribute;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirModule *root_module;
    const CmHirItem *cbrt;
    const CmHirItem *cbrtf;
    const CmHirFunctionSignature *cbrt_signature;
    const CmHirFunctionSignature *cbrtf_signature;
    const CmHirImport *import_value;
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&root_information, 0, sizeof(root_information));
    memset(&block_view, 0, sizeof(block_view));
    memset(&cbrt_view, 0, sizeof(cbrt_view));
    memset(&cbrtf_view, 0, sizeof(cbrtf_view));
    memset(&block_attribute, 0, sizeof(block_attribute));
    memset(&cbrt_attribute, 0, sizeof(cbrt_attribute));
    memset(&cbrtf_attribute, 0, sizeof(cbrtf_attribute));
    check(cm_source_add_memory(&sources, "extern-c/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add safe extern C declaration fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    if (cm_module_graph_get_module(&graph, graph_result.root,
            &root_information)) {
        for (index = 0u; index < root_information.effective_item_count;
             ++index) {
            CmResolveEffectiveItem effective;

            if (cm_module_graph_get_effective_item(&graph,
                    graph_result.revision, graph_result.root, index,
                    &effective) == CM_RESOLVE_VIEW_OK
                && effective.item_kind == CM_AST_ITEM_EXTERN_BLOCK) {
                block_view = effective;
                break;
            }
        }
    }
    check(graph_result.error_count == 0u
        && block_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && block_view.child_kind == CM_EXPANDED_CHILD_EXTERN_BLOCK
        && block_view.child_count == 2u
        && effective_item_has_source(&block_view, root)
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &block_view, block_attributes, 1u,
            &block_attribute)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &block_view, "cbrt", &cbrt_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &block_view, "cbrtf", &cbrtf_view)
        && !effective_child_named(&graph, graph_result.revision,
            graph_result.root, &block_view, "hidden", &cbrtf_view)
        && effective_item_has_source(&cbrt_view, root)
        && effective_item_has_source(&cbrtf_view, root)
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &cbrt_view, cbrt_attributes, 1u,
            &cbrt_attribute)
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &cbrtf_view, cbrtf_attributes, 1u,
            &cbrtf_attribute),
        "extern block effective children differ from the cfg-active plan");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower extern C: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 2u
        && hir.items.len == 2u && hir.definitions.len == 3u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "safe extern C declarations did not lower as exactly two roots");
    root_module = cm_hir_get_module(&hir, root_hir);
    cbrt = find_hir_item(&hir, root_hir, "cbrt");
    cbrtf = find_hir_item(&hir, root_hir, "cbrtf");
    cbrt_signature = cbrt == NULL || cbrt->kind != CM_HIR_ITEM_FUNCTION
        ? NULL : &cbrt->data.function_item.signature;
    cbrtf_signature = cbrtf == NULL || cbrtf->kind != CM_HIR_ITEM_FUNCTION
        ? NULL : &cbrtf->data.function_item.signature;
    import_value = root_module != NULL && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    check(cbrt != NULL && cbrtf != NULL
        && cm_hir_def_id_is_none(cbrt->parent_definition)
        && cm_hir_def_id_is_none(cbrtf->parent_definition)
        && cbrt->visibility.kind == CM_HIR_VIS_CRATE
        && cbrtf->visibility.kind == CM_HIR_VIS_CRATE
        && cbrt->attribute_count == 1u && cbrtf->attribute_count == 1u
        && hir_attributes_match_graph(&graph, &hir, cbrt,
            &cbrt_attribute, 1u)
        && hir_attributes_match_graph(&graph, &hir, cbrtf,
            &cbrtf_attribute, 1u)
        && cbrt->generic_parameter_count == 0u
        && cbrtf->generic_parameter_count == 0u
        && cbrt->predicate_count == 0u && cbrtf->predicate_count == 0u
        && cbrt->data.function_item.body == CM_HIR_BODY_NONE
        && cbrtf->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_def_id_is_none(
            cbrt->data.function_item.trait_item_definition)
        && cm_hir_def_id_is_none(
            cbrtf->data.function_item.trait_item_definition)
        && cbrt_signature != NULL && cbrtf_signature != NULL
        && hir_name_is(&hir, cbrt_signature->abi, "C")
        && hir_name_is(&hir, cbrtf_signature->abi, "C")
        && cbrt_signature->safety == CM_HIR_SAFE
        && cbrtf_signature->safety == CM_HIR_UNSAFE
        && cbrt_signature->receiver == CM_HIR_RECEIVER_NONE
        && cbrtf_signature->receiver == CM_HIR_RECEIVER_NONE
        && !cbrt_signature->is_const && !cbrtf_signature->is_const
        && !cbrt_signature->is_async && !cbrtf_signature->is_async
        && !cbrt_signature->is_variadic && !cbrtf_signature->is_variadic
        && cbrt_signature->parameter_count == 1u
        && cbrtf_signature->parameter_count == 1u
        && hir_type_is_float(&hir,
            cbrt_signature->parameters[0].type, CM_HIR_FLOAT_F64)
        && hir_type_is_float(&hir,
            cbrt_signature->return_type, CM_HIR_FLOAT_F64)
        && hir_type_is_float(&hir,
            cbrtf_signature->parameters[0].type, CM_HIR_FLOAT_F32)
        && hir_type_is_float(&hir,
            cbrtf_signature->return_type, CM_HIR_FLOAT_F32)
        && source_span_is(&sources, cbrt->span,
            "pub(crate) safe fn cbrt(n: f64) -> f64;")
        && source_span_is(&sources, cbrtf->span,
            "pub(crate) fn cbrtf(n: f32) -> f32;")
        && import_value != NULL && import_value->binding_count == 1u
        && hir_import_binding_is(&hir, import_value, 0u,
            CM_HIR_NAMESPACE_VALUE, "cube_root", cbrt->definition),
        "safe extern C HIR lost ABI, signature, identity, span, or import");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_unadjusted_extern_declaration(void)
{
    static const unsigned char source[] =
        "#[allow(improper_ctypes)]\n"
        "unsafe extern \"unadjusted\" {\n"
        "    #[link_name = \"llvm.x86.rdtsc\"]\n"
        "    fn rdtsc() -> u64;\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *rdtsc;
    const CmHirFunctionSignature *signature;
    const CmHirType *return_type;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "extern-unadjusted/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add unadjusted extern declaration fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower extern unadjusted: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 1u && hir.items.len == 1u
        && hir.definitions.len == 2u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "unadjusted extern declaration did not lower as one root item");
    rdtsc = find_hir_item(&hir, root_hir, "rdtsc");
    signature = rdtsc == NULL || rdtsc->kind != CM_HIR_ITEM_FUNCTION
        ? NULL : &rdtsc->data.function_item.signature;
    return_type = signature == NULL ? NULL
        : cm_hir_get_type(&hir, signature->return_type);
    check(rdtsc != NULL && signature != NULL
        && cm_hir_def_id_is_none(rdtsc->parent_definition)
        && rdtsc->visibility.kind == CM_HIR_VIS_PRIVATE
        && rdtsc->attribute_count == 1u
        && hir_name_is(&hir, rdtsc->attributes[0].metadata,
            "link_name = \"llvm.x86.rdtsc\"")
        && signature->parameter_count == 0u
        && signature->receiver == CM_HIR_RECEIVER_NONE
        && signature->safety == CM_HIR_UNSAFE
        && hir_name_is(&hir, signature->abi, "unadjusted")
        && !signature->is_const && !signature->is_async
        && !signature->is_variadic
        && rdtsc->data.function_item.body == CM_HIR_BODY_NONE
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && return_type->data.integer_type.kind == CM_HIR_INT_U64
        && source_span_is(&sources, rdtsc->span,
            "fn rdtsc() -> u64;"),
        "unadjusted extern HIR lost ABI, safety, attribute, type, or span");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_foreign_type_declaration(void)
{
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    static const unsigned char source[] =
        "unsafe extern \"C\" {\n"
        "    #[doc = \"opaque vtable\"]\n"
        "    #[cfg(enabled)]\n"
        "    type VTable;\n"
        "    #[cfg(disabled)] type Hidden;\n"
        "}\n"
        "use self::VTable as ImportedVTable;\n"
        "pub struct Metadata { ptr: *const ImportedVTable }\n";
    static const char *const vtable_attributes[] = {
        "doc = \"opaque vtable\""
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveModuleInfo root_information;
    CmResolveEffectiveItem block_view;
    CmResolveEffectiveItem vtable_view;
    CmResolveEffectiveItem hidden_view;
    CmResolveEffectiveAttribute vtable_attribute;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirModule *root_module;
    const CmHirItem *vtable;
    const CmHirItem *metadata;
    const CmHirType *pointer;
    const CmHirType *pointee;
    const CmHirImport *import_value;
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&root_information, 0, sizeof(root_information));
    memset(&block_view, 0, sizeof(block_view));
    memset(&vtable_view, 0, sizeof(vtable_view));
    memset(&hidden_view, 0, sizeof(hidden_view));
    memset(&vtable_attribute, 0, sizeof(vtable_attribute));
    check(cm_source_add_memory(&sources, "foreign-type/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add foreign type fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    if (cm_module_graph_get_module(&graph, graph_result.root,
            &root_information)) {
        for (index = 0u; index < root_information.effective_item_count;
             ++index) {
            CmResolveEffectiveItem effective;

            if (cm_module_graph_get_effective_item(&graph,
                    graph_result.revision, graph_result.root, index,
                    &effective) == CM_RESOLVE_VIEW_OK
                && effective.item_kind == CM_AST_ITEM_EXTERN_BLOCK) {
                block_view = effective;
                break;
            }
        }
    }
    check(graph_result.error_count == 0u
        && block_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && block_view.child_kind == CM_EXPANDED_CHILD_EXTERN_BLOCK
        && block_view.child_count == 1u
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &block_view, "VTable", &vtable_view)
        && !effective_child_named(&graph, graph_result.revision,
            graph_result.root, &block_view, "Hidden", &hidden_view)
        && vtable_view.item_kind == CM_AST_ITEM_TYPE_ALIAS
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &vtable_view, vtable_attributes, 1u,
            &vtable_attribute),
        "foreign type effective child lost cfg or attribute provenance");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower foreign type: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 2u
        && hir.items.len == 2u && hir.definitions.len == 3u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "foreign type did not lower as one root definition");
    root_module = cm_hir_get_module(&hir, root_hir);
    vtable = find_hir_item(&hir, root_hir, "VTable");
    metadata = find_hir_item(&hir, root_hir, "Metadata");
    pointer = metadata == NULL || metadata->kind != CM_HIR_ITEM_STRUCT
            || metadata->data.aggregate_item.field_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            metadata->data.aggregate_item.fields[0].type);
    pointee = pointer == NULL
            || pointer->kind != CM_HIR_TYPE_RAW_POINTER_KIND
        ? NULL : cm_hir_get_type(&hir,
            pointer->data.raw_pointer_type.pointee);
    import_value = root_module != NULL && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    check(vtable != NULL && vtable->kind == CM_HIR_ITEM_EXTERN_TYPE
        && cm_hir_def_id_is_none(vtable->parent_definition)
        && vtable->visibility.kind == CM_HIR_VIS_PRIVATE
        && vtable->generic_parameter_count == 0u
        && vtable->predicate_count == 0u
        && vtable->outlives_predicate_count == 0u
        && vtable->attribute_count == 1u
        && hir_attributes_match_graph(&graph, &hir, vtable,
            &vtable_attribute, 1u)
        && source_span_is(&sources, vtable->span, "type VTable;")
        && pointee != NULL && pointee->kind == CM_HIR_TYPE_FOREIGN_KIND
        && pointee->data.named_type.argument_count == 0u
        && cm_hir_def_id_equal(pointee->data.named_type.definition,
            vtable->definition)
        && import_value != NULL && import_value->binding_count == 1u
        && hir_import_binding_is(&hir, import_value, 0u,
            CM_HIR_NAMESPACE_TYPE, "ImportedVTable", vtable->definition),
        "foreign type HIR lost identity, attributes, span, or type binding");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_foreign_type_generic_use_rejection(void)
{
    static const unsigned char source[] =
        "unsafe extern \"C\" { type VTable; }\n"
        "struct Invalid { ptr: *const VTable<u8> }\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "foreign-generic/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add foreign generic-use rejection fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "foreign generic-use rejection fixture did not build a graph");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(result.first_error.message,
            "foreign types do not accept generic arguments") != NULL
        && result.crate_id == CM_HIR_CRATE_NONE
        && result.root_module == CM_HIR_MODULE_NONE
        && result.lowered_item_count == 0u
        && hir.crates.len == 0u && hir.modules.len == 0u
        && hir.items.len == 0u && hir.types.len == 0u
        && hir.definitions.len == 0u
        && cm_interner_length(&hir.strings) == 0u
        && cm_hir_module_map_count(&map) == 0u,
        "foreign generic-use rejection did not rewind HIR transactionally");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_source_const(void)
{
    static const unsigned char source[] =
        "#[doc = \"circle\"] pub const PI: f128 = 3.0_f128;\n";
    static const char *const pi_attributes[] = { "doc = \"circle\"" };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem pi_view;
    CmResolveEffectiveAttribute pi_attribute;
    const CmAst *pi_ast;
    const CmAstItem *pi_declaration;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *pi_item;
    const CmHirType *pi_type;
    const CmHirBody *pi_body;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&pi_view, 0, sizeof(pi_view));
    memset(&pi_attribute, 0, sizeof(pi_attribute));
    pi_ast = NULL;
    pi_declaration = NULL;
    check(cm_source_add_memory(&sources, "source-const/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add source const fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "PI", &pi_view)
        && !pi_view.is_generated
        && pi_view.item_kind == CM_AST_ITEM_CONST
        && pi_view.provenance.source_item.source == root
        && pi_view.provenance.source_item.item == pi_view.declaration.item
        && pi_view.provenance.macro_invocation.item == CM_AST_ITEM_NONE
        && pi_view.provenance.expansion_depth == 0u
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &pi_view, pi_attributes, 1u,
            &pi_attribute)
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            pi_view.declaration, &pi_ast)
        && (pi_declaration = cm_ast_get_item(pi_ast,
            pi_view.declaration.item)) != NULL
        && pi_declaration->kind == CM_AST_ITEM_CONST
        && pi_declaration->data.value_item.type != CM_AST_TYPE_NONE
        && pi_declaration->data.value_item.initializer != CM_AST_EXPR_NONE
        && cm_ast_get_expr(pi_ast,
            pi_declaration->data.value_item.initializer) != NULL,
        "source const graph identity differs");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower source const: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    (void)cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, graph_result.root, &hir, &root_hir);
    pi_item = find_hir_item(&hir, root_hir, "PI");
    pi_type = pi_item == NULL ? NULL
        : cm_hir_get_type(&hir, pi_item->data.value_item.type);
    pi_body = pi_item == NULL ? NULL
        : cm_hir_get_body(&hir, pi_item->data.value_item.body);
    check(result.error_count == 0u && result.lowered_item_count == 1u
        && hir.items.len == 1u && hir.definitions.len == 2u
        && hir.bodies.len == 1u && pi_item != NULL,
        "source const did not lower exactly once");
    check(pi_item != NULL && pi_declaration != NULL
        && pi_item->kind == CM_HIR_ITEM_CONST
        && pi_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(pi_item->parent_definition)
        && pi_item->data.value_item.mutability == CM_HIR_IMMUTABLE
        && hir_attributes_match_graph(&graph, &hir, pi_item,
            &pi_attribute, 1u)
        && pi_type != NULL && pi_type->kind == CM_HIR_TYPE_FLOAT_KIND
        && pi_type->data.float_type.kind == CM_HIR_FLOAT_F128
        && pi_body != NULL && pi_body->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_def_id_equal(pi_body->owner, pi_item->definition)
        && pi_body->expected_type == pi_item->data.value_item.type
        && pi_body->source == root
        && pi_body->source_expression_id
            == pi_declaration->data.value_item.initializer
        && pi_body->span.source == pi_view.span.source
        && pi_body->span.start == pi_view.span.start
        && pi_body->span.end == pi_view.span.end,
        "source const lost type, metadata, or initializer identity");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_trait_associated_const_declarations(void)
{
    static const unsigned char source[] =
        "#[doc = \"integers\"] pub trait Integer {\n"
        "  #[doc = \"zero\"] const ZERO: Self;\n"
        "  const ONE: Self = loop {};\n"
        "}\n";
    static const char *const zero_attributes[] = { "doc = \"zero\"" };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem trait_view;
    CmResolveEffectiveItem zero_view;
    CmResolveEffectiveItem one_view;
    CmResolveEffectiveAttribute zero_attribute;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *zero_item;
    const CmHirItem *one_item;
    const CmHirType *zero_type;
    const CmHirType *one_type;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&trait_view, 0, sizeof(trait_view));
    memset(&zero_view, 0, sizeof(zero_view));
    memset(&one_view, 0, sizeof(one_view));
    memset(&zero_attribute, 0, sizeof(zero_attribute));
    check(cm_source_add_memory(&sources, "trait-const/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add trait associated const fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "Integer", &trait_view)
        && trait_view.item_kind == CM_AST_ITEM_TRAIT
        && trait_view.child_kind == CM_EXPANDED_CHILD_TRAIT
        && trait_view.child_count == 2u
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "ZERO", &zero_view)
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &trait_view, "ONE", &one_view)
        && !zero_view.is_generated && !one_view.is_generated
        && zero_view.item_kind == CM_AST_ITEM_CONST
        && one_view.item_kind == CM_AST_ITEM_CONST
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &zero_view, zero_attributes, 1u,
            &zero_attribute),
        "trait associated const graph topology differs");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower trait const: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "Integer");
    zero_item = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_CONST, "ZERO");
    one_item = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_CONST, "ONE");
    zero_type = zero_item == NULL ? NULL
        : cm_hir_get_type(&hir, zero_item->data.value_item.type);
    one_type = one_item == NULL ? NULL
        : cm_hir_get_type(&hir, one_item->data.value_item.type);
    check(result.error_count == 0u && result.lowered_item_count == 3u
        && hir.items.len == 3u && hir.definitions.len == 4u
        && hir.bodies.len == 1u && trait_item != NULL
        && zero_item != NULL && one_item != NULL,
        "trait associated const declarations did not lower exactly once");
    check(trait_item != NULL && zero_item != NULL && one_item != NULL
        && trait_item->kind == CM_HIR_ITEM_TRAIT
        && zero_item->kind == CM_HIR_ITEM_CONST
        && one_item->kind == CM_HIR_ITEM_CONST
        && cm_hir_def_id_equal(zero_item->parent_definition,
            trait_item->definition)
        && cm_hir_def_id_equal(one_item->parent_definition,
            trait_item->definition)
        && zero_item->visibility.kind == CM_HIR_VIS_PRIVATE
        && one_item->visibility.kind == CM_HIR_VIS_PRIVATE
        && zero_item->data.value_item.mutability == CM_HIR_IMMUTABLE
        && one_item->data.value_item.mutability == CM_HIR_IMMUTABLE
        && zero_item->data.value_item.body == CM_HIR_BODY_NONE
        && one_item->data.value_item.body != CM_HIR_BODY_NONE
        && hir_attributes_match_graph(&graph, &hir, zero_item,
            &zero_attribute, 1u)
        && zero_type != NULL && zero_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(zero_type->data.self_type.owner,
            trait_item->definition)
        && one_type != NULL && one_type->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(one_type->data.self_type.owner,
            trait_item->definition),
        "trait associated const lost parent, type, metadata, or targetless state");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_trait_impl_consts(void)
{
    static const unsigned char source[] =
        "trait Integer { const ZERO: Self; const ONE: Self; "
        "const DEFAULT: Self = loop {}; }\n"
        "macro_rules! int { ($($ty:ty),+) => { $(\n"
        "impl Integer for $ty {\n"
        "const ZERO: Self = 0; const ONE: Self = 1;\n"
        "}\n"
        ")+ } }\n"
        "int!(u16, u32, u64);\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *zero_declaration;
    const CmHirItem *one_declaration;
    const CmHirItem *default_declaration;
    size_t index;
    size_t impl_count;
    size_t zero_count;
    size_t one_count;
    int definitions_valid;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "generated-trait-const/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generated trait impl const fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generated trait const: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "Integer");
    zero_declaration = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_CONST, "ZERO");
    one_declaration = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_CONST, "ONE");
    default_declaration = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_CONST, "DEFAULT");
    impl_count = 0u;
    zero_count = 0u;
    one_count = 0u;
    definitions_valid = 1;
    for (index = 0u; index < hir.items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir.items, index);
        if (item != NULL && item->kind == CM_HIR_ITEM_IMPL) {
            impl_count += 1u;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_CONST
            && item->data.value_item.body != CM_HIR_BODY_NONE
            && !cm_hir_def_id_is_none(
                item->data.value_item.trait_item_definition)) {
            const CmHirType *type;
            const CmHirBody *body;

            type = cm_hir_get_type(&hir, item->data.value_item.type);
            body = cm_hir_get_body(&hir, item->data.value_item.body);
            definitions_valid = definitions_valid
                && type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND
                && cm_hir_def_id_equal(type->data.self_type.owner,
                    item->parent_definition)
                && body != NULL
                && cm_hir_def_id_equal(body->owner, item->definition);
            if (zero_declaration != NULL
                && cm_hir_def_id_equal(item->data.value_item
                        .trait_item_definition,
                    zero_declaration->definition)) {
                zero_count += 1u;
            } else if (one_declaration != NULL
                && cm_hir_def_id_equal(item->data.value_item
                        .trait_item_definition,
                    one_declaration->definition)) {
                one_count += 1u;
            } else {
                definitions_valid = 0;
            }
        }
    }
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 13u && hir.items.len == 13u
        && hir.bodies.len == 7u && trait_item != NULL
        && zero_declaration != NULL && one_declaration != NULL
        && default_declaration != NULL
        && default_declaration->data.value_item.body != CM_HIR_BODY_NONE
        && impl_count == 3u && zero_count == 3u && one_count == 3u
        && definitions_valid,
        "generated trait impl consts lost declaration links, Self, or bodies");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_source_static_with_named_array_length(void)
{
    static const unsigned char source[] =
        "const N_POWERS_OF_FIVE: usize = 1usize;\n"
        "use self::N_POWERS_OF_FIVE as LEN;\n"
        "#[doc = \"powers\"] pub static POWER_OF_FIVE_128: "
        "[(u64, u64); LEN] = [(1u64, 1u64)];\n";
    static const char *const power_attributes[] = { "doc = \"powers\"" };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem length_view;
    CmResolveEffectiveItem power_view;
    CmResolveEffectiveAttribute power_attribute;
    const CmAst *power_ast;
    const CmAstItem *length_declaration;
    const CmAstItem *power_declaration;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *length_item;
    const CmHirItem *power_item;
    const CmHirType *array_type;
    const CmHirType *tuple_type;
    const CmHirType *first_element;
    const CmHirType *second_element;
    const CmHirBody *power_body;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&length_view, 0, sizeof(length_view));
    memset(&power_view, 0, sizeof(power_view));
    memset(&power_attribute, 0, sizeof(power_attribute));
    power_ast = NULL;
    length_declaration = NULL;
    power_declaration = NULL;
    check(cm_source_add_memory(&sources, "source-static/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add source static fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "N_POWERS_OF_FIVE", &length_view)
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "POWER_OF_FIVE_128", &power_view)
        && !length_view.is_generated && !power_view.is_generated
        && length_view.item_kind == CM_AST_ITEM_CONST
        && power_view.item_kind == CM_AST_ITEM_STATIC
        && length_view.provenance.source_item.source == root
        && power_view.provenance.source_item.source == root
        && length_view.provenance.expansion_depth == 0u
        && power_view.provenance.expansion_depth == 0u
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &power_view, power_attributes, 1u,
            &power_attribute)
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            power_view.declaration, &power_ast)
        && (length_declaration = cm_ast_get_item(power_ast,
            length_view.declaration.item)) != NULL
        && (power_declaration = cm_ast_get_item(power_ast,
            power_view.declaration.item)) != NULL
        && length_declaration->kind == CM_AST_ITEM_CONST
        && power_declaration->kind == CM_AST_ITEM_STATIC
        && !power_declaration->data.value_item.is_mutable
        && power_declaration->data.value_item.type != CM_AST_TYPE_NONE
        && power_declaration->data.value_item.initializer != CM_AST_EXPR_NONE
        && cm_ast_get_expr(power_ast,
            power_declaration->data.value_item.initializer) != NULL,
        "source static graph identity differs");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower source static: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    (void)cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, graph_result.root, &hir, &root_hir);
    length_item = find_hir_item(&hir, root_hir, "N_POWERS_OF_FIVE");
    power_item = find_hir_item(&hir, root_hir, "POWER_OF_FIVE_128");
    array_type = power_item == NULL ? NULL
        : cm_hir_get_type(&hir, power_item->data.value_item.type);
    tuple_type = array_type == NULL
            || array_type->kind != CM_HIR_TYPE_ARRAY_KIND
        ? NULL : cm_hir_get_type(&hir, array_type->data.array_type.element);
    first_element = tuple_type == NULL
            || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
            || tuple_type->data.tuple_type.element_count != 2u
        ? NULL : cm_hir_get_type(&hir,
            tuple_type->data.tuple_type.elements[0]);
    second_element = tuple_type == NULL
            || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
            || tuple_type->data.tuple_type.element_count != 2u
        ? NULL : cm_hir_get_type(&hir,
            tuple_type->data.tuple_type.elements[1]);
    power_body = power_item == NULL ? NULL
        : cm_hir_get_body(&hir, power_item->data.value_item.body);
    check(result.error_count == 0u && result.lowered_item_count == 2u
        && hir.items.len == 2u && hir.definitions.len == 3u
        && hir.bodies.len == 2u && length_item != NULL
        && power_item != NULL,
        "source static and its length const did not lower exactly once");
    check(length_item != NULL && power_item != NULL
        && power_declaration != NULL
        && length_item->kind == CM_HIR_ITEM_CONST
        && power_item->kind == CM_HIR_ITEM_STATIC
        && power_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(power_item->parent_definition)
        && power_item->data.value_item.mutability == CM_HIR_IMMUTABLE
        && hir_attributes_match_graph(&graph, &hir, power_item,
            &power_attribute, 1u)
        && array_type != NULL && array_type->kind == CM_HIR_TYPE_ARRAY_KIND
        && array_type->data.array_type.length.kind
            == CM_HIR_CONST_UNEVALUATED
        && cm_hir_def_id_equal(
            array_type->data.array_type.length.data.definition,
            length_item->definition)
        && hir_type_is_usize(&hir,
            array_type->data.array_type.length.type)
        && tuple_type != NULL && tuple_type->kind == CM_HIR_TYPE_TUPLE_KIND
        && first_element != NULL
        && first_element->kind == CM_HIR_TYPE_INTEGER_KIND
        && first_element->data.integer_type.kind == CM_HIR_INT_U64
        && second_element != NULL
        && second_element->kind == CM_HIR_TYPE_INTEGER_KIND
        && second_element->data.integer_type.kind == CM_HIR_INT_U64
        && power_body != NULL
        && power_body->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_def_id_equal(power_body->owner,
            power_item->definition)
        && power_body->expected_type == power_item->data.value_item.type
        && power_body->source == root
        && power_body->source_expression_id
            == power_declaration->data.value_item.initializer
        && power_body->span.source == power_view.span.source
        && power_body->span.start == power_view.span.start
        && power_body->span.end == power_view.span.end,
        "source static lost type, metadata, const length, or body identity");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_enum_variant_glob_import(void)
{
    static const unsigned char source[] =
        "pub enum Choice { Unit, Tuple(u8), Named { value: u8 } }\n"
        "use Choice::*;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirModule *root_module;
    const CmHirItem *choice;
    const CmHirImport *glob;
    const CmHirDefinition *choice_definition;
    const CmHirDefinition *variant_definition;
    CmHirDefId unit;
    CmHirDefId tuple;
    CmHirDefId named;
    char *dump;
    uint32_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "variant-glob/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add enum variant glob fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower enum variant glob: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    (void)cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, graph_result.root, &hir, &root_hir);
    root_module = cm_hir_get_module(&hir, root_hir);
    choice = find_hir_item(&hir, root_hir, "Choice");
    glob = root_module != NULL && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    unit = choice == NULL || choice->data.enum_item.variant_count != 3u
        ? cm_hir_def_id_none()
        : choice->data.enum_item.variants[0].definition;
    tuple = choice == NULL || choice->data.enum_item.variant_count != 3u
        ? cm_hir_def_id_none()
        : choice->data.enum_item.variants[1].definition;
    named = choice == NULL || choice->data.enum_item.variant_count != 3u
        ? cm_hir_def_id_none()
        : choice->data.enum_item.variants[2].definition;
    choice_definition = choice == NULL ? NULL
        : cm_hir_lookup_definition(&hir, choice->definition);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 1u && hir.items.len == 1u
        && hir.definitions.len == 5u && choice != NULL
        && choice->kind == CM_HIR_ITEM_ENUM
        && choice->data.enum_item.variant_count == 3u
        && choice_definition != NULL
        && choice_definition->kind == CM_HIR_DEFINITION_ITEM
        && choice_definition->state == CM_HIR_DEFINITION_BOUND
        && root_module != NULL && glob != NULL
        && glob->binding_count == 5u,
        "enum variants did not retain distinct canonical definitions");
    if (choice != NULL && choice->data.enum_item.variant_count == 3u
        && choice_definition != NULL) {
        for (index = 0u; index < 3u; ++index) {
            variant_definition = cm_hir_lookup_definition(&hir,
                choice->data.enum_item.variants[index].definition);
            check(variant_definition != NULL
                && variant_definition->kind
                    == CM_HIR_DEFINITION_ENUM_VARIANT
                && variant_definition->state == CM_HIR_DEFINITION_BOUND
                && variant_definition->entity.enum_variant.enum_item_id
                    == choice_definition->entity.item_id
                && variant_definition->entity.enum_variant.variant_index
                    == index
                && !cm_hir_def_id_equal(variant_definition->id,
                    choice->definition),
                "enum variant definition lost parent/index identity");
        }
    }
    check(glob != NULL
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_TYPE, "Unit", unit) == 1u
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_VALUE, "Unit", unit) == 1u
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_TYPE, "Tuple", tuple) == 1u
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_VALUE, "Tuple", tuple) == 1u
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_TYPE, "Named", named) == 1u
        && hir_import_binding_match_count(&hir, glob,
            CM_HIR_NAMESPACE_VALUE, "Named", named) == 0u,
        "enum variant glob bindings lost namespace-specific identities");
    dump = dump_hir(&hir);
    check(dump != NULL
        && strncmp(dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(dump, "enum-variant bound enum-item#1 variant=0")
            != NULL,
        "hir-v27 dump omitted canonical enum variant identity");
    free(dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_macro_import_identity(void)
{
    static const unsigned char source[] =
        "#[macro_export] macro_rules! Copy { () => {}; }\n"
        "use crate::Copy as CopyA;\n"
        "use crate::Copy as CopyB;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirModule *root_module;
    const CmHirImportBinding *first_binding;
    const CmHirImportBinding *second_binding;
    const CmHirDefinition *definition;
    char *dump;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "macro-import/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add macro import identity fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower macro import: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    (void)cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, graph_result.root, &hir, &root_hir);
    root_module = cm_hir_get_module(&hir, root_hir);
    first_binding = root_module != NULL && root_module->import_count == 2u
            && root_module->imports[0].binding_count == 1u
        ? &root_module->imports[0].bindings[0] : NULL;
    second_binding = root_module != NULL && root_module->import_count == 2u
            && root_module->imports[1].binding_count == 1u
        ? &root_module->imports[1].bindings[0] : NULL;
    definition = first_binding == NULL ? NULL
        : cm_hir_lookup_definition(&hir, first_binding->target);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 0u && hir.items.len == 0u
        && hir.definitions.len == 2u && root_module != NULL
        && first_binding != NULL && second_binding != NULL
        && first_binding->namespace_kind == CM_HIR_NAMESPACE_MACRO
        && second_binding->namespace_kind == CM_HIR_NAMESPACE_MACRO
        && hir_name_is(&hir, first_binding->name, "CopyA")
        && hir_name_is(&hir, second_binding->name, "CopyB")
        && cm_hir_def_id_equal(first_binding->target,
            second_binding->target),
        "macro aliases did not share one canonical HIR definition");
    check(definition != NULL
        && definition->kind == CM_HIR_DEFINITION_MACRO
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->entity.macro_definition.owner_module == root_hir
        && definition->entity.macro_definition.form
            == CM_HIR_MACRO_RULES_DEFINITION
        && hir_name_is(&hir, definition->entity.macro_definition.name,
            "Copy"),
        "macro definition lost owner, form, or declared name identity");
    dump = dump_hir(&hir);
    check(dump != NULL
        && strstr(dump,
            "macro bound module#1 form=macro-rules name=\"Copy\"")
            != NULL,
        "canonical HIR dump omitted imported macro identity");
    free(dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_typed_const_generic_declarations(void)
{
    static const unsigned char source[] =
        "pub enum AtomicOrdering { Relaxed }\n"
        "#[rustc_intrinsic]\n"
        "pub unsafe fn atomic_cxchg<T,\n"
        "    const ORD_SUCC: AtomicOrdering,\n"
        "    const ORD_FAIL: AtomicOrdering,\n"
        ">(dst: *mut T, old: T, src: T) -> (T, bool);\n"
        "struct Assume;\n"
        "impl Assume { const NOTHING: Assume = Assume; }\n"
        "unsafe trait TransmuteFrom<Src,\n"
        "    const ASSUME: Assume = { Assume::NOTHING },\n"
        "> where Src: ?Sized {}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *ordering;
    const CmHirItem *function;
    const CmHirGenericParam *success;
    const CmHirGenericParam *failure;
    const CmHirItem *assume;
    const CmHirItem *assume_impl;
    const CmHirItem *nothing;
    const CmHirItem *transmute;
    const CmHirGenericParam *source_parameter;
    const CmHirGenericParam *assumption_parameter;
    const CmHirType *success_type;
    const CmHirType *failure_type;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "const-generics/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add typed const generic fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower typed const generics: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    (void)cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, graph_result.root, &hir, &root_hir);
    ordering = find_hir_item(&hir, root_hir, "AtomicOrdering");
    function = find_hir_item(&hir, root_hir, "atomic_cxchg");
    success = function == NULL || function->generic_parameter_count != 3u
        ? NULL : cm_hir_get_generic_param(&hir,
            function->generic_parameter_start + 1u);
    failure = function == NULL || function->generic_parameter_count != 3u
        ? NULL : cm_hir_get_generic_param(&hir,
            function->generic_parameter_start + 2u);
    success_type = success == NULL ? NULL
        : cm_hir_get_type(&hir, success->declared_type);
    failure_type = failure == NULL ? NULL
        : cm_hir_get_type(&hir, failure->declared_type);
    assume = find_hir_item(&hir, root_hir, "Assume");
    assume_impl = assume == NULL ? NULL
        : find_hir_impl_for(&hir, assume->definition);
    nothing = assume_impl == NULL ? NULL
        : find_hir_associated_item(&hir, assume_impl->definition,
            CM_HIR_ITEM_CONST, "NOTHING");
    transmute = find_hir_item(&hir, root_hir, "TransmuteFrom");
    source_parameter = transmute == NULL
            || transmute->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&hir,
            transmute->generic_parameter_start);
    assumption_parameter = transmute == NULL
            || transmute->generic_parameter_count != 2u
        ? NULL : cm_hir_get_generic_param(&hir,
            transmute->generic_parameter_start + 1u);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && ordering != NULL && ordering->kind == CM_HIR_ITEM_ENUM
        && function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->generic_parameter_count == 3u
        && success != NULL && success->kind == CM_HIR_GENERIC_CONST
        && success->index == 1u
        && hir_name_is(&hir, success->name, "ORD_SUCC")
        && failure != NULL && failure->kind == CM_HIR_GENERIC_CONST
        && failure->index == 2u
        && hir_name_is(&hir, failure->name, "ORD_FAIL")
        && success_type != NULL
        && success_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(success_type->data.named_type.definition,
            ordering->definition)
        && failure_type != NULL
        && failure_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(failure_type->data.named_type.definition,
            ordering->definition)
        && assume != NULL && assume->kind == CM_HIR_ITEM_STRUCT
        && nothing != NULL && nothing->kind == CM_HIR_ITEM_CONST
        && transmute != NULL && transmute->kind == CM_HIR_ITEM_TRAIT
        && transmute->predicate_count == 0u
        && source_parameter != NULL
        && source_parameter->kind == CM_HIR_GENERIC_TYPE
        && source_parameter->is_relaxed_sized
        && assumption_parameter != NULL
        && assumption_parameter->kind == CM_HIR_GENERIC_CONST
        && assumption_parameter->has_default
        && assumption_parameter->default_argument.kind
            == CM_HIR_GENERIC_ARG_CONST
        && assumption_parameter->default_argument.data.constant.kind
            == CM_HIR_CONST_UNEVALUATED
        && assumption_parameter->default_argument.data.constant.type
            == assumption_parameter->declared_type
        && cm_hir_def_id_equal(
            assumption_parameter->default_argument.data.constant
                .data.definition,
            nothing->definition),
        "typed const generic declarations lost kind, order, or nominal type");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_const_generic_trait_method_declaration(void)
{
    static const unsigned char source[] =
        "trait Clone {}\n"
        "trait SpecArrayClone: Clone {\n"
        "fn clone<const N: usize>(array: &[Self; N]) -> [Self; N];\n"
        "}\n";
    static const char *const rejected[] = {
        "trait Bad { fn clone<const N: u8>(array: [u8; N]); }"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_TYPE
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *method;
    const CmHirGenericParam *parameter;
    const CmHirType *declared_type;
    const CmHirType *input_reference;
    const CmHirType *input_array;
    const CmHirType *output_array;
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "const-method/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add const-generic trait method fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower const-generic method: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    owner = find_hir_item_anywhere(&hir, "SpecArrayClone");
    method = owner == NULL ? NULL : find_hir_associated_item(&hir,
        owner->definition, CM_HIR_ITEM_FUNCTION, "clone");
    parameter = method == NULL || method->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&hir,
            method->generic_parameter_start);
    declared_type = parameter == NULL ? NULL
        : cm_hir_get_type(&hir, parameter->declared_type);
    input_reference = method == NULL
            || method->data.function_item.signature.parameter_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            method->data.function_item.signature.parameters[0].type);
    input_array = input_reference == NULL
            || input_reference->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&hir,
            input_reference->data.reference_type.pointee);
    output_array = method == NULL ? NULL : cm_hir_get_type(&hir,
        method->data.function_item.signature.return_type);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && owner != NULL && owner->kind == CM_HIR_ITEM_TRAIT
        && method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
        && method->data.function_item.body == CM_HIR_BODY_NONE
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST
        && parameter->index == 0u && !parameter->has_default
        && cm_hir_def_id_equal(parameter->owner, method->definition)
        && declared_type != NULL
        && declared_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && declared_type->data.integer_type.kind == CM_HIR_INT_USIZE
        && input_array != NULL && input_array->kind == CM_HIR_TYPE_ARRAY_KIND
        && input_array->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && input_array->data.array_type.length.type
            == parameter->declared_type
        && input_array->data.array_type.length.data.parameter
            == method->generic_parameter_start
        && output_array != NULL
        && output_array->kind == CM_HIR_TYPE_ARRAY_KIND
        && output_array->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && output_array->data.array_type.length.type
            == parameter->declared_type
        && output_array->data.array_type.length.data.parameter
            == method->generic_parameter_start,
        "const-generic trait method lost its typed declaration or uses");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "const-method-bad/lib.rs",
            (const unsigned char *)rejected[index],
            strlen(rejected[index]), &root) == CM_SOURCE_OK,
            "could not add rejected const-generic method fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u && result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
            "unsupported const-generic trait method did not fail closed");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_attributed_import_is_structural(void)
{
    static const unsigned char source[] =
        "mod a { pub struct A; } "
        "#[allow(dead_code)] use crate::a::A;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmModuleId child_graph;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId child_hir;
    const CmHirModule *root_module;
    const CmHirModule *child_module;
    const CmHirImport *import_value;
    const CmHirItem *a_item;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "attributed-import/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add attributed import source");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    child_graph = CM_MODULE_NONE;
    check(graph_result.error_count == 0u
        && graph_module_named(&graph, "a", &child_graph),
        "attributed import graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    root_hir = CM_HIR_MODULE_NONE;
    child_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 2u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, child_graph, &hir, &child_hir)
            == CM_HIR_MODULE_MAP_OK,
        "attributed import did not lower structurally");
    root_module = cm_hir_get_module(&hir, root_hir);
    child_module = cm_hir_get_module(&hir, child_hir);
    a_item = find_hir_item(&hir, child_hir, "A");
    import_value = root_module != NULL && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    check(root_module != NULL && child_module != NULL && a_item != NULL
        && root_module->import_count == 1u
        && child_module->import_count == 0u
        && import_value != NULL
        && hir_name_is(&hir, import_value->tree, "crate::a::A")
        && import_value->visibility.kind == CM_HIR_VIS_PRIVATE
        && import_value->attribute_count == 1u
        && import_value->attributes != NULL
        && hir_name_is(&hir, import_value->attributes[0].metadata,
            "allow(dead_code)")
        && import_value->binding_count == 2u
        && hir_import_binding_is(&hir, import_value, 0u,
            CM_HIR_NAMESPACE_TYPE, "A", a_item->definition)
        && hir_import_binding_is(&hir, import_value, 1u,
            CM_HIR_NAMESPACE_VALUE, "A", a_item->definition),
        "attributed import lost metadata or its resolved binding");
    check(hir.items.len == 1u && hir.definitions.len == 3u,
        "attributed import invented an item or definition");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_separate_namespaces(void)
{
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    static const unsigned char source[] =
        "#[cfg(enabled)] type Same = u8; fn Same();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "names/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add namespace fixture");
    cm_module_graph_options_init(&graph_options);
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "namespace fixture graph failed");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision,
        &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower namespace: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u && hir.items.len == 2u,
        "same spelling in separate namespaces was rejected");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_imported_trait_projection(void)
{
    static const unsigned char source[] =
        "mod defs { pub trait Trait { type Assoc; } }"
        "mod exports { pub use crate::defs::Trait as Step; }"
        "use crate::exports::Step as Imported;"
        "type Concrete = Project<u32>;"
        "type Project<T> = <T as Imported>::Assoc;";
    static const char *const rejected[] = {
        "trait Trait { type Other; } type P<T> = <T as Trait>::Assoc;"
    };
    static const CmCfgEntry enabled[] = {
        { "enabled", NULL }
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmModuleId defs_graph;
    CmHirModuleId root_hir;
    CmHirModuleId defs_hir;
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;
    const CmHirItem *project_item;
    const CmHirItem *concrete_item;
    const CmHirType *projection;
    const CmHirType *self_type;
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "projection/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add projection graph source");
    cm_cfg_set_init(&cfg);
    cfg.environment.entries = enabled;
    cfg.environment.entry_count = 1u;
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && graph_module_named(&graph, "defs", &defs_graph),
        "projection graph did not build expected modules");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower projection: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, defs_graph, &hir, &defs_hir)
            == CM_HIR_MODULE_MAP_OK,
        "projection graph did not lower or map modules");
    trait_item = find_hir_item(&hir, defs_hir, "Trait");
    associated_item = find_hir_item(&hir, defs_hir, "Assoc");
    project_item = find_hir_item(&hir, root_hir, "Project");
    concrete_item = find_hir_item(&hir, root_hir, "Concrete");
    projection = concrete_item == NULL ? NULL : cm_hir_get_type(&hir,
        concrete_item->data.type_alias_item.target);
    self_type = projection == NULL ? NULL : cm_hir_get_type(&hir,
        projection->data.projection_type.self_type);
    check(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && associated_item != NULL
        && associated_item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && cm_hir_def_id_equal(associated_item->parent_definition,
            trait_item->definition)
        && project_item != NULL && concrete_item != NULL
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            projection->data.projection_type.trait_type.definition,
            trait_item->definition)
        && cm_hir_def_id_equal(
            projection->data.projection_type.associated_type.definition,
            associated_item->definition)
        && self_type != NULL
        && self_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && self_type->data.integer_type.kind == CM_HIR_INT_U32,
        "imported or chained-reexported trait projection was not preserved");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "projection-reject/lib.rs",
            (const unsigned char *)rejected[index], strlen(rejected[index]),
            &root) == CM_SOURCE_OK,
            "could not add rejected projection graph source");
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        check(graph_result.error_count == 0u,
            "rejected projection fixture did not build a graph");
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(result.error_count == 1u && hir_is_empty(&hir)
            && cm_hir_module_map_count(&map) == 0u,
            "rejected projection graph mutated HIR or module map");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_cross_trait_projection_default_prebinding(void)
{
    static const unsigned char source[] =
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Residual; } "
        "trait Base<Rhs = Self> {} trait Child: Base {} "
        "trait PrimitiveChild: PrimitiveBase {} "
        "trait PrimitiveBase<Rhs = u8> {}";
    static const char *const rejected[] = {
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Other; }",
        "trait Bad<T = T> {}",
        "trait FromResidual<R = <Self as Try>::Residual> {} "
        "trait Try { type Residual = u8; }",
        "trait T { type A: Base; } "
        "trait Base<X = (Self, u8)> {}",
        "trait Base<Rhs = Self> {} trait Owner { "
        "type Assoc<T>: Base; }",
        "trait Base<Rhs = Self> {} trait Owner<'a> { "
        "type Assoc<'b>: 'a + Base; }",
        "trait Base<T = &u8> {} trait Owner { type Assoc: Base; }",
        "trait Base<T = _> {} trait Child: Base {}",
        "trait Base<T = &u8> {} trait Child: Base {}",
        "trait Base<'a> {} trait Child<'b>: Base<'_> {}",
        "trait Base<T = Self> { type A; } "
        "trait Child: Base<Missing = ()> {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNRESOLVED_PATH
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *try_item;
    const CmHirItem *residual_item;
    const CmHirItem *from_residual_item;
    const CmHirItem *base_item;
    const CmHirItem *child_item;
    const CmHirItem *primitive_child_item;
    const CmHirGenericParam *parameter;
    const CmHirGenericArg *supertrait_argument;
    const CmHirGenericArg *primitive_argument;
    const CmHirType *primitive_type;
    size_t index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "projection-default/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add cross-trait projection default fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision,
        &map, &options);
    try_item = find_hir_item_anywhere(&hir, "Try");
    residual_item = try_item == NULL ? NULL
        : find_hir_associated_item(&hir, try_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Residual");
    from_residual_item = find_hir_item_anywhere(&hir, "FromResidual");
    parameter = from_residual_item == NULL
            || from_residual_item->generic_parameter_count != 1u
        ? NULL : cm_hir_get_generic_param(&hir,
            from_residual_item->generic_parameter_start);
    base_item = find_hir_item_anywhere(&hir, "Base");
    child_item = find_hir_item_anywhere(&hir, "Child");
    supertrait_argument = child_item == NULL
            || child_item->data.trait_item.supertrait_count != 1u
            || child_item->data.trait_item.supertraits[0]
                .trait_type.argument_count != 1u
        ? NULL : &child_item->data.trait_item.supertraits[0]
            .trait_type.arguments[0];
    primitive_child_item = find_hir_item_anywhere(&hir,
        "PrimitiveChild");
    primitive_argument = primitive_child_item == NULL
            || primitive_child_item->data.trait_item.supertrait_count != 1u
            || primitive_child_item->data.trait_item.supertraits[0]
                .trait_type.argument_count != 1u
        ? NULL : &primitive_child_item->data.trait_item.supertraits[0]
            .trait_type.arguments[0];
    primitive_type = primitive_argument == NULL
            || primitive_argument->kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir, primitive_argument->data.type);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 7u && hir.items.len == 7u
        && try_item != NULL && residual_item != NULL
        && from_residual_item != NULL && parameter != NULL
        && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && hir_type_is_projection(&hir,
            parameter->default_argument.data.type,
            from_residual_item->definition, try_item->definition,
            residual_item->definition)
        && base_item != NULL && child_item != NULL
        && cm_hir_def_id_equal(child_item->data.trait_item.supertraits[0]
                .trait_type.definition,
            base_item->definition)
        && supertrait_argument != NULL
        && supertrait_argument->kind == CM_HIR_GENERIC_ARG_TYPE
        && hir_type_is_self(&hir, supertrait_argument->data.type,
            child_item->definition)
        && primitive_type != NULL
        && primitive_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && primitive_type->data.integer_type.kind == CM_HIR_INT_U8,
        "forward cross-trait projection default was not prebound once");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
         ++index) {
        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "projection-default-reject/lib.rs",
            (const unsigned char *)rejected[index],
            strlen(rejected[index]), &root) == CM_SOURCE_OK,
            "could not add rejected projection default fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u
            && result.error_count == 1u
            && result.first_error.kind == rejected_kinds[index]
            && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
            "invalid projection default did not roll back transactionally");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_imported_trait_impl_selection(void)
{
    static const unsigned char source[] =
        "mod defs { pub trait T { type A; } pub struct O; }"
        "mod exports { pub use crate::defs::T as Step; }"
        "mod implementations {"
        " use crate::defs::O;"
        " use crate::exports::Step as Imported;"
        " impl Imported for u8 { type A = O; }"
        "}"
        "type P = <u8 as exports::Step>::A;";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *output_item;
    const CmHirItem *impl_item;
    const CmHirItem *projection_item;
    const CmHirType *projection;
    const CmHirType *selected;
    CmHirProjectionResult selection;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "impl-selection/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add imported impl-selection source");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "imported impl-selection graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower impl selection: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "T");
    output_item = find_hir_item_anywhere(&hir, "O");
    impl_item = find_hir_impl(&hir);
    projection_item = find_hir_item_anywhere(&hir, "P");
    projection = projection_item == NULL ? NULL : cm_hir_get_type(&hir,
        projection_item->data.type_alias_item.target);
    if (projection_item == NULL) {
        memset(&selection, 0, sizeof(selection));
        selection.status = CM_HIR_PROJECTION_INVALID_ASSOCIATION;
        selection.target = CM_HIR_TYPE_NONE;
        selection.impl_definition = cm_hir_def_id_none();
        selection.impl_associated_definition = cm_hir_def_id_none();
    } else {
        selection = cm_hir_select_projection(&hir,
            projection_item->definition.crate_id,
            projection_item->data.type_alias_item.target);
    }
    selected = selection.status == CM_HIR_PROJECTION_SELECTED
        ? cm_hir_get_type(&hir, selection.target) : NULL;
    check(result.error_count == 0u && result.lowered_item_count == 9u
        && trait_item != NULL && output_item != NULL && impl_item != NULL
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(
            impl_item->data.impl_item.trait_type.definition,
            trait_item->definition)
        && selection.status == CM_HIR_PROJECTION_SELECTED
        && selected != NULL && selected->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(selected->data.named_type.definition,
            output_item->definition),
        "imported trait impl did not preserve identity or select exactly");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_ordered_generic_impl_selection(void)
{
    static const unsigned char source[] =
        "struct Wrapper<T>;"
        "trait Trait { type Assoc; }"
        "impl<T> Trait for Wrapper<T> { type Assoc = T; }"
        "type P = <Wrapper<u8> as Trait>::Assoc;";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *wrapper_item;
    const CmHirItem *trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *projection_item;
    const CmHirType *impl_self;
    const CmHirType *selected;
    CmHirProjectionResult selection;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "generic-impl/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add ordered generic impl source");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "ordered generic impl graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generic impl: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 6u
        && cm_hir_module_map_count(&map) == 1u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "ordered generic impl graph did not lower or map its root");
    wrapper_item = find_hir_item(&hir, root_hir, "Wrapper");
    trait_item = find_hir_item(&hir, root_hir, "Trait");
    impl_item = find_hir_impl(&hir);
    projection_item = find_hir_item(&hir, root_hir, "P");
    memset(&selection, 0, sizeof(selection));
    selection.status = CM_HIR_PROJECTION_INVALID_ASSOCIATION;
    if (projection_item != NULL) {
        selection = cm_hir_select_projection(&hir,
            projection_item->definition.crate_id,
            projection_item->data.type_alias_item.target);
    }
    selected = selection.status == CM_HIR_PROJECTION_SELECTED
        ? cm_hir_get_type(&hir, selection.target) : NULL;
    impl_self = impl_item == NULL ? NULL : cm_hir_get_type(&hir,
        impl_item->data.impl_item.self_type);
    check(wrapper_item != NULL
        && wrapper_item->kind == CM_HIR_ITEM_STRUCT
        && wrapper_item->generic_parameter_count == 1u
        && trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && impl_item != NULL && impl_item->generic_parameter_count == 1u
        && impl_self != NULL && impl_self->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_self->data.named_type.definition,
            wrapper_item->definition)
        && selection.status == CM_HIR_PROJECTION_SELECTED
        && selection.hir_status == CM_HIR_OK
        && selection.allocated_type_count == 0u
        && cm_hir_def_id_equal(selection.impl_definition,
            impl_item->definition)
        && selected != NULL && selected->kind == CM_HIR_TYPE_INTEGER_KIND
        && selected->data.integer_type.kind == CM_HIR_INT_U8,
        "ordered generic graph impl did not select Wrapper<u8>::Assoc");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_inherent_method_bound_lifetime_binder(void)
{
    static const unsigned char source[] =
        "struct VaList<'copy, 'f>(&'copy u8, &'f u8);"
        "trait FnOnce<Args> { type Output; }"
        "struct VaListImpl<'f>(&'f u8);"
        "impl<'f> VaListImpl<'f> {"
        "unsafe fn with_copy<F, R>(&self, f: F) -> R where "
        "F: for<'copy> FnOnce(VaList<'copy, 'f>) -> R { loop {} }"
        "}"
        "struct Number;"
        "impl Number {"
        "#[inline] pub const fn is_nan(self) -> bool { loop {} }"
        "}";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *impl_item;
    const CmHirItem *method;
    const CmHirItem *const_method;
    const CmHirTraitPredicate *predicate;
    const CmHirType *call_tuple;
    const CmHirType *va_list;
    const CmHirGenericParam *outer_lifetime;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "hrtb-inherent/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add inherent HRTB source");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "inherent HRTB graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower inherent HRTB: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    impl_item = find_hir_impl(&hir);
    method = impl_item == NULL ? NULL : find_hir_associated_item(&hir,
        impl_item->definition, CM_HIR_ITEM_FUNCTION, "with_copy");
    const_method = find_hir_item_anywhere(&hir, "is_nan");
    predicate = method == NULL || method->predicate_count != 1u
        ? NULL : &method->predicates[0];
    call_tuple = predicate == NULL
            || predicate->trait_type.arguments == NULL
            || predicate->trait_type.argument_count != 1u
            || predicate->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir,
            predicate->trait_type.arguments[0].data.type);
    va_list = call_tuple == NULL
            || call_tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || call_tuple->data.tuple_type.elements == NULL
            || call_tuple->data.tuple_type.element_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            call_tuple->data.tuple_type.elements[0]);
    outer_lifetime = impl_item == NULL
        ? NULL : cm_hir_get_generic_param(&hir,
            impl_item->generic_parameter_start);
    check(result.error_count == 0u && result.lowered_item_count == 9u
        && impl_item != NULL && impl_item->generic_parameter_count == 1u
        && outer_lifetime != NULL
        && outer_lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && predicate != NULL && predicate->binder.lifetime_count == 1u
        && predicate->binder.lifetimes != NULL
        && hir_name_is(&hir, predicate->binder.lifetimes[0], "'copy")
        && va_list != NULL && va_list->kind == CM_HIR_TYPE_ADT_KIND
        && va_list->data.named_type.arguments != NULL
        && va_list->data.named_type.argument_count == 2u
        && va_list->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && va_list->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_LATE_BOUND
        && va_list->data.named_type.arguments[0].data.lifetime.data.binder_index
            == 0u
        && va_list->data.named_type.arguments[1].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && va_list->data.named_type.arguments[1].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && va_list->data.named_type.arguments[1].data.lifetime.data.parameter
            == impl_item->generic_parameter_start
        && predicate->equality_count == 1u
        && predicate->equalities != NULL,
        "inherent HRTB lost binder, late/early regions, or callable output");
    check(const_method != NULL
        && const_method->kind == CM_HIR_ITEM_FUNCTION
        && !cm_hir_def_id_is_none(const_method->parent_definition)
        && const_method->visibility.kind == CM_HIR_VIS_PUBLIC
        && const_method->attribute_count == 1u
        && hir_name_is(&hir, const_method->attributes[0].metadata, "inline")
        && const_method->data.function_item.signature.is_const
        && !const_method->data.function_item.signature.is_async
        && hir_name_is(&hir,
            const_method->data.function_item.signature.abi, "Rust")
        && const_method->data.function_item.body != CM_HIR_BODY_NONE,
        "const inherent method lost parent, visibility, attribute, or body");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generic_trait_impl_method(void)
{
    static const unsigned char source[] =
        "trait Hasher {}\n"
        "trait Hash { extern \"rust-call\" fn hash<H>(&self, state: &mut H) "
        "where H: Hasher; }\n"
        "struct Value;\n"
        "impl Hash for Value {\n"
        "#[inline] extern \"rust-call\" fn hash<H>(&self, state: &mut H) "
        "where H: Hasher "
        "{ loop {} }\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *hash_trait;
    const CmHirItem *trait_method;
    const CmHirItem *impl_item;
    const CmHirItem *impl_method;
    const CmHirGenericParam *generic;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "generic-trait-method/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generic trait impl method fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generic trait method: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    hash_trait = find_hir_item_anywhere(&hir, "Hash");
    trait_method = hash_trait == NULL ? NULL
        : find_hir_associated_item(&hir, hash_trait->definition,
            CM_HIR_ITEM_FUNCTION, "hash");
    impl_item = find_hir_impl(&hir);
    impl_method = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_FUNCTION, "hash");
    generic = impl_method == NULL ? NULL : cm_hir_get_generic_param(&hir,
        impl_method->generic_parameter_start);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 6u && hash_trait != NULL
        && trait_method != NULL && impl_item != NULL && impl_method != NULL
        && impl_item->data.impl_item.has_trait
        && cm_hir_def_id_equal(impl_method->parent_definition,
            impl_item->definition)
        && cm_hir_def_id_equal(impl_method->data.function_item
                .trait_item_definition,
            trait_method->definition)
        && trait_method->generic_parameter_count == 1u
        && impl_method->generic_parameter_count == 1u
        && generic != NULL && generic->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(generic->owner, impl_method->definition)
        && hir_name_is(&hir, generic->name, "H")
        && impl_method->predicate_count == 1u
        && impl_method->data.function_item.signature.parameter_count == 2u
        && hir_name_is(&hir,
            impl_method->data.function_item.signature.abi, "rust-call")
        && impl_method->data.function_item.body != CM_HIR_BODY_NONE
        && impl_method->attribute_count == 1u
        && hir_name_is(&hir, impl_method->attributes[0].metadata, "inline"),
        "generic trait impl method lost declaration, generics, predicate, or body");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_lifetime_generic_default_trait_method(void)
{
    static const unsigned char source[] =
        "struct Request<'a>(&'a u8);\n"
        "trait Error {\n"
        "fn provide<'a>(&'a self, request: &mut Request<'a>) {}\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *error_trait;
    const CmHirItem *provide;
    const CmHirGenericParam *lifetime;
    const CmHirFunctionSignature *signature;
    const CmHirType *receiver;
    const CmHirType *request_reference;
    const CmHirType *request;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "lifetime-trait-method/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add lifetime-generic default trait method fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower lifetime trait method: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    error_trait = find_hir_item_anywhere(&hir, "Error");
    provide = error_trait == NULL ? NULL
        : find_hir_associated_item(&hir, error_trait->definition,
            CM_HIR_ITEM_FUNCTION, "provide");
    lifetime = provide == NULL ? NULL : cm_hir_get_generic_param(&hir,
        provide->generic_parameter_start);
    signature = provide == NULL
        ? NULL : &provide->data.function_item.signature;
    receiver = signature == NULL || signature->parameter_count != 2u
        ? NULL : cm_hir_get_type(&hir, signature->parameters[0].type);
    request_reference = signature == NULL || signature->parameter_count != 2u
        ? NULL : cm_hir_get_type(&hir, signature->parameters[1].type);
    request = request_reference == NULL
            || request_reference->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(&hir,
            request_reference->data.reference_type.pointee);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 3u && error_trait != NULL
        && provide != NULL && provide->generic_parameter_count == 1u
        && lifetime != NULL && lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && cm_hir_def_id_equal(lifetime->owner, provide->definition)
        && hir_name_is(&hir, lifetime->name, "'a")
        && signature != NULL
        && signature->receiver == CM_HIR_RECEIVER_REF_SHARED
        && signature->parameter_count == 2u
        && receiver != NULL
        && receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
        && receiver->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && receiver->data.reference_type.region.kind
            == CM_HIR_REGION_EARLY_BOUND
        && receiver->data.reference_type.region.data.parameter
            == provide->generic_parameter_start
        && request_reference != NULL
        && request_reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && request_reference->data.reference_type.mutability
            == CM_HIR_MUTABLE
        && request != NULL && request->kind == CM_HIR_TYPE_ADT_KIND
        && request->data.named_type.argument_count == 1u
        && request->data.named_type.arguments != NULL
        && request->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && request->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && request->data.named_type.arguments[0].data.lifetime.data.parameter
            == provide->generic_parameter_start
        && provide->data.function_item.body != CM_HIR_BODY_NONE,
        "default trait method lost lifetime owner, regions, signature, or body");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_adt_generic_type_defaults(void)
{
    static const unsigned char source[] =
        "pub enum ControlFlow<B, C = ()> { Continue(C), Break(B) }\n"
        "struct Pair<T = u8, U = T>(T, U);\n"
        "struct Uses {\n"
        "enum_default: ControlFlow<u16>,\n"
        "enum_explicit: ControlFlow<u16, u32>,\n"
        "pair_defaults: Pair,\n"
        "pair_partial: Pair<u64>,\n"
        "pair_explicit: Pair<u32, u16>,\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *control_flow;
    const CmHirItem *pair;
    const CmHirItem *uses;
    const CmHirGenericParam *break_parameter;
    const CmHirGenericParam *continue_parameter;
    const CmHirGenericParam *pair_first_parameter;
    const CmHirGenericParam *pair_second_parameter;
    const CmHirType *continue_default;
    const CmHirType *pair_first_default;
    const CmHirType *pair_second_default;
    const CmHirType *applications[5];
    const CmHirType *arguments[10];
    uint32_t field_index;
    uint32_t argument_index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "adt-defaults/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add ADT generic-default fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower ADT defaults: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    control_flow = find_hir_item_anywhere(&hir, "ControlFlow");
    pair = find_hir_item_anywhere(&hir, "Pair");
    uses = find_hir_item_anywhere(&hir, "Uses");
    break_parameter = control_flow == NULL ? NULL
        : cm_hir_get_generic_param(&hir,
            control_flow->generic_parameter_start);
    continue_parameter = control_flow == NULL ? NULL
        : cm_hir_get_generic_param(&hir,
            control_flow->generic_parameter_start + 1u);
    pair_first_parameter = pair == NULL ? NULL
        : cm_hir_get_generic_param(&hir, pair->generic_parameter_start);
    pair_second_parameter = pair == NULL ? NULL
        : cm_hir_get_generic_param(&hir,
            pair->generic_parameter_start + 1u);
    continue_default = continue_parameter == NULL
            || !continue_parameter->has_default
        ? NULL : cm_hir_get_type(&hir,
            continue_parameter->default_argument.data.type);
    pair_first_default = pair_first_parameter == NULL
            || !pair_first_parameter->has_default
        ? NULL : cm_hir_get_type(&hir,
            pair_first_parameter->default_argument.data.type);
    pair_second_default = pair_second_parameter == NULL
            || !pair_second_parameter->has_default
        ? NULL : cm_hir_get_type(&hir,
            pair_second_parameter->default_argument.data.type);
    memset(applications, 0, sizeof(applications));
    memset(arguments, 0, sizeof(arguments));
    if (uses != NULL && uses->kind == CM_HIR_ITEM_STRUCT
        && uses->data.aggregate_item.field_count == 5u) {
        for (field_index = 0u; field_index < 5u; ++field_index) {
            applications[field_index] = cm_hir_get_type(&hir,
                uses->data.aggregate_item.fields[field_index].type);
            if (applications[field_index] != NULL
                && applications[field_index]->kind
                    == CM_HIR_TYPE_ADT_KIND
                && applications[field_index]->data.named_type.argument_count
                    == 2u) {
                for (argument_index = 0u; argument_index < 2u;
                     ++argument_index) {
                    arguments[field_index * 2u + argument_index] =
                        cm_hir_get_type(&hir, applications[field_index]
                            ->data.named_type.arguments[argument_index]
                            .data.type);
                }
            }
        }
    }
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 3u && control_flow != NULL
        && control_flow->kind == CM_HIR_ITEM_ENUM
        && control_flow->generic_parameter_count == 2u
        && break_parameter != NULL && !break_parameter->has_default
        && continue_parameter != NULL && continue_parameter->has_default
        && continue_parameter->default_argument.kind
            == CM_HIR_GENERIC_ARG_TYPE
        && cm_hir_def_id_equal(continue_parameter->owner,
            control_flow->definition)
        && continue_default != NULL
        && continue_default->kind == CM_HIR_TYPE_UNIT_KIND
        && pair != NULL && pair->kind == CM_HIR_ITEM_STRUCT
        && pair->generic_parameter_count == 2u
        && pair_first_parameter != NULL && pair_first_parameter->has_default
        && pair_first_default != NULL
        && pair_first_default->kind == CM_HIR_TYPE_INTEGER_KIND
        && pair_first_default->data.integer_type.kind == CM_HIR_INT_U8
        && pair_second_parameter != NULL
        && pair_second_parameter->has_default
        && cm_hir_def_id_equal(pair_second_parameter->owner,
            pair->definition)
        && pair_second_default != NULL
        && pair_second_default->kind == CM_HIR_TYPE_PARAMETER_KIND
        && pair_second_default->data.parameter_type.parameter
            == pair->generic_parameter_start
        && applications[0] != NULL
        && cm_hir_def_id_equal(applications[0]->data.named_type.definition,
            control_flow->definition)
        && arguments[0] != NULL
        && arguments[0]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[0]->data.integer_type.kind == CM_HIR_INT_U16
        && arguments[1] != NULL
        && arguments[1]->kind == CM_HIR_TYPE_UNIT_KIND
        && arguments[2] != NULL
        && arguments[2]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[2]->data.integer_type.kind == CM_HIR_INT_U16
        && arguments[3] != NULL
        && arguments[3]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[3]->data.integer_type.kind == CM_HIR_INT_U32
        && applications[2] != NULL
        && cm_hir_def_id_equal(applications[2]->data.named_type.definition,
            pair->definition)
        && arguments[4] != NULL && arguments[5] != NULL
        && arguments[4]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[4]->data.integer_type.kind == CM_HIR_INT_U8
        && arguments[5]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[5]->data.integer_type.kind == CM_HIR_INT_U8
        && arguments[6] != NULL && arguments[7] != NULL
        && arguments[6]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[6]->data.integer_type.kind == CM_HIR_INT_U64
        && arguments[7]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[7]->data.integer_type.kind == CM_HIR_INT_U64
        && arguments[8] != NULL && arguments[9] != NULL
        && arguments[8]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[8]->data.integer_type.kind == CM_HIR_INT_U32
        && arguments[9]->kind == CM_HIR_TYPE_INTEGER_KIND
        && arguments[9]->data.integer_type.kind == CM_HIR_INT_U16,
        "ADT defaults lost declaration identity or application substitution");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_lifetime_generic_trait_outlives(void)
{
    static const unsigned char source[] =
        "trait Copy {}\n"
        "unsafe trait Erased<'a>: Copy + 'static + 'a {}\n";
    static const char *const rejected[] = {
        "trait Bad: 'free {}",
        "trait Bad: '_ {}",
        "trait Bad<T>: 'T {}"
    };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *erased;
    const CmHirItem *copy;
    const CmHirGenericParam *lifetime;
    const CmHirOutlivesPredicate *static_outlives;
    const CmHirOutlivesPredicate *generic_outlives;
    const CmHirType *static_subject;
    const CmHirType *generic_subject;
    size_t case_index;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "lifetime-trait/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add lifetime-generic trait fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower lifetime trait: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    erased = find_hir_item_anywhere(&hir, "Erased");
    copy = find_hir_item_anywhere(&hir, "Copy");
    lifetime = erased == NULL ? NULL : cm_hir_get_generic_param(&hir,
        erased->generic_parameter_start);
    static_outlives = erased == NULL
            || erased->outlives_predicate_count != 2u
        ? NULL : &erased->outlives_predicates[0];
    generic_outlives = erased == NULL
            || erased->outlives_predicate_count != 2u
        ? NULL : &erased->outlives_predicates[1];
    static_subject = static_outlives == NULL
        ? NULL : cm_hir_get_type(&hir, static_outlives->subject.type);
    generic_subject = generic_outlives == NULL
        ? NULL : cm_hir_get_type(&hir, generic_outlives->subject.type);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 2u && erased != NULL && copy != NULL
        && erased->kind == CM_HIR_ITEM_TRAIT
        && erased->data.trait_item.safety == CM_HIR_UNSAFE
        && erased->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(erased->data.trait_item.supertraits[0]
                .trait_type.definition,
            copy->definition)
        && source_span_is(&sources,
            erased->data.trait_item.supertraits[0].span, "Copy")
        && erased->generic_parameter_count == 1u
        && lifetime != NULL && lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && cm_hir_def_id_equal(lifetime->owner, erased->definition)
        && hir_name_is(&hir, lifetime->name, "'a")
        && erased->predicate_count == 0u
        && erased->outlives_predicate_count == 2u
        && static_outlives != NULL && generic_outlives != NULL
        && static_outlives->subject_kind == CM_HIR_OUTLIVES_TYPE
        && static_outlives->scope == CM_HIR_PREDICATE_SCOPE_NONE
        && static_subject != NULL
        && static_subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(static_subject->data.self_type.owner,
            erased->definition)
        && static_outlives->bound.kind == CM_HIR_REGION_STATIC
        && source_span_is(&sources, static_outlives->span, "'static")
        && generic_outlives->subject_kind == CM_HIR_OUTLIVES_TYPE
        && generic_outlives->scope == CM_HIR_PREDICATE_SCOPE_NONE
        && generic_subject != NULL
        && generic_subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(generic_subject->data.self_type.owner,
            erased->definition)
        && generic_outlives->bound.kind == CM_HIR_REGION_EARLY_BOUND
        && generic_outlives->bound.data.parameter
            == erased->generic_parameter_start
        && source_span_is(&sources, generic_outlives->span, "'a"),
        "lifetime trait lost static/generic Self outlives source order");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    for (case_index = 0u;
         case_index < sizeof(rejected) / sizeof(rejected[0]);
         ++case_index) {
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;
        size_t saved_definitions;
        size_t saved_strings;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "lifetime-trait-reject/lib.rs",
            (const unsigned char *)rejected[case_index],
            strlen(rejected[case_index]), &root) == CM_SOURCE_OK,
            "could not add rejected lifetime supertrait fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        sentinel_span.source = 92u;
        sentinel_span.start = 3u;
        sentinel_span.end = 8u;
        sentinel_name = cm_interner_intern(&hir.strings,
            (const unsigned char *)"sentinel", 8u);
        check(cm_hir_create_crate(&hir, sentinel_name,
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root) == CM_HIR_OK,
            "could not seed lifetime supertrait rollback fixture");
        saved_definitions = hir.definitions.len;
        saved_strings = cm_interner_length(&hir.strings);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u && result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
            && cm_hir_module_map_count(&map) == 0u
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.items.len == 0u && hir.types.len == 0u
            && hir.definitions.len == saved_definitions
            && cm_interner_length(&hir.strings) == saved_strings,
            "free lifetime supertrait escaped or mutated HIR");
        (void)sentinel_crate;
        (void)sentinel_root;
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_associated_type_lifetime_bounds_graph(void)
{
    static const unsigned char source[] =
        "trait Copy<Rhs = Self> {} "
        "trait Owner<'a, T, const N: usize> { "
        "type Assoc: 'static + Copy + 'a where T: 'a; }";
    static const unsigned char rejected[] =
        "trait Owner { type Assoc: 'static + '_; }";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *owner;
    const CmHirItem *associated;
    const CmHirAssociatedTypeBound *trait_bound;
    const CmHirOutlivesPredicate *static_bound;
    const CmHirOutlivesPredicate *early_bound;
    const CmHirType *projection;
    char *dump;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "associated-lifetime/lib.rs",
        source, sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add associated lifetime graph fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    owner = find_hir_item_anywhere(&hir, "Owner");
    associated = owner == NULL ? NULL : find_hir_associated_item(&hir,
        owner->definition, CM_HIR_ITEM_TYPE_ALIAS, "Assoc");
    trait_bound = associated == NULL
            || associated->data.type_alias_item.bound_count != 1u
        ? NULL : &associated->data.type_alias_item.bounds[0];
    static_bound = associated == NULL
            || associated->outlives_predicate_count != 3u
        ? NULL : &associated->outlives_predicates[0];
    early_bound = associated == NULL
            || associated->outlives_predicate_count != 3u
        ? NULL : &associated->outlives_predicates[1];
    projection = static_bound == NULL ? NULL
        : cm_hir_get_type(&hir, static_bound->subject.type);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && owner != NULL && owner->generic_parameter_count == 3u
        && associated != NULL
        && associated->data.type_alias_item.bound_count == 1u
        && trait_bound != NULL
        && trait_bound->trait_type.argument_count == 1u
        && trait_bound->trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && static_bound != NULL && early_bound != NULL
        && trait_bound->trait_type.arguments[0].data.type
            == static_bound->subject.type
        && static_bound->bound.kind == CM_HIR_REGION_STATIC
        && early_bound->bound.kind == CM_HIR_REGION_EARLY_BOUND
        && early_bound->bound.data.parameter
            == owner->generic_parameter_start
        && static_bound->subject.type == early_bound->subject.type
        && projection != NULL
        && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(projection->data.projection_type
                .trait_type.definition,
            owner->definition)
        && cm_hir_def_id_equal(projection->data.projection_type
                .associated_type.definition,
            associated->definition)
        && projection->data.projection_type.trait_type.argument_count == 3u
        && source_span_is(&sources, static_bound->span, "'static")
        && source_span_is(&sources, early_bound->span, "'a"),
        "associated lifetime graph lowering lost projection identity");
    dump = dump_hir(&hir);
    check(dump != NULL && strstr(dump, " projection ") != NULL
        && strstr(dump, "outlives-predicate") != NULL
        && strstr(dump, "bound='static") != NULL,
        "associated lifetime graph dump lost projection or predicate");
    free(dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);

    {
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmHirDefId sentinel_trait;
        CmHirDefId sentinel_associated;
        CmHirItem sentinel_item;
        CmHirItemId ignored_item;
        CmSpan sentinel_span;
        size_t saved_arena;
        size_t saved_bodies;
        size_t saved_crates;
        size_t saved_definitions;
        size_t saved_expressions;
        size_t saved_generics;
        size_t saved_items;
        size_t saved_modules;
        size_t saved_prebinds;
        size_t saved_string_arena;
        size_t saved_strings;
        size_t saved_types;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "associated-lifetime-reject/lib.rs", rejected,
            sizeof(rejected) - 1u, &root) == CM_SOURCE_OK,
            "could not add rejected associated lifetime graph fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        sentinel_span.source = 93u;
        sentinel_span.start = 1u;
        sentinel_span.end = 20u;
        check(cm_hir_create_crate(&hir,
            cm_hir_intern(&hir, "associated_lifetime_sentinel"),
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root) == CM_HIR_OK,
            "could not seed associated lifetime graph rollback crate");
        check(cm_hir_reserve_item_definition_as(&hir, sentinel_crate,
            CM_HIR_ITEM_TRAIT, sentinel_span, &sentinel_trait) == CM_HIR_OK,
            "could not reserve associated lifetime rollback trait");
        sentinel_span.start = 5u;
        sentinel_span.end = 15u;
        check(cm_hir_reserve_item_definition_as(&hir, sentinel_crate,
            CM_HIR_ITEM_TYPE_ALIAS, sentinel_span,
            &sentinel_associated) == CM_HIR_OK,
            "could not reserve associated lifetime rollback child");
        memset(&sentinel_item, 0, sizeof(sentinel_item));
        sentinel_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
        sentinel_item.definition = sentinel_associated;
        sentinel_item.owner_module = sentinel_root;
        sentinel_item.parent_definition = sentinel_trait;
        sentinel_item.name = cm_hir_intern(&hir, "SentinelAssoc");
        sentinel_item.visibility.kind = CM_HIR_VIS_PRIVATE;
        sentinel_item.visibility.restriction = cm_hir_def_id_none();
        sentinel_item.span = sentinel_span;
        sentinel_item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
        sentinel_item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
        sentinel_item.data.type_alias_item.trait_item_definition =
            cm_hir_def_id_none();
        check(cm_hir_prebind_trait_associated_type_declaration(&hir,
            &sentinel_item, &ignored_item) == CM_HIR_OK,
            "could not seed associated lifetime rollback prebind");
        saved_arena = cm_arena_bytes_used(&hir.storage);
        saved_string_arena = cm_arena_bytes_used(&hir.strings.strings);
        saved_strings = cm_interner_length(&hir.strings);
        saved_crates = hir.crates.len;
        saved_modules = hir.modules.len;
        saved_items = hir.items.len;
        saved_bodies = hir.bodies.len;
        saved_expressions = hir.expressions.len;
        saved_types = hir.types.len;
        saved_generics = hir.generic_parameters.len;
        saved_definitions = hir.definitions.len;
        saved_prebinds = hir.prebound_associated_types.len;
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u && result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
            && cm_hir_module_map_count(&map) == 0u
            && hir.crates.len == saved_crates
            && hir.modules.len == saved_modules
            && hir.items.len == saved_items
            && hir.bodies.len == saved_bodies
            && hir.expressions.len == saved_expressions
            && hir.types.len == saved_types
            && hir.generic_parameters.len == saved_generics
            && hir.definitions.len == saved_definitions
            && hir.prebound_associated_types.len == saved_prebinds
            && cm_arena_bytes_used(&hir.storage) == saved_arena
            && cm_interner_length(&hir.strings) == saved_strings
            && cm_arena_bytes_used(&hir.strings.strings)
                == saved_string_arena,
            "associated lifetime graph failure escaped its HIR transaction");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_adt_generic_type_defaults_fail_closed(void)
{
    static const char *const sources_text[] = {
        "struct Bad<T = u8, U>(T, U);",
        "struct Bad<T = T>(T);",
        "enum Bad<T = U, U = u8> { Value(T, U) }",
        "enum ControlFlow<B, C = ()> { Continue(C), Break(B) } "
        "struct Bad(ControlFlow);"
    };
    static const char *const messages[] = {
        "a required generic parameter follows a defaulted one",
        "generic type default references itself or a later parameter",
        "generic type default references itself or a later parameter",
        "ADT type application omits a required generic argument"
    };
    size_t case_index;

    for (case_index = 0u;
         case_index < sizeof(sources_text) / sizeof(sources_text[0]);
         ++case_index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "adt-default-failure/lib.rs",
            (const unsigned char *)sources_text[case_index],
            strlen(sources_text[case_index]), &root) == CM_SOURCE_OK,
            "could not add ADT default failure fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u && result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
            && strstr(result.first_error.message, messages[case_index])
                != NULL
            && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
            "invalid ADT default did not fail closed transactionally");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_generic_associated_type_declaration(void)
{
    static const unsigned char source[] =
        "trait Future<T = ()> { type Output; }\n"
        "trait AsyncFnMut<Args>: AsyncFn<Args> {\n"
        "type CallRefFuture<'a>: Future<u8, Output = Self::Output>\n"
        "where Self: 'a;\n"
        "}\n"
        "trait AsyncFn<Args> { type Output; }\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *future;
    const CmHirItem *future_output;
    const CmHirItem *async_fn;
    const CmHirItem *async_output;
    const CmHirItem *async_fn_mut;
    const CmHirItem *gat;
    const CmHirGenericParam *lifetime;
    const CmHirAssociatedTypeBound *bound;
    const CmHirType *bound_argument;
    const CmHirType *equality_value;
    const CmHirType *projection_argument;
    const CmHirType *supertrait_argument;
    const CmHirType *subject;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "gat/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generic associated type fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower GAT declaration: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    future = find_hir_item_anywhere(&hir, "Future");
    future_output = future == NULL ? NULL
        : find_hir_associated_item(&hir, future->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    async_fn = find_hir_item_anywhere(&hir, "AsyncFn");
    async_output = async_fn == NULL ? NULL
        : find_hir_associated_item(&hir, async_fn->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    async_fn_mut = find_hir_item_anywhere(&hir, "AsyncFnMut");
    gat = async_fn_mut == NULL ? NULL
        : find_hir_associated_item(&hir, async_fn_mut->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "CallRefFuture");
    lifetime = gat == NULL ? NULL : cm_hir_get_generic_param(&hir,
        gat->generic_parameter_start);
    bound = gat == NULL || gat->data.type_alias_item.bound_count != 1u
        ? NULL : &gat->data.type_alias_item.bounds[0];
    equality_value = bound == NULL || bound->equality_count != 1u
        ? NULL : cm_hir_get_type(&hir, bound->equalities[0].value);
    projection_argument = equality_value == NULL
            || equality_value->kind != CM_HIR_TYPE_PROJECTION_KIND
            || equality_value->data.projection_type.trait_type.argument_count
                != 1u
            || equality_value->data.projection_type.trait_type.arguments[0]
                    .kind != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir,
            equality_value->data.projection_type.trait_type.arguments[0]
                .data.type);
    supertrait_argument = async_fn_mut == NULL
            || async_fn_mut->data.trait_item.supertrait_count != 1u
            || async_fn_mut->data.trait_item.supertraits[0]
                    .trait_type.argument_count != 1u
            || async_fn_mut->data.trait_item.supertraits[0]
                    .trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir,
            async_fn_mut->data.trait_item.supertraits[0]
                .trait_type.arguments[0].data.type);
    bound_argument = bound == NULL
            || bound->trait_type.argument_count != 1u
            || bound->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir,
            bound->trait_type.arguments[0].data.type);
    subject = gat == NULL || gat->outlives_predicate_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            gat->outlives_predicates[0].subject.type);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 6u && future != NULL
        && future_output != NULL && async_fn != NULL
        && async_output != NULL && async_fn_mut != NULL && gat != NULL
        && async_fn->generic_parameter_count == 1u
        && async_fn_mut->generic_parameter_count == 1u
        && supertrait_argument != NULL
        && supertrait_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
        && supertrait_argument->data.parameter_type.parameter
            == async_fn_mut->generic_parameter_start
        && gat->generic_parameter_count == 1u
        && lifetime != NULL && lifetime->kind == CM_HIR_GENERIC_LIFETIME
        && cm_hir_def_id_equal(lifetime->owner, gat->definition)
        && hir_name_is(&hir, lifetime->name, "'a")
        && bound != NULL
        && cm_hir_def_id_equal(bound->trait_type.definition,
            future->definition)
        && bound_argument != NULL
        && bound_argument->kind == CM_HIR_TYPE_INTEGER_KIND
        && bound_argument->data.integer_type.kind == CM_HIR_INT_U8
        && bound->equality_count == 1u
        && cm_hir_def_id_equal(bound->equalities[0].associated_type,
            future_output->definition)
        && equality_value != NULL
        && equality_value->kind == CM_HIR_TYPE_PROJECTION_KIND
        && cm_hir_def_id_equal(equality_value->data.projection_type
                .trait_type.definition,
            async_fn->definition)
        && equality_value->data.projection_type.trait_type.argument_count
            == 1u
        && equality_value->data.projection_type.trait_type.arguments[0]
                .data.type
            == async_fn_mut->data.trait_item.supertraits[0]
                .trait_type.arguments[0].data.type
        && projection_argument == supertrait_argument
        && cm_hir_def_id_equal(equality_value->data.projection_type
                .associated_type.definition,
            async_output->definition)
        && gat->outlives_predicate_count == 1u
        && subject != NULL && subject->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(subject->data.self_type.owner,
            async_fn_mut->definition)
        && gat->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_EARLY_BOUND
        && gat->outlives_predicates[0].bound.data.parameter
            == gat->generic_parameter_start,
        "GAT declaration lost lifetime, bound equality, or Self outlives predicate");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_transitive_generic_self_projection_graph_rollback(void)
{
    static const unsigned char source[] =
        "trait Future { type Output; }\n"
        "trait Leaf<V>: Mid<V> {\n"
        "type Gat<'a>: Future<Output = Self::Output> where Self: 'a;\n"
        "}\n"
        "trait Mid<U>: Base<U> {}\n"
        "trait Base<T> { type Output; }\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "transitive-gat/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add transitive generic GAT rollback fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    check(graph_result.error_count == 0u && result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_GENERIC
        && strstr(result.first_error.message,
            "transitive generic supertraits requires structural substitution")
            != NULL
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "transitive generic GAT failure escaped its graph transaction");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_attributed_async_trait_method(void)
{
    static const unsigned char source[] =
        "struct Pin<T>(T);\n"
        "trait AsyncDrop {\n"
        "#[allow(async_fn_in_trait)]\n"
        "async fn drop(self: Pin<&mut Self>);\n"
        "}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *trait_item;
    const CmHirItem *method;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "async-drop/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add attributed async trait method fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower async trait method: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    trait_item = find_hir_item_anywhere(&hir, "AsyncDrop");
    method = trait_item == NULL ? NULL
        : find_hir_associated_item(&hir, trait_item->definition,
            CM_HIR_ITEM_FUNCTION, "drop");
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 3u && trait_item != NULL
        && method != NULL && method->kind == CM_HIR_ITEM_FUNCTION
        && method->data.function_item.signature.is_async
        && method->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_CUSTOM
        && method->data.function_item.signature.parameter_count == 1u
        && method->data.function_item.body == CM_HIR_BODY_NONE
        && method->attribute_count == 1u
        && hir_name_is(&hir, method->attributes[0].metadata,
            "allow(async_fn_in_trait)"),
        "async trait method lost attribute, async flag, or custom receiver");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_predicate_prefix_lifetime_binder(void)
{
    static const unsigned char source[] =
        "struct GenericShunt<'a, I, R>(&'a I, &'a R);"
        "trait FnMut<Args> { type Output; }"
        "fn adapt<I, R, F, U>() where "
        "for<'a> F: FnMut(GenericShunt<'a, I, R>) -> U {}"
        "trait Marker<T> {} trait Other<T> {}"
        "fn share<F>() where "
        "for<'a> F: Marker<&'a u8> + Other<&'a u8> + 'a {}";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *function;
    const CmHirItem *shared_function;
    const CmHirPredicateScope *scope;
    const CmHirTraitPredicate *predicate;
    const CmHirType *call_tuple;
    const CmHirType *shunt;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "hrtb-prefix/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add predicate-prefix HRTB source");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "predicate-prefix HRTB graph did not build");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower predicate-prefix HRTB: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    function = find_hir_item_anywhere(&hir, "adapt");
    shared_function = find_hir_item_anywhere(&hir, "share");
    scope = function == NULL || function->predicate_scope_count != 1u
        ? NULL : &function->predicate_scopes[0];
    predicate = function == NULL || function->predicate_count != 1u
        ? NULL : &function->predicates[0];
    call_tuple = predicate == NULL
            || predicate->trait_type.argument_count != 1u
            || predicate->trait_type.arguments == NULL
            || predicate->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(&hir,
            predicate->trait_type.arguments[0].data.type);
    shunt = call_tuple == NULL
            || call_tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || call_tuple->data.tuple_type.element_count != 1u
            || call_tuple->data.tuple_type.elements == NULL
        ? NULL : cm_hir_get_type(&hir,
            call_tuple->data.tuple_type.elements[0]);
    check(result.error_count == 0u && function != NULL && scope != NULL
        && predicate != NULL && predicate->scope == 1u
        && predicate->binder.lifetime_count == 0u
        && scope->subject_kind == CM_HIR_OUTLIVES_TYPE
        && scope->subject.type == predicate->subject
        && scope->binder.lifetime_count == 1u
        && scope->binder.lifetimes != NULL
        && hir_name_is(&hir, scope->binder.lifetimes[0], "'a")
        && scope->trait_predicate_count == 1u
        && scope->outlives_predicate_count == 0u
        && shunt != NULL && shunt->kind == CM_HIR_TYPE_ADT_KIND
        && shunt->data.named_type.argument_count == 3u
        && shunt->data.named_type.arguments != NULL
        && shunt->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_LIFETIME
        && shunt->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_LATE_BOUND
        && shunt->data.named_type.arguments[0].data.lifetime.data.binder_index
            == 0u
        && predicate->equality_count == 1u,
        "predicate-prefix HRTB lost shared scope, late region, or output");
    check(shared_function != NULL
        && shared_function->predicate_scope_count == 1u
        && shared_function->predicate_scopes != NULL
        && shared_function->predicate_scopes[0].trait_predicate_count == 2u
        && shared_function->predicate_scopes[0].outlives_predicate_count == 1u
        && shared_function->predicate_count == 2u
        && shared_function->predicates[0].scope == 1u
        && shared_function->predicates[1].scope == 1u
        && shared_function->outlives_predicate_count == 1u
        && shared_function->outlives_predicates[0].scope == 1u
        && shared_function->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_LATE_BOUND,
        "multi-bound predicate-prefix HRTB did not share one scope");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_declarations(void)
{
    static const unsigned char source[] =
        "struct Source;\n"
        "macro_rules! make { ($ty:ty) => {\n"
        "  #[cfg(unix)] pub struct Generated { pub source: Source }\n"
        "  pub enum Choice { One, Two(Source) }\n"
        "  pub type Alias = Source;\n"
        "  #[cfg(unix)] #[doc = \"value\"] pub const VALUE: u8 = 7;\n"
        "  #[cfg(unix)] #[doc = \"function\"]\n"
        "  pub const unsafe fn transform(value: $ty) -> $ty { value }\n"
        "  #[cfg(windows)] struct Hidden;\n"
        "} }\n"
        "make!(u8);\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem generated_view;
    CmResolveEffectiveItem value_view;
    CmResolveEffectiveItem function_view;
    CmResolveEffectiveAttribute value_attribute;
    CmResolveEffectiveAttribute function_attribute;
    const char *const value_attributes[] = { "doc = \"value\"" };
    const char *const function_attributes[] = { "doc = \"function\"" };
    const CmAst *value_ast;
    const CmAstItem *value_declaration;
    const CmAst *function_ast;
    const CmAstItem *function_declaration;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *source_item;
    const CmHirItem *generated_item;
    const CmHirItem *choice_item;
    const CmHirItem *alias_item;
    const CmHirItem *value_item;
    const CmHirItem *function_item;
    const CmHirType *field_type;
    const CmHirType *alias_type;
    const CmHirType *value_type;
    const CmHirType *parameter_type;
    const CmHirType *return_type;
    const CmHirBody *value_body;
    const CmHirBody *function_body;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&generated_view, 0, sizeof(generated_view));
    memset(&value_view, 0, sizeof(value_view));
    memset(&function_view, 0, sizeof(function_view));
    memset(&value_attribute, 0, sizeof(value_attribute));
    memset(&function_attribute, 0, sizeof(function_attribute));
    value_ast = NULL;
    value_declaration = NULL;
    function_ast = NULL;
    function_declaration = NULL;
    check(cm_source_add_memory(&sources, "generated/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generated declaration fixture");
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "Generated", &generated_view)
        && generated_view.is_generated
        && generated_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && generated_view.provenance.macro_invocation.source == root
        && generated_view.provenance.macro_invocation.item
            != CM_AST_ITEM_NONE
        && generated_view.provenance.macro_definition.source == root
        && generated_view.provenance.macro_definition.item
            != CM_AST_ITEM_NONE
        && generated_view.provenance.expansion_depth == 1u
        && generated_view.attribute_count == 0u
        && generated_view.span.source == root
        && generated_view.span.end > generated_view.span.start,
        "generated effective item has invalid provenance or attributes");
    check(effective_item_named(&graph, graph_result.revision,
            graph_result.root, "VALUE", &value_view)
        && value_view.is_generated
        && value_view.item_kind == CM_AST_ITEM_CONST
        && value_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && value_view.provenance.macro_invocation.source
            == generated_view.provenance.macro_invocation.source
        && value_view.provenance.macro_invocation.item
            == generated_view.provenance.macro_invocation.item
        && value_view.provenance.macro_definition.source
            == generated_view.provenance.macro_definition.source
        && value_view.provenance.macro_definition.item
            == generated_view.provenance.macro_definition.item
        && value_view.provenance.expansion_depth == 1u
        && value_view.span.source == generated_view.span.source
        && value_view.span.start == generated_view.span.start
        && value_view.span.end == generated_view.span.end
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &value_view, value_attributes, 1u,
            &value_attribute)
        && value_attribute.expansion_depth == 0u
        && value_attribute.span.start == value_view.span.start
        && value_attribute.span.end == value_view.span.end
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            value_view.declaration, &value_ast)
        && (value_declaration = cm_ast_get_item(value_ast,
            value_view.declaration.item)) != NULL
        && value_declaration->kind == CM_AST_ITEM_CONST
        && value_declaration->data.value_item.initializer
            != CM_AST_EXPR_NONE
        && cm_ast_get_expr(value_ast,
            value_declaration->data.value_item.initializer) != NULL,
        "generated const lacks authenticated declaration/body provenance");
    check(effective_item_named(&graph, graph_result.revision,
            graph_result.root, "transform", &function_view)
        && function_view.is_generated
        && function_view.item_kind == CM_AST_ITEM_FUNCTION
        && function_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && function_view.provenance.macro_invocation.source
            == generated_view.provenance.macro_invocation.source
        && function_view.provenance.macro_invocation.item
            == generated_view.provenance.macro_invocation.item
        && function_view.provenance.macro_definition.source
            == generated_view.provenance.macro_definition.source
        && function_view.provenance.macro_definition.item
            == generated_view.provenance.macro_definition.item
        && function_view.provenance.expansion_depth == 1u
        && function_view.declaration.source == root
        && function_view.span.source == root
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &function_view, function_attributes, 1u,
            &function_attribute)
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            function_view.declaration, &function_ast)
        && (function_declaration = cm_ast_get_item(function_ast,
            function_view.declaration.item)) != NULL
        && function_declaration->kind == CM_AST_ITEM_FUNCTION
        && function_declaration->data.function_item.is_const
        && function_declaration->data.function_item.is_unsafe
        && function_declaration->data.function_item.parameter_count == 1u
        && function_declaration->data.function_item.body != CM_AST_EXPR_NONE
        && cm_ast_get_expr(function_ast,
            function_declaration->data.function_item.body) != NULL,
        "generated function lacks authenticated declaration/body provenance");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph,
        graph_result.revision, &map, &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generated: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u && result.lowered_item_count == 6u
        && hir.items.len == 6u && hir.definitions.len == 9u,
        "generated declaration set did not lower exactly once");
    check(cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir,
            &root_hir) == CM_HIR_MODULE_MAP_OK,
        "generated fixture root module was not mapped");
    source_item = find_hir_item(&hir, root_hir, "Source");
    generated_item = find_hir_item(&hir, root_hir, "Generated");
    choice_item = find_hir_item(&hir, root_hir, "Choice");
    alias_item = find_hir_item(&hir, root_hir, "Alias");
    value_item = find_hir_item(&hir, root_hir, "VALUE");
    function_item = find_hir_item(&hir, root_hir, "transform");
    check(source_item != NULL && generated_item != NULL
        && choice_item != NULL && alias_item != NULL && value_item != NULL
        && function_item != NULL
        && find_hir_item(&hir, root_hir, "Hidden") == NULL,
        "generated declaration set differs from cfg-active graph items");
    field_type = NULL;
    if (generated_item != NULL
        && generated_item->data.aggregate_item.field_count == 1u
        && generated_item->data.aggregate_item.fields != NULL) {
        field_type = cm_hir_get_type(&hir,
            generated_item->data.aggregate_item.fields[0].type);
    }
    alias_type = alias_item == NULL
        ? NULL : cm_hir_get_type(&hir,
            alias_item->data.type_alias_item.target);
    value_type = value_item == NULL ? NULL
        : cm_hir_get_type(&hir, value_item->data.value_item.type);
    value_body = value_item == NULL ? NULL
        : cm_hir_get_body(&hir, value_item->data.value_item.body);
    parameter_type = function_item == NULL
            || function_item->kind != CM_HIR_ITEM_FUNCTION
            || function_item->data.function_item.signature.parameter_count
                != 1u
        ? NULL : cm_hir_get_type(&hir,
            function_item->data.function_item.signature.parameters[0].type);
    return_type = function_item == NULL
        ? NULL : cm_hir_get_type(&hir,
            function_item->data.function_item.signature.return_type);
    function_body = function_item == NULL ? NULL
        : cm_hir_get_body(&hir, function_item->data.function_item.body);
    check(generated_item != NULL
        && generated_item->span.source == generated_view.span.source
        && generated_item->span.start == generated_view.span.start
        && generated_item->span.end == generated_view.span.end
        && generated_item->data.aggregate_item.field_count == 1u
        && generated_item->data.aggregate_item.fields[0].span.source
            == generated_view.span.source
        && generated_item->data.aggregate_item.fields[0].span.start
            == generated_view.span.start
        && generated_item->data.aggregate_item.fields[0].span.end
            == generated_view.span.end
        && field_type != NULL
        && field_type->span.source == generated_view.span.source
        && field_type->span.start == generated_view.span.start
        && field_type->span.end == generated_view.span.end,
        "generated item or nested HIR spans lost the invocation anchor");
    check(source_item != NULL && field_type != NULL
        && field_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(field_type->data.named_type.definition,
            source_item->definition)
        && alias_type != NULL && alias_type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(alias_type->data.named_type.definition,
            source_item->definition),
        "generated field or alias did not resolve a source declaration");
    check(value_item != NULL && value_declaration != NULL
        && value_item->kind == CM_HIR_ITEM_CONST
        && value_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && value_item->span.source == value_view.span.source
        && value_item->span.start == value_view.span.start
        && value_item->span.end == value_view.span.end
        && hir_attributes_match_graph(&graph, &hir, value_item,
            &value_attribute, 1u)
        && value_item->attributes[0].span.start == value_view.span.start
        && value_item->attributes[0].span.end == value_view.span.end
        && value_type != NULL
        && value_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && value_type->data.integer_type.kind == CM_HIR_INT_U8
        && value_type->span.source == value_view.span.source
        && value_type->span.start == value_view.span.start
        && value_type->span.end == value_view.span.end
        && value_body != NULL
        && value_body->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_def_id_equal(value_body->owner, value_item->definition)
        && value_body->expected_type == value_item->data.value_item.type
        && value_body->source == value_view.declaration.source
        && value_body->source_expression_id
            == value_declaration->data.value_item.initializer
        && value_body->span.source == value_view.span.source
        && value_body->span.start == value_view.span.start
        && value_body->span.end == value_view.span.end,
        "generated const HIR lost type, attribute, body, or invocation provenance");
    check(function_item != NULL && function_declaration != NULL
        && function_item->kind == CM_HIR_ITEM_FUNCTION
        && function_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && function_item->span.source == function_view.span.source
        && function_item->span.start == function_view.span.start
        && function_item->span.end == function_view.span.end
        && hir_attributes_match_graph(&graph, &hir, function_item,
            &function_attribute, 1u)
        && function_item->data.function_item.signature.safety
            == CM_HIR_UNSAFE
        && function_item->data.function_item.signature.is_const
        && !function_item->data.function_item.signature.is_async
        && parameter_type != NULL
        && parameter_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && parameter_type->data.integer_type.kind == CM_HIR_INT_U8
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && return_type->data.integer_type.kind == CM_HIR_INT_U8
        && function_body != NULL
        && function_body->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_def_id_equal(function_body->owner,
            function_item->definition)
        && function_body->expected_type
            == function_item->data.function_item.signature.return_type
        && function_body->parameter_count == 1u
        && function_body->local_count == 1u
        && function_body->source == function_view.declaration.source
        && function_body->source_expression_id
            == function_declaration->data.function_item.body
        && function_body->span.source == function_view.span.source
        && function_body->span.start == function_view.span.start
        && function_body->span.end == function_view.span.end,
        "generated function HIR lost signature, body, or invocation provenance");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_declarative_macro_item(void)
{
    static const unsigned char source[] =
        "macro make { () => { pub struct Made; }, }\n"
        "make!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem made_view;
    const CmAst *definition_ast;
    const CmAstItem *definition;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *made;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&made_view, 0, sizeof(made_view));
    definition_ast = NULL;
    definition = NULL;
    check(cm_source_add_memory(&sources, "declarative-macro/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add declarative macro HIR fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "Made", &made_view)
        && made_view.is_generated
        && made_view.provenance.macro_definition.source == root
        && made_view.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            made_view.provenance.macro_definition, &definition_ast)
        && (definition = cm_ast_get_item(definition_ast,
            made_view.provenance.macro_definition.item)) != NULL
        && definition->kind == CM_AST_ITEM_MACRO
        && definition->data.macro_item.form
            == CM_AST_MACRO_DECLARATIVE_DEFINITION,
        "generated item did not retain its declarative macro definition");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower declarative macro: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 1u
        && hir.items.len == 1u && hir.definitions.len == 2u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "declarative macro output did not lower into HIR");
    made = find_hir_item(&hir, root_hir, "Made");
    check(made != NULL && made->kind == CM_HIR_ITEM_STRUCT
        && made->visibility.kind == CM_HIR_VIS_PUBLIC
        && made->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT
        && source_span_is(&sources, made->span, "make!();"),
        "declarative macro output lost item identity or invocation span");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_inline_module(void)
{
    static const unsigned char source[] =
        "macro_rules! make_module { () => {\n"
        "  #[doc = \"outer\"] mod private {\n"
        "    #![allow(dead_code)]\n"
        "    #[doc = \"sealed\"] pub trait Sealed {}\n"
        "  }\n"
        "} }\n"
        "make_module!();\n"
        "use self::private as private_alias;\n";
    static const char *const outer_attributes[] = { "doc = \"outer\"" };
    static const char *const sealed_attributes[] = { "doc = \"sealed\"" };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem module_view;
    CmResolveEffectiveItem sealed_view;
    CmResolveEffectiveAttribute outer_attribute;
    CmResolveEffectiveAttribute inner_attribute;
    CmResolveEffectiveAttribute sealed_attribute;
    CmModuleId private_graph;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    CmHirModuleId private_hir;
    const CmHirModule *root_module;
    const CmHirModule *private_module;
    const CmHirItem *sealed;
    const CmHirImport *import_value;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&module_view, 0, sizeof(module_view));
    memset(&sealed_view, 0, sizeof(sealed_view));
    memset(&outer_attribute, 0, sizeof(outer_attribute));
    memset(&inner_attribute, 0, sizeof(inner_attribute));
    memset(&sealed_attribute, 0, sizeof(sealed_attribute));
    check(cm_source_add_memory(&sources, "generated-module/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generated inline module fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    private_graph = CM_MODULE_NONE;
    check(graph_result.error_count == 0u
        && graph_module_named(&graph, "private", &private_graph)
        && effective_item_named(&graph, graph_result.revision,
            graph_result.root, "private", &module_view)
        && module_view.item_kind == CM_AST_ITEM_MODULE
        && module_view.is_generated
        && module_view.provenance.source_item.item == CM_AST_ITEM_NONE
        && module_view.provenance.macro_invocation.source == root
        && module_view.provenance.macro_invocation.item != CM_AST_ITEM_NONE
        && module_view.provenance.macro_definition.source == root
        && module_view.provenance.macro_definition.item != CM_AST_ITEM_NONE
        && module_view.provenance.expansion_depth == 1u
        && source_span_is(&sources, module_view.span, "make_module!();")
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &module_view, outer_attributes, 1u,
            &outer_attribute)
        && cm_module_graph_get_effective_inner_attribute(&graph,
            graph_result.revision, private_graph, 0u, &inner_attribute)
            == CM_RESOLVE_VIEW_OK
        && graph_string_is(&graph, inner_attribute.metadata,
            "allow (dead_code)")
        && inner_attribute.span.source == module_view.span.source
        && inner_attribute.span.start == module_view.span.start
        && inner_attribute.span.end == module_view.span.end
        && effective_item_named(&graph, graph_result.revision,
            private_graph, "Sealed", &sealed_view)
        && sealed_view.is_generated
        && source_span_is(&sources, sealed_view.span, "make_module!();")
        && effective_attributes_are(&graph, graph_result.revision,
            private_graph, &sealed_view, sealed_attributes, 1u,
            &sealed_attribute),
        "generated module graph lost invocation, attribute, or child identity");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower generated module: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    private_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 2u
        && hir.modules.len == 2u && hir.items.len == 1u
        && hir.definitions.len == 3u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, private_graph, &hir, &private_hir)
            == CM_HIR_MODULE_MAP_OK,
        "generated inline module did not map without a synthetic HIR item");
    root_module = cm_hir_get_module(&hir, root_hir);
    private_module = cm_hir_get_module(&hir, private_hir);
    sealed = find_hir_item(&hir, private_hir, "Sealed");
    import_value = root_module != NULL && root_module->import_count == 1u
        ? &root_module->imports[0] : NULL;
    check(private_module != NULL && sealed != NULL
        && private_module->parent == root_hir
        && hir_name_is(&hir, private_module->name, "private")
        && source_span_is(&sources, private_module->span, "make_module!();")
        && private_module->outer_attribute_count == 1u
        && private_module->outer_attributes != NULL
        && hir_name_is(&hir,
            private_module->outer_attributes[0].metadata, "doc = \"outer\"")
        && source_span_is(&sources,
            private_module->outer_attributes[0].span, "make_module!();")
        && private_module->inner_attribute_count == 1u
        && private_module->inner_attributes != NULL
        && hir_name_is(&hir,
            private_module->inner_attributes[0].metadata,
            "allow (dead_code)")
        && source_span_is(&sources,
            private_module->inner_attributes[0].span, "make_module!();")
        && sealed->kind == CM_HIR_ITEM_TRAIT
        && sealed->owner_module == private_hir
        && source_span_is(&sources, sealed->span, "make_module!();")
        && sealed->attribute_count == 1u && sealed->attributes != NULL
        && hir_name_is(&hir, sealed->attributes[0].metadata,
            "doc = \"sealed\"")
        && import_value != NULL && import_value->binding_count == 1u
        && hir_import_binding_is(&hir, import_value, 0u,
            CM_HIR_NAMESPACE_TYPE, "private_alias",
            private_module->definition),
        "generated HIR module lost mapped identity, attributes, or import");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_generated_anonymous_consts(void)
{
    static const unsigned char source[] =
        "macro_rules! make { () => {\n"
        "  const _: () = ();\n"
        "  const _: () = ();\n"
        "  pub const NAMED: () = ();\n"
        "} }\n"
        "make!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveModuleInfo root_information;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmHirModuleId root_hir;
    const CmHirItem *named;
    const CmHirItem *anonymous[2];
    const CmHirBody *anonymous_body[2];
    size_t item_index;
    uint32_t anonymous_count;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&root_information, 0, sizeof(root_information));
    memset(anonymous, 0, sizeof(anonymous));
    memset(anonymous_body, 0, sizeof(anonymous_body));
    check(cm_source_add_memory(&sources, "anonymous-const/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add generated anonymous const fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u
        && cm_module_graph_get_module(&graph, graph_result.root,
            &root_information)
        && root_information.effective_item_count == 3u
        && root_information.value_count == 1u,
        "anonymous consts were incorrectly published as value names");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower anonymous const: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(result.error_count == 0u && result.lowered_item_count == 3u
        && hir.items.len == 3u && hir.bodies.len == 3u
        && hir.definitions.len == 4u
        && cm_hir_module_map_lookup_hir(&map, &graph,
            graph_result.revision, graph_result.root, &hir, &root_hir)
            == CM_HIR_MODULE_MAP_OK,
        "generated anonymous consts did not retain distinct definitions");
    anonymous_count = 0u;
    for (item_index = 0u; item_index < hir.items.len; ++item_index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir.items, item_index);
        if (item != NULL && item->owner_module == root_hir
            && item->kind == CM_HIR_ITEM_CONST
            && hir_name_is(&hir, item->name, "_")
            && anonymous_count < 2u) {
            anonymous[anonymous_count] = item;
            anonymous_count += 1u;
        }
    }
    named = find_hir_item(&hir, root_hir, "NAMED");
    if (anonymous_count == 2u) {
        anonymous_body[0] = cm_hir_get_body(&hir,
            anonymous[0]->data.value_item.body);
        anonymous_body[1] = cm_hir_get_body(&hir,
            anonymous[1]->data.value_item.body);
    }
    check(anonymous_count == 2u && anonymous[0] != NULL
        && anonymous[1] != NULL && named != NULL
        && !cm_hir_def_id_equal(anonymous[0]->definition,
            anonymous[1]->definition)
        && anonymous[0]->data.value_item.body != CM_HIR_BODY_NONE
        && anonymous[1]->data.value_item.body != CM_HIR_BODY_NONE
        && anonymous[0]->data.value_item.body
            != anonymous[1]->data.value_item.body
        && anonymous_body[0] != NULL && anonymous_body[1] != NULL
        && anonymous_body[0]->state == CM_HIR_BODY_UNLOWERED
        && anonymous_body[1]->state == CM_HIR_BODY_UNLOWERED
        && source_span_is(&sources, anonymous[0]->span, "make!();")
        && source_span_is(&sources, anonymous[1]->span, "make!();")
        && named->visibility.kind == CM_HIR_VIS_PUBLIC,
        "anonymous const HIR lost unique identity, body, span, or named peer");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_inherent_associated_const(void)
{
    static const unsigned char source[] =
        "struct Number;\n"
        "impl Number {\n"
        "  #[doc = \"value\"] pub const VALUE: u8 = 7;\n"
        "}\n";
    static const char *const value_attributes[] = { "doc = \"value\"" };
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmResolveEffectiveItem impl_view;
    CmResolveEffectiveItem value_view;
    CmResolveEffectiveAttribute value_attribute;
    const CmAst *value_ast;
    const CmAstItem *value_declaration;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *number_item;
    const CmHirItem *impl_item;
    const CmHirItem *value_item;
    const CmHirType *impl_self;
    const CmHirType *value_type;
    const CmHirBody *value_body;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    memset(&impl_view, 0, sizeof(impl_view));
    memset(&value_view, 0, sizeof(value_view));
    memset(&value_attribute, 0, sizeof(value_attribute));
    value_ast = NULL;
    value_declaration = NULL;
    check(cm_source_add_memory(&sources, "inherent-const/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add inherent associated const fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    if (graph_result.error_count == 0u) {
        CmResolveModuleInfo information;
        uint32_t index;

        (void)cm_module_graph_get_module(&graph, graph_result.root,
            &information);
        for (index = 0u; index < information.effective_item_count; ++index) {
            CmResolveEffectiveItem item;

            if (cm_module_graph_get_effective_item(&graph,
                    graph_result.revision, graph_result.root, index, &item)
                    == CM_RESOLVE_VIEW_OK
                && item.item_kind == CM_AST_ITEM_IMPL) {
                impl_view = item;
                break;
            }
        }
    }
    check(graph_result.error_count == 0u
        && impl_view.id != CM_RESOLVE_EFFECTIVE_ITEM_NONE
        && impl_view.child_kind == CM_EXPANDED_CHILD_IMPL
        && impl_view.child_count == 1u
        && effective_child_named(&graph, graph_result.revision,
            graph_result.root, &impl_view, "VALUE", &value_view)
        && !value_view.is_generated
        && value_view.item_kind == CM_AST_ITEM_CONST
        && value_view.provenance.source_item.source == root
        && value_view.provenance.source_item.item
            == value_view.declaration.item
        && effective_attributes_are(&graph, graph_result.revision,
            graph_result.root, &value_view, value_attributes, 1u,
            &value_attribute)
        && cm_module_graph_borrow_item_ast(&graph, graph_result.root,
            value_view.declaration, &value_ast)
        && (value_declaration = cm_ast_get_item(value_ast,
            value_view.declaration.item)) != NULL
        && value_declaration->kind == CM_AST_ITEM_CONST
        && value_declaration->data.value_item.initializer
            != CM_AST_EXPR_NONE
        && cm_ast_get_expr(value_ast,
            value_declaration->data.value_item.initializer) != NULL,
        "inherent associated const graph identity differs");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower inherent const: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    number_item = find_hir_item_anywhere(&hir, "Number");
    impl_item = find_hir_impl(&hir);
    value_item = impl_item == NULL ? NULL
        : find_hir_associated_item(&hir, impl_item->definition,
            CM_HIR_ITEM_CONST, "VALUE");
    impl_self = impl_item == NULL ? NULL
        : cm_hir_get_type(&hir, impl_item->data.impl_item.self_type);
    value_type = value_item == NULL ? NULL
        : cm_hir_get_type(&hir, value_item->data.value_item.type);
    value_body = value_item == NULL ? NULL
        : cm_hir_get_body(&hir, value_item->data.value_item.body);
    check(result.error_count == 0u && result.lowered_item_count == 3u
        && hir.items.len == 3u && hir.definitions.len == 4u
        && number_item != NULL && impl_item != NULL && value_item != NULL,
        "inherent impl and associated const did not lower exactly once");
    check(number_item != NULL && impl_item != NULL
        && impl_item->kind == CM_HIR_ITEM_IMPL
        && !impl_item->data.impl_item.has_trait
        && !impl_item->data.impl_item.is_negative
        && impl_item->data.impl_item.safety == CM_HIR_SAFE
        && cm_hir_def_id_is_none(
            impl_item->data.impl_item.trait_type.definition)
        && impl_item->data.impl_item.trait_type.argument_count == 0u
        && impl_item->data.impl_item.trait_type.arguments == NULL
        && impl_self != NULL && impl_self->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(impl_self->data.named_type.definition,
            number_item->definition),
        "inherent impl header invented trait identity or lost its self type");
    check(value_item != NULL && value_declaration != NULL
        && value_item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_equal(value_item->parent_definition,
            impl_item->definition)
        && hir_attributes_match_graph(&graph, &hir, value_item,
            &value_attribute, 1u)
        && value_type != NULL
        && value_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && value_type->data.integer_type.kind == CM_HIR_INT_U8
        && value_body != NULL
        && value_body->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_def_id_equal(value_body->owner, value_item->definition)
        && value_body->expected_type == value_item->data.value_item.type
        && value_body->source == root
        && value_body->source_expression_id
            == value_declaration->data.value_item.initializer
        && value_body->span.source == root
        && value_body->span.start == value_view.span.start
        && value_body->span.end == value_view.span.end,
        "associated const lost parent, type, attribute, or body identity");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_dependency_generated_declarations(void)
{
    static const unsigned char dependency_source[] =
        "#[macro_export]\n"
        "macro_rules! make { () => { pub struct FromDependency; }; }\n"
        "pub mod api { pub use crate::make; }\n";
    static const unsigned char consumer_source[] =
        "use dep::api::make as alias;\n"
        "alias!();\n";
    static const unsigned char rejected_consumer_source[] =
        "use dep::api::{make, Missing};\n"
        "make!();\n";
    static const char included_consumer_root_source[] =
        "#[rustc_builtin_macro]\n"
        "macro_rules! include { ($file:expr) => {}; }\n"
        "use dep::api::make as alias;\n"
        "include!(\"part.rs\");\n";
    static const char included_consumer_part_source[] =
        "alias!();\n";
    char directory[] = "/tmp/cmrustc-hir-dependency-include-XXXXXX";
    char included_root_path[256];
    char included_part_path[256];
    CmSourceSet dependency_sources;
    CmSourceSet consumer_sources;
    CmSourceSet rejected_consumer_sources;
    CmSourceSet included_consumer_sources;
    CmSourceId dependency_root;
    CmSourceId consumer_root;
    CmSourceId rejected_consumer_root;
    CmSourceId included_consumer_root;
    CmModuleGraph dependency_graph;
    CmModuleGraph consumer_graph;
    CmModuleGraph rejected_consumer_graph;
    CmModuleGraph included_consumer_graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult dependency_result;
    CmModuleGraphResult consumer_result;
    CmModuleGraphResult rejected_consumer_result;
    CmModuleGraphResult included_consumer_result;
    CmDependencyMacroArtifact artifact;
    CmDependencyMacroArtifactResult artifact_result;
    const CmDependencyMacroArtifact *artifacts[1];
    CmCfgSet cfg;
    CmResolveEffectiveItem generated_view;
    CmResolveEffectiveItem included_generated_view;
    CmResolveImport macro_import;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmHirModuleId root_hir;
    const CmHirItem *generated_item;

    if (mkdtemp(directory) == NULL) {
        check(0, "could not create dependency include fixture");
        return;
    }
    (void)snprintf(included_root_path, sizeof(included_root_path),
        "%s/lib.rs", directory);
    (void)snprintf(included_part_path, sizeof(included_part_path),
        "%s/part.rs", directory);
    if (!write_text(included_root_path, included_consumer_root_source)
        || !write_text(included_part_path, included_consumer_part_source)) {
        check(0, "could not write dependency include fixture");
        (void)unlink(included_part_path);
        (void)unlink(included_root_path);
        (void)rmdir(directory);
        return;
    }
    cm_source_set_init(&dependency_sources);
    cm_source_set_init(&consumer_sources);
    cm_source_set_init(&rejected_consumer_sources);
    cm_source_set_init(&included_consumer_sources);
    cm_module_graph_init(&dependency_graph);
    cm_module_graph_init(&consumer_graph);
    cm_module_graph_init(&rejected_consumer_graph);
    cm_module_graph_init(&included_consumer_graph);
    cm_dependency_macro_artifact_init(&artifact);
    check(cm_source_add_memory(&dependency_sources,
            "dependency-generated/dep.rs", dependency_source,
            sizeof(dependency_source) - 1u, &dependency_root)
            == CM_SOURCE_OK
        && cm_source_add_memory(&consumer_sources,
            "dependency-generated/consumer.rs", consumer_source,
            sizeof(consumer_source) - 1u, &consumer_root) == CM_SOURCE_OK
        && cm_source_add_memory(&rejected_consumer_sources,
            "dependency-generated/rejected-consumer.rs",
            rejected_consumer_source,
            sizeof(rejected_consumer_source) - 1u,
            &rejected_consumer_root) == CM_SOURCE_OK
        && cm_source_load_file(&included_consumer_sources,
            included_root_path, &included_consumer_root) == CM_SOURCE_OK,
        "could not add dependency-generated declaration fixtures");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    dependency_result = cm_module_graph_build(&dependency_graph,
        &dependency_sources, dependency_root, &graph_options);
    artifact_result = cm_dependency_macro_artifact_build(&artifact,
        &dependency_graph, dependency_result.revision, "dep", "rust_dep");
    artifacts[0] = &artifact;
    graph_options.dependency_macros = artifacts;
    graph_options.dependency_macro_count = 1u;
    consumer_result = cm_module_graph_build(&consumer_graph,
        &consumer_sources, consumer_root, &graph_options);
    rejected_consumer_result = cm_module_graph_build(
        &rejected_consumer_graph, &rejected_consumer_sources,
        rejected_consumer_root, &graph_options);
    included_consumer_result = cm_module_graph_build(
        &included_consumer_graph, &included_consumer_sources,
        included_consumer_root, &graph_options);
    memset(&generated_view, 0, sizeof(generated_view));
    memset(&included_generated_view, 0, sizeof(included_generated_view));
    memset(&macro_import, 0, sizeof(macro_import));
    check(dependency_result.error_count == 0u
        && artifact_result.status == CM_DEPENDENCY_MACRO_OK
        && consumer_result.error_count == 0u
        && rejected_consumer_result.error_count == 0u
        && included_consumer_result.error_count == 0u,
        "dependency-generated fixture graphs did not build");
    check(cm_module_graph_get_import(&consumer_graph,
            consumer_result.root, 0u, &macro_import)
        && effective_item_named(&consumer_graph, consumer_result.revision,
            consumer_result.root, "FromDependency", &generated_view)
        && generated_view.is_generated
        && generated_view.provenance.macro_invocation.source
            == consumer_root
        && generated_view.provenance.macro_invocation.item
            != CM_AST_ITEM_NONE
        && generated_view.provenance.macro_definition.source == 0u
        && generated_view.provenance.macro_definition.item
            == CM_AST_ITEM_NONE
        && generated_view.provenance.dependency_macro_definition.certificate
            != CM_RESOLVE_DEPENDENCY_CERTIFICATE_NONE
        && cm_module_graph_validate_dependency_macro_definition(
            &consumer_graph, consumer_result.revision,
            generated_view.provenance.dependency_macro_definition)
            == CM_RESOLVE_VIEW_OK,
        "dependency-generated declaration was not authenticated");
    check(effective_item_named(&included_consumer_graph,
            included_consumer_result.revision,
            included_consumer_result.root, "FromDependency",
            &included_generated_view),
        "include-origin dependency declaration was not published");
    check(included_generated_view.is_generated,
        "include-origin dependency declaration was not generated");
    check(cm_module_graph_validate_dependency_macro_import(&consumer_graph,
            consumer_result.revision, consumer_result.root,
            macro_import.declaration, (const unsigned char *)"alias", 5u)
            == CM_RESOLVE_VIEW_OK
        && cm_module_graph_validate_dependency_macro_import(&consumer_graph,
            consumer_result.revision, consumer_result.root,
            macro_import.declaration, (const unsigned char *)"make", 4u)
            == CM_RESOLVE_VIEW_NOT_FOUND
        && cm_module_graph_validate_dependency_macro_import(&consumer_graph,
            consumer_result.revision + 1u, consumer_result.root,
            macro_import.declaration, (const unsigned char *)"alias", 5u)
            == CM_RESOLVE_VIEW_STALE_REVISION,
        "dependency macro import authentication accepted a wrong leaf");

    cm_dependency_macro_artifact_destroy(&artifact);
    cm_module_graph_destroy(&dependency_graph);
    cm_source_set_destroy(&dependency_sources);
    check(cm_module_graph_validate_dependency_macro_definition(
            &consumer_graph, consumer_result.revision,
        generated_view.provenance.dependency_macro_definition)
            == CM_RESOLVE_VIEW_OK,
        "consumer dependency certificate requires a live producer graph");
    check(cm_module_graph_validate_dependency_macro_import(&consumer_graph,
            consumer_result.revision, consumer_result.root,
            macro_import.declaration, (const unsigned char *)"alias", 5u)
            == CM_RESOLVE_VIEW_OK,
        "consumer macro import requires a live producer graph");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&lower_options);
    lower_result = lower_module_graph(&hir, &consumer_graph,
        consumer_result.revision, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower dependency-generated: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    root_hir = CM_HIR_MODULE_NONE;
    check(lower_result.error_count == 0u
        && lower_result.lowered_item_count == 1u
        && cm_hir_module_map_lookup_hir(&map, &consumer_graph,
            consumer_result.revision, consumer_result.root, &hir,
            &root_hir) == CM_HIR_MODULE_MAP_OK,
        "dependency-generated declaration did not lower or map its root");
    generated_item = find_hir_item(&hir, root_hir, "FromDependency");
    check(generated_item != NULL
        && generated_item->kind == CM_HIR_ITEM_STRUCT
        && generated_item->span.source == consumer_root
        && generated_item->span.start == generated_view.span.start
        && generated_item->span.end == generated_view.span.end,
        "dependency-generated HIR item lost consumer-owned syntax or span");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    lower_result = lower_module_graph(&hir, &included_consumer_graph,
        included_consumer_result.revision, &map, &lower_options);
    root_hir = CM_HIR_MODULE_NONE;
    check(lower_result.error_count == 0u
        && lower_result.lowered_item_count == 1u
        && cm_hir_module_map_lookup_hir(&map, &included_consumer_graph,
            included_consumer_result.revision,
            included_consumer_result.root, &hir,
            &root_hir) == CM_HIR_MODULE_MAP_OK
        && find_hir_item(&hir, root_hir, "FromDependency") != NULL,
        "include-origin dependency macro invocation did not lower");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    lower_result = lower_module_graph(&hir, &rejected_consumer_graph,
        rejected_consumer_result.revision, &map, &lower_options);
    check(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "dependency macro import hid an unrelated unresolved import");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&included_consumer_graph);
    cm_module_graph_destroy(&rejected_consumer_graph);
    cm_module_graph_destroy(&consumer_graph);
    cm_source_set_destroy(&rejected_consumer_sources);
    cm_source_set_destroy(&consumer_sources);
    cm_source_set_destroy(&included_consumer_sources);
    (void)unlink(included_part_path);
    (void)unlink(included_root_path);
    (void)rmdir(directory);
}

static void test_hir_library_artifact_types(void)
{
    static const unsigned char producer_source[] =
        "mod hidden {\n"
        "    pub struct Shared;\n"
        "    pub type Alias = Shared;\n"
        "    pub trait Inner {}\n"
        "    pub mod api { pub struct Nested; pub trait NestedTrait {} }\n"
        "}\n"
        "pub use hidden::Shared as Exported;\n"
        "pub use hidden::Alias;\n"
        "pub use hidden::Inner as ExportedTrait;\n"
        "pub use hidden::api as exposed;\n"
        "pub trait Marker {}\n"
        "pub trait Generic<T> {}\n"
        "pub trait WithAssoc { type Item; }\n"
        "pub trait WithMethod { fn run(&self); }\n"
        "pub mod primitive { pub use bool; pub use u8; }\n"
        "pub struct Root;\n"
        "struct PrivateRoot;\n";
    static const unsigned char consumer_source[] =
        "use dep::{Root as ImportedRoot, Exported as Imported, "
        "ExportedTrait as ImportedTrait, exposed as ImportedModule, "
        "primitive::u8 as Byte};\n"
        "pub struct Uses {\n"
        "    root: dep::Root,\n"
        "    exported: dep::Exported,\n"
        "    alias: dep::Alias,\n"
        "    imported_root: ImportedRoot,\n"
        "    imported: Imported,\n"
        "    nested: dep::exposed::Nested,\n"
        "    nested_imported: ImportedModule::Nested,\n"
        "    primitive_qualified: dep::primitive::bool,\n"
        "    primitive_imported: Byte,\n"
        "}\n"
        "pub struct Local;\n"
        "impl dep::Marker for Local {}\n"
        "impl ImportedTrait for Local {}\n"
        "impl ImportedModule::NestedTrait for Local {}\n"
        "impl dep::WithAssoc for Local { type Item = dep::Root; }\n"
        "impl dep::WithMethod for Local { fn run(&self) {} }\n"
        "pub type Project = <Local as dep::WithAssoc>::Item;\n"
        "pub trait Bounded: dep::Marker {}\n"
        "pub trait GenericBound: dep::Generic<dep::Root> {}\n"
        "pub trait BoundAssoc { type Output: dep::Marker; }\n";
    static const unsigned char private_consumer_source[] =
        "use dep::{Root, hidden::Inner};\n"
        "struct Rejected; impl Inner for Rejected {}\n";
    static const unsigned char unrelated_consumer_source[] =
        "use dep::Root;\n"
        "use unrelated::Missing;\n"
        "struct Rejected { field: Root }\n";
    static const unsigned char collision_consumer_source[] =
        "mod ImportedModule { pub struct Nested; }\n"
        "trait ImportedTrait {}\n"
        "use dep::{exposed as ImportedModule, "
        "ExportedTrait as ImportedTrait};\n"
        "struct Rejected { field: ImportedModule::Nested }\n";
    static const unsigned char cross_equality_consumer_source[] =
        "pub trait Rejected: dep::WithAssoc<Item = Self> {}\n";
    static const unsigned char *const root_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"Root"
    };
    static const size_t root_path_lengths[] = { 3u, 4u };
    static const unsigned char *const exported_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"Exported"
    };
    static const size_t exported_path_lengths[] = { 3u, 8u };
    static const unsigned char *const alias_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"Alias"
    };
    static const size_t alias_path_lengths[] = { 3u, 5u };
    static const unsigned char *const nested_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"exposed",
        (const unsigned char *)"Nested"
    };
    static const size_t nested_path_lengths[] = { 3u, 7u, 6u };
    static const unsigned char *const private_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"hidden",
        (const unsigned char *)"Shared"
    };
    static const size_t private_path_lengths[] = { 3u, 6u, 6u };
    static const unsigned char *const marker_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"Marker"
    };
    static const size_t marker_path_lengths[] = { 3u, 6u };
    static const unsigned char *const exported_trait_path_bytes[] = {
        (const unsigned char *)"dep",
        (const unsigned char *)"ExportedTrait"
    };
    static const size_t exported_trait_path_lengths[] = { 3u, 13u };
    static const unsigned char *const private_trait_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"hidden",
        (const unsigned char *)"Inner"
    };
    static const size_t private_trait_path_lengths[] = { 3u, 6u, 5u };
    static const unsigned char *const primitive_bool_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"primitive",
        (const unsigned char *)"bool"
    };
    static const size_t primitive_bool_path_lengths[] = { 3u, 9u, 4u };
    static const unsigned char *const primitive_u8_path_bytes[] = {
        (const unsigned char *)"dep", (const unsigned char *)"primitive",
        (const unsigned char *)"u8"
    };
    static const size_t primitive_u8_path_lengths[] = { 3u, 9u, 2u };
    CmSourceSet producer_sources;
    CmSourceSet consumer_sources;
    CmSourceSet private_sources;
    CmSourceSet unrelated_sources;
    CmSourceSet collision_sources;
    CmSourceSet cross_equality_sources;
    CmSourceId producer_root;
    CmSourceId consumer_root;
    CmSourceId private_root;
    CmSourceId unrelated_root;
    CmSourceId collision_root;
    CmSourceId cross_equality_root;
    CmModuleGraph producer_graph;
    CmModuleGraph consumer_graph;
    CmModuleGraph private_graph;
    CmModuleGraph unrelated_graph;
    CmModuleGraph collision_graph;
    CmModuleGraph cross_equality_graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult producer_graph_result;
    CmModuleGraphResult consumer_graph_result;
    CmModuleGraphResult private_graph_result;
    CmModuleGraphResult unrelated_graph_result;
    CmModuleGraphResult collision_graph_result;
    CmModuleGraphResult cross_equality_graph_result;
    CmCfgSet cfg;
    CmHirContext hir;
    CmHirModuleMap producer_map;
    CmHirModuleMap consumer_map;
    CmHirModuleMap private_map;
    CmHirModuleMap unrelated_map;
    CmHirModuleMap collision_map;
    CmHirModuleMap cross_equality_map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult producer_lower;
    CmHirLowerResult consumer_lower;
    CmHirLowerResult private_lower;
    CmHirLowerResult unrelated_lower;
    CmHirLowerResult collision_lower;
    CmHirLowerResult cross_equality_lower;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult artifact_result;
    CmHirLibraryArtifactIdentity identity;
    const CmHirLibraryArtifact *libraries[2];
    CmHirLibraryPathSegment root_path[2];
    CmHirLibraryPathSegment exported_path[2];
    CmHirLibraryPathSegment alias_path[2];
    CmHirLibraryPathSegment nested_path[3];
    CmHirLibraryPathSegment private_path[3];
    CmHirLibraryPathSegment marker_path[2];
    CmHirLibraryPathSegment exported_trait_path[2];
    CmHirLibraryPathSegment private_trait_path[3];
    CmHirLibraryPathSegment primitive_bool_path[3];
    CmHirLibraryPathSegment primitive_u8_path[3];
    CmHirLibraryType root_type;
    CmHirLibraryType exported_type;
    CmHirLibraryType alias_type;
    CmHirLibraryType nested_type;
    CmHirLibraryBinding trait_binding;
    CmHirLibraryBinding primitive_binding;
    CmHirLibraryType primitive_type;
    const CmHirItem *producer_root_item;
    const CmHirItem *shared_item;
    const CmHirItem *alias_item;
    const CmHirItem *nested_item;
    const CmHirItem *marker_item;
    const CmHirItem *inner_trait_item;
    const CmHirItem *nested_trait_item;
    const CmHirItem *with_assoc_item;
    const CmHirItem *with_assoc_associated_item;
    const CmHirItem *generic_trait_item;
    const CmHirItem *with_method_item;
    const CmHirItem *with_method_declaration;
    const CmHirModule *exposed_module;
    const CmHirItem *uses_item;
    const CmHirItem *local_item;
    const CmHirItem *bounded_item;
    const CmHirItem *bound_assoc_item;
    const CmHirItem *generic_bound_item;
    const CmHirItem *bound_output_item;
    const CmHirItem *project_item;
    CmHirModuleId consumer_root_hir;
    const CmHirModule *consumer_root_module;
    CmHirDefId producer_root_definition;
    CmHirDefId shared_definition;
    CmHirDefId alias_definition;
    CmHirDefId nested_definition;
    CmHirDefId marker_definition;
    CmHirDefId inner_trait_definition;
    CmHirDefId nested_trait_definition;
    CmHirDefId with_assoc_definition;
    CmHirDefId with_assoc_associated_definition;
    CmHirDefId generic_trait_definition;
    CmHirDefId with_method_definition;
    CmHirDefId with_method_declaration_definition;
    CmHirDefId exposed_module_definition;
    size_t saved_crates;
    size_t saved_modules;
    size_t saved_items;
    size_t saved_types;
    size_t saved_definitions;
    size_t saved_strings;
    size_t index;
    uint32_t matched_trait_impls;
    int method_linked;

    cm_source_set_init(&producer_sources);
    cm_source_set_init(&consumer_sources);
    cm_source_set_init(&private_sources);
    cm_source_set_init(&unrelated_sources);
    cm_source_set_init(&collision_sources);
    cm_source_set_init(&cross_equality_sources);
    cm_module_graph_init(&producer_graph);
    cm_module_graph_init(&consumer_graph);
    cm_module_graph_init(&private_graph);
    cm_module_graph_init(&unrelated_graph);
    cm_module_graph_init(&collision_graph);
    cm_module_graph_init(&cross_equality_graph);
    cm_cfg_set_init(&cfg);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&producer_map);
    cm_hir_module_map_init(&consumer_map);
    cm_hir_module_map_init(&private_map);
    cm_hir_module_map_init(&unrelated_map);
    cm_hir_module_map_init(&collision_map);
    cm_hir_module_map_init(&cross_equality_map);
    cm_hir_library_artifact_init(&artifact);
    check(cm_source_add_memory(&producer_sources,
            "hir-library/producer.rs", producer_source,
            sizeof(producer_source) - 1u, &producer_root) == CM_SOURCE_OK
        && cm_source_add_memory(&consumer_sources,
            "hir-library/consumer.rs", consumer_source,
            sizeof(consumer_source) - 1u, &consumer_root) == CM_SOURCE_OK
        && cm_source_add_memory(&private_sources,
            "hir-library/private.rs", private_consumer_source,
            sizeof(private_consumer_source) - 1u,
            &private_root) == CM_SOURCE_OK
        && cm_source_add_memory(&unrelated_sources,
            "hir-library/unrelated.rs", unrelated_consumer_source,
            sizeof(unrelated_consumer_source) - 1u,
            &unrelated_root) == CM_SOURCE_OK
        && cm_source_add_memory(&collision_sources,
            "hir-library/collision.rs", collision_consumer_source,
            sizeof(collision_consumer_source) - 1u,
            &collision_root) == CM_SOURCE_OK
        && cm_source_add_memory(&cross_equality_sources,
            "hir-library/cross-equality.rs",
            cross_equality_consumer_source,
            sizeof(cross_equality_consumer_source) - 1u,
            &cross_equality_root) == CM_SOURCE_OK,
        "could not add HIR library artifact fixtures");
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    producer_graph_result = cm_module_graph_build(&producer_graph,
        &producer_sources, producer_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "producer";
    producer_lower = lower_module_graph(&hir, &producer_graph,
        producer_graph_result.revision, &producer_map, &lower_options);
    check(producer_graph_result.error_count == 0u
        && producer_lower.error_count == 0u,
        "HIR library producer did not lower");
    producer_root_item = find_hir_item_anywhere(&hir, "Root");
    shared_item = find_hir_item_anywhere(&hir, "Shared");
    alias_item = find_hir_item_anywhere(&hir, "Alias");
    nested_item = find_hir_item_anywhere(&hir, "Nested");
    marker_item = find_hir_item_anywhere(&hir, "Marker");
    inner_trait_item = find_hir_item_anywhere(&hir, "Inner");
    nested_trait_item = find_hir_item_anywhere(&hir, "NestedTrait");
    with_assoc_item = find_hir_item_anywhere(&hir, "WithAssoc");
    with_assoc_associated_item = with_assoc_item == NULL ? NULL
        : find_hir_associated_item(&hir, with_assoc_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Item");
    generic_trait_item = find_hir_item_anywhere(&hir, "Generic");
    with_method_item = find_hir_item_anywhere(&hir, "WithMethod");
    with_method_declaration = with_method_item == NULL ? NULL
        : find_hir_associated_item(&hir, with_method_item->definition,
            CM_HIR_ITEM_FUNCTION, "run");
    exposed_module = nested_item == NULL ? NULL
        : cm_hir_get_module(&hir, nested_item->owner_module);
    producer_root_definition = producer_root_item == NULL
        ? cm_hir_def_id_none() : producer_root_item->definition;
    shared_definition = shared_item == NULL
        ? cm_hir_def_id_none() : shared_item->definition;
    alias_definition = alias_item == NULL
        ? cm_hir_def_id_none() : alias_item->definition;
    nested_definition = nested_item == NULL
        ? cm_hir_def_id_none() : nested_item->definition;
    marker_definition = marker_item == NULL
        ? cm_hir_def_id_none() : marker_item->definition;
    inner_trait_definition = inner_trait_item == NULL
        ? cm_hir_def_id_none() : inner_trait_item->definition;
    nested_trait_definition = nested_trait_item == NULL
        ? cm_hir_def_id_none() : nested_trait_item->definition;
    with_assoc_definition = with_assoc_item == NULL
        ? cm_hir_def_id_none() : with_assoc_item->definition;
    with_assoc_associated_definition = with_assoc_associated_item == NULL
        ? cm_hir_def_id_none() : with_assoc_associated_item->definition;
    generic_trait_definition = generic_trait_item == NULL
        ? cm_hir_def_id_none() : generic_trait_item->definition;
    with_method_definition = with_method_item == NULL
        ? cm_hir_def_id_none() : with_method_item->definition;
    with_method_declaration_definition = with_method_declaration == NULL
        ? cm_hir_def_id_none() : with_method_declaration->definition;
    exposed_module_definition = exposed_module == NULL
        ? cm_hir_def_id_none() : exposed_module->definition;
    check(producer_root_item != NULL && shared_item != NULL
        && alias_item != NULL
        && nested_item != NULL && marker_item != NULL
        && inner_trait_item != NULL && nested_trait_item != NULL
        && with_assoc_item != NULL && with_assoc_associated_item != NULL
        && generic_trait_item != NULL
        && with_method_item != NULL && with_method_declaration != NULL
        && exposed_module != NULL,
        "HIR library producer definitions are missing");
    artifact_result = cm_hir_library_artifact_build(&artifact, &hir,
        producer_lower.crate_id, &producer_graph,
        producer_graph_result.revision, &producer_map, "dep");
    memset(&identity, 0, sizeof(identity));
    check(artifact_result.status == CM_HIR_LIBRARY_OK
        && artifact_result.module_count == 4u
        && artifact_result.public_type_entry_count >= 6u
        && cm_hir_library_artifact_identity(&artifact, &identity)
        && identity.context == &hir
        && identity.crate_id == producer_lower.crate_id
        && strcmp(identity.extern_name, "dep") == 0,
        "HIR library artifact did not snapshot its producer identity");
    for (index = 0u; index < 2u; ++index) {
        root_path[index].bytes = root_path_bytes[index];
        root_path[index].length = root_path_lengths[index];
        exported_path[index].bytes = exported_path_bytes[index];
        exported_path[index].length = exported_path_lengths[index];
        alias_path[index].bytes = alias_path_bytes[index];
        alias_path[index].length = alias_path_lengths[index];
        marker_path[index].bytes = marker_path_bytes[index];
        marker_path[index].length = marker_path_lengths[index];
        exported_trait_path[index].bytes = exported_trait_path_bytes[index];
        exported_trait_path[index].length =
            exported_trait_path_lengths[index];
    }
    for (index = 0u; index < 3u; ++index) {
        nested_path[index].bytes = nested_path_bytes[index];
        nested_path[index].length = nested_path_lengths[index];
        private_path[index].bytes = private_path_bytes[index];
        private_path[index].length = private_path_lengths[index];
        private_trait_path[index].bytes = private_trait_path_bytes[index];
        private_trait_path[index].length = private_trait_path_lengths[index];
        primitive_bool_path[index].bytes = primitive_bool_path_bytes[index];
        primitive_bool_path[index].length =
            primitive_bool_path_lengths[index];
        primitive_u8_path[index].bytes = primitive_u8_path_bytes[index];
        primitive_u8_path[index].length = primitive_u8_path_lengths[index];
    }
    memset(&root_type, 0, sizeof(root_type));
    memset(&exported_type, 0, sizeof(exported_type));
    memset(&alias_type, 0, sizeof(alias_type));
    memset(&nested_type, 0, sizeof(nested_type));
    check(cm_hir_library_artifact_lookup_type(&artifact, root_path, 2u,
            &root_type) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(root_type.definition,
            producer_root_definition)
        && root_type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_library_artifact_lookup_type(&artifact, exported_path, 2u,
            &exported_type) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(exported_type.definition,
            shared_definition)
        && cm_hir_library_artifact_lookup_type(&artifact, alias_path, 2u,
            &alias_type) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(alias_type.definition,
            alias_definition)
        && alias_type.kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
        && cm_hir_library_artifact_lookup_type(&artifact, nested_path, 2u,
            &nested_type) == CM_HIR_LIBRARY_WRONG_NAMESPACE
        && cm_hir_library_artifact_lookup_type(&artifact, nested_path, 3u,
            &nested_type) == CM_HIR_LIBRARY_OK
        && cm_hir_def_id_equal(nested_type.definition,
            nested_definition)
        && cm_hir_library_artifact_lookup_type(&artifact, private_path, 3u,
            &root_type) == CM_HIR_LIBRARY_NOT_FOUND,
        "HIR library public-path snapshot accepted the wrong definition");
    memset(&trait_binding, 0, sizeof(trait_binding));
    check(cm_hir_library_artifact_lookup_binding(&artifact, marker_path, 2u,
            &trait_binding) == CM_HIR_LIBRARY_OK
        && trait_binding.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(trait_binding.definition, marker_definition)
        && cm_hir_library_artifact_lookup_binding(&artifact,
            exported_trait_path, 2u, &trait_binding) == CM_HIR_LIBRARY_OK
        && trait_binding.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(trait_binding.definition,
            inner_trait_definition)
        && cm_hir_library_artifact_lookup_binding(&artifact,
            private_trait_path, 3u,
            &trait_binding) == CM_HIR_LIBRARY_NOT_FOUND,
        "HIR library trait paths lost identity or exposed a private path");
    memset(&primitive_binding, 0, sizeof(primitive_binding));
    memset(&primitive_type, 0, sizeof(primitive_type));
    check(cm_hir_library_artifact_lookup_binding(&artifact,
            primitive_bool_path, 3u, &primitive_binding)
                == CM_HIR_LIBRARY_OK
        && primitive_binding.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
        && primitive_binding.primitive_kind == CM_HIR_PRIMITIVE_BOOL
        && cm_hir_def_id_is_none(primitive_binding.definition)
        && cm_hir_library_artifact_lookup_binding(&artifact,
            primitive_u8_path, 3u, &primitive_binding)
                == CM_HIR_LIBRARY_OK
        && primitive_binding.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE
        && primitive_binding.primitive_kind == CM_HIR_PRIMITIVE_U8
        && cm_hir_def_id_is_none(primitive_binding.definition)
        && cm_hir_library_artifact_lookup_type(&artifact,
            primitive_u8_path, 3u, &primitive_type) == CM_HIR_LIBRARY_OK
        && primitive_type.primitive_kind == CM_HIR_PRIMITIVE_U8
        && cm_hir_def_id_is_none(primitive_type.definition),
        "HIR library primitive reexports lost identity or invented DefIds");

    cm_hir_module_map_destroy(&producer_map);
    cm_module_graph_destroy(&producer_graph);
    cm_source_set_destroy(&producer_sources);
    consumer_graph_result = cm_module_graph_build(&consumer_graph,
        &consumer_sources, consumer_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "consumer";
    libraries[0] = &artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    consumer_lower = lower_module_graph(&hir, &consumer_graph,
        consumer_graph_result.revision, &consumer_map, &lower_options);
    if (consumer_lower.error_count != 0u) {
        fprintf(stderr, "hir-graph-lower library consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(consumer_lower.first_error.kind),
            consumer_lower.first_error.message);
    }
    uses_item = find_hir_item_anywhere(&hir, "Uses");
    local_item = find_hir_item_anywhere(&hir, "Local");
    bounded_item = find_hir_item_anywhere(&hir, "Bounded");
    bound_assoc_item = find_hir_item_anywhere(&hir, "BoundAssoc");
    generic_bound_item = find_hir_item_anywhere(&hir, "GenericBound");
    bound_output_item = bound_assoc_item == NULL ? NULL
        : find_hir_associated_item(&hir, bound_assoc_item->definition,
            CM_HIR_ITEM_TYPE_ALIAS, "Output");
    project_item = find_hir_item_anywhere(&hir, "Project");
    consumer_root_hir = CM_HIR_MODULE_NONE;
    consumer_root_module = NULL;
    if (cm_hir_module_map_lookup_hir(&consumer_map, &consumer_graph,
            consumer_graph_result.revision, consumer_graph_result.root,
            &hir, &consumer_root_hir) == CM_HIR_MODULE_MAP_OK) {
        consumer_root_module = cm_hir_get_module(&hir, consumer_root_hir);
    }
    check(consumer_graph_result.error_count == 0u
        && consumer_lower.error_count == 0u && uses_item != NULL
        && local_item != NULL && bounded_item != NULL
        && bound_assoc_item != NULL && bound_output_item != NULL
        && generic_bound_item != NULL
        && project_item != NULL
        && uses_item->kind == CM_HIR_ITEM_STRUCT
        && uses_item->data.aggregate_item.field_count == 9u
        && consumer_root_module != NULL
        && consumer_root_module->import_count == 1u
        && consumer_root_module->imports[0].binding_count == 5u
        && hir_import_binding_is(&hir, &consumer_root_module->imports[0],
            0u, CM_HIR_NAMESPACE_TYPE, "ImportedRoot",
            producer_root_definition)
        && hir_import_binding_is(&hir, &consumer_root_module->imports[0],
            1u, CM_HIR_NAMESPACE_TYPE, "Imported", shared_definition)
        && hir_import_binding_is(&hir, &consumer_root_module->imports[0],
            2u, CM_HIR_NAMESPACE_TYPE, "ImportedTrait",
            inner_trait_definition)
        && hir_import_binding_is(&hir, &consumer_root_module->imports[0],
            3u, CM_HIR_NAMESPACE_TYPE, "ImportedModule",
            exposed_module_definition)
        && hir_import_binding_is(&hir, &consumer_root_module->imports[0],
            4u, CM_HIR_NAMESPACE_TYPE, "Byte", cm_hir_def_id_none())
        && consumer_root_module->imports[0].bindings[4].primitive_kind
            == CM_HIR_PRIMITIVE_U8,
        "artifact-backed dependency types did not lower");
    if (uses_item != NULL && uses_item->kind == CM_HIR_ITEM_STRUCT
        && uses_item->data.aggregate_item.field_count == 9u) {
        const CmHirType *field_type;

        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[0].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                producer_root_definition),
            "direct dependency type lost its producer DefId");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[1].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                shared_definition),
            "dependency type reexport lost its producer DefId");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[2].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                shared_definition),
            "dependency type alias did not normalize through producer HIR");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[3].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                producer_root_definition),
            "first grouped dependency import lost its producer DefId");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[4].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                shared_definition),
            "imported dependency type lost its producer DefId");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[5].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                nested_definition),
            "dependency module reexport lost its producer DefId");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[6].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(field_type->data.named_type.definition,
                nested_definition),
            "imported dependency module lost qualified type lookup");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[7].type);
        check(field_type != NULL && field_type->kind == CM_HIR_TYPE_BOOL_KIND,
            "qualified dependency primitive lowered to the wrong type");
        field_type = cm_hir_get_type(&hir,
            uses_item->data.aggregate_item.fields[8].type);
        check(field_type != NULL
            && field_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && field_type->data.integer_type.kind == CM_HIR_INT_U8,
            "imported dependency primitive lowered to the wrong type");
    }
    matched_trait_impls = 0u;
    method_linked = 0;
    for (index = 0u; index < hir.items.len; ++index) {
        const CmHirItem *item;
        const CmHirType *self_type;
        CmHirDefId trait_definition;

        item = (const CmHirItem *)cm_vec_at_const(&hir.items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || !item->data.impl_item.has_trait || local_item == NULL) {
            continue;
        }
        self_type = cm_hir_get_type(&hir, item->data.impl_item.self_type);
        if (self_type == NULL || self_type->kind != CM_HIR_TYPE_ADT_KIND
            || !cm_hir_def_id_equal(self_type->data.named_type.definition,
                local_item->definition)) continue;
        trait_definition = item->data.impl_item.trait_type.definition;
        if (cm_hir_def_id_equal(trait_definition, marker_definition)
            || cm_hir_def_id_equal(trait_definition,
                inner_trait_definition)
            || cm_hir_def_id_equal(trait_definition,
                nested_trait_definition)
            || cm_hir_def_id_equal(trait_definition,
                with_assoc_definition)
            || cm_hir_def_id_equal(trait_definition,
                with_method_definition)) {
            matched_trait_impls += 1u;
        }
        if (cm_hir_def_id_equal(trait_definition, with_method_definition)
            && with_method_declaration != NULL) {
            const CmHirItem *implementation;

            implementation = find_hir_associated_item(&hir,
                item->definition, CM_HIR_ITEM_FUNCTION, "run");
            method_linked = implementation != NULL
                && cm_hir_def_id_equal(implementation->data.function_item
                        .trait_item_definition,
                    with_method_declaration_definition);
        }
    }
    check(matched_trait_impls == 5u && method_linked
        && bounded_item != NULL
        && bounded_item->kind == CM_HIR_ITEM_TRAIT
        && bounded_item->data.trait_item.supertrait_count == 1u
        && cm_hir_def_id_equal(
            bounded_item->data.trait_item.supertraits[0]
                .trait_type.definition,
            marker_definition),
        "dependency impls or generic bounds lost producer trait DefIds");
    if (project_item != NULL && bound_output_item != NULL) {
        const CmHirType *projection;
        const CmHirType *generic_argument;

        projection = cm_hir_get_type(&hir,
            project_item->data.type_alias_item.target);
        generic_argument = generic_bound_item == NULL
                || generic_bound_item->kind != CM_HIR_ITEM_TRAIT
                || generic_bound_item->data.trait_item.supertrait_count != 1u
                || generic_bound_item->data.trait_item.supertraits[0]
                    .trait_type.argument_count != 1u
            ? NULL : cm_hir_get_type(&hir,
                generic_bound_item->data.trait_item.supertraits[0]
                    .trait_type.arguments[0].data.type);
        check(projection != NULL
            && projection->kind == CM_HIR_TYPE_PROJECTION_KIND
            && cm_hir_def_id_equal(projection->data.projection_type
                    .trait_type.definition,
                with_assoc_definition)
            && cm_hir_def_id_equal(projection->data.projection_type
                    .associated_type.definition,
                with_assoc_associated_definition)
            && bound_output_item->data.type_alias_item.bound_count == 1u
            && cm_hir_def_id_equal(bound_output_item->data.type_alias_item
                    .bounds[0].trait_type.definition,
                marker_definition)
            && generic_bound_item != NULL
            && generic_bound_item->kind == CM_HIR_ITEM_TRAIT
            && generic_bound_item->data.trait_item.supertrait_count == 1u
            && generic_bound_item->data.trait_item.supertraits[0]
                .trait_type.argument_count == 1u
            && generic_bound_item->data.trait_item.supertraits[0]
                .trait_type.arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE
            && cm_hir_def_id_equal(generic_bound_item->data.trait_item
                    .supertraits[0].trait_type.definition,
                generic_trait_definition)
            && generic_argument != NULL
            && generic_argument->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(generic_argument->data.named_type.definition,
                producer_root_definition),
            "dependency associated projection or bound lost producer DefIds");
    }

    saved_crates = hir.crates.len;
    saved_modules = hir.modules.len;
    saved_items = hir.items.len;
    saved_types = hir.types.len;
    saved_definitions = hir.definitions.len;
    saved_strings = cm_interner_length(&hir.strings);
    private_graph_result = cm_module_graph_build(&private_graph,
        &private_sources, private_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "private_consumer";
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    private_lower = lower_module_graph(&hir, &private_graph,
        private_graph_result.revision, &private_map, &lower_options);
    check(private_lower.error_count == 1u
        && private_lower.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
        && cm_hir_module_map_count(&private_map) == 0u
        && hir.crates.len == saved_crates
        && hir.modules.len == saved_modules
        && hir.items.len == saved_items
        && hir.types.len == saved_types
        && hir.definitions.len == saved_definitions
        && cm_interner_length(&hir.strings) == saved_strings,
        "private dependency trait path did not reject transactionally");
    unrelated_graph_result = cm_module_graph_build(&unrelated_graph,
        &unrelated_sources, unrelated_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "unrelated_consumer";
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    unrelated_lower = lower_module_graph(&hir, &unrelated_graph,
        unrelated_graph_result.revision, &unrelated_map, &lower_options);
    check(unrelated_lower.error_count == 1u
        && unrelated_lower.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
        && cm_hir_module_map_count(&unrelated_map) == 0u
        && hir.crates.len == saved_crates
        && hir.modules.len == saved_modules
        && hir.items.len == saved_items
        && hir.types.len == saved_types
        && hir.definitions.len == saved_definitions
        && cm_interner_length(&hir.strings) == saved_strings,
        "authenticated dependency import hid an unrelated resolver error");
    collision_graph_result = cm_module_graph_build(&collision_graph,
        &collision_sources, collision_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "collision_consumer";
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    collision_lower = lower_module_graph(&hir, &collision_graph,
        collision_graph_result.revision, &collision_map, &lower_options);
    check(collision_lower.error_count == 1u
        && collision_lower.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
        && cm_hir_module_map_count(&collision_map) == 0u
        && hir.crates.len == saved_crates
        && hir.modules.len == saved_modules
        && hir.items.len == saved_items
        && hir.types.len == saved_types
        && hir.definitions.len == saved_definitions
        && cm_interner_length(&hir.strings) == saved_strings,
        "dependency module or trait import displaced a local binding");
    cross_equality_graph_result = cm_module_graph_build(
        &cross_equality_graph, &cross_equality_sources,
        cross_equality_root, &graph_options);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "cross_equality_consumer";
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    cross_equality_lower = lower_module_graph(&hir,
        &cross_equality_graph, cross_equality_graph_result.revision,
        &cross_equality_map, &lower_options);
    check(cross_equality_graph_result.error_count == 0u
        && cross_equality_lower.error_count == 1u
        && cross_equality_lower.first_error.kind
            == CM_HIR_LOWER_HIR_FAILURE
        && cm_hir_module_map_count(&cross_equality_map) == 0u
        && hir.crates.len == saved_crates
        && hir.modules.len == saved_modules
        && hir.items.len == saved_items
        && hir.types.len == saved_types
        && hir.definitions.len == saved_definitions
        && cm_interner_length(&hir.strings) == saved_strings,
        "cross-crate supertrait equality did not reject transactionally");
    cm_hir_module_map_destroy(&private_map);
    cm_hir_module_map_init(&private_map);
    libraries[1] = &artifact;
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "duplicate_consumer";
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 2u;
    private_lower = lower_module_graph(&hir, &private_graph,
        private_graph_result.revision, &private_map, &lower_options);
    check(private_lower.error_count == 1u
        && private_lower.first_error.kind == CM_HIR_LOWER_INVALID_ARGUMENT
        && cm_hir_module_map_count(&private_map) == 0u
        && hir.crates.len == saved_crates
        && hir.modules.len == saved_modules
        && hir.items.len == saved_items
        && hir.types.len == saved_types
        && hir.definitions.len == saved_definitions
        && cm_interner_length(&hir.strings) == saved_strings,
        "duplicate dependency extern names were not rejected before mutation");

    cm_hir_module_map_destroy(&private_map);
    cm_hir_module_map_destroy(&unrelated_map);
    cm_hir_module_map_destroy(&collision_map);
    cm_hir_module_map_destroy(&cross_equality_map);
    cm_hir_module_map_destroy(&consumer_map);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&private_graph);
    cm_module_graph_destroy(&unrelated_graph);
    cm_module_graph_destroy(&collision_graph);
    cm_module_graph_destroy(&cross_equality_graph);
    cm_module_graph_destroy(&consumer_graph);
    cm_source_set_destroy(&private_sources);
    cm_source_set_destroy(&unrelated_sources);
    cm_source_set_destroy(&collision_sources);
    cm_source_set_destroy(&cross_equality_sources);
    cm_source_set_destroy(&consumer_sources);
}

static void test_generated_preflight_rejections(void)
{
    static const char *const rejected_sources[] = {
        "macro_rules! make { () => { static GENERATED: u8 = 0; } } "
            "make!();"
    };
    size_t index;

    for (index = 0u; index < sizeof(rejected_sources)
            / sizeof(rejected_sources[0]); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "generated-reject/lib.rs",
            (const unsigned char *)rejected_sources[index],
            strlen(rejected_sources[index]), &root) == CM_SOURCE_OK,
            "could not add generated rejection fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        check(graph_result.error_count == 0u,
            "generated rejection fixture did not build a graph");
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph,
            graph_result.revision, &map, &options);
        check(result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
            && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
            "unsupported generated declaration mutated HIR");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_import_failures_are_transactional(void)
{
    static const char *const rejected_sources[] = {
        "mod defs { #[cfg(off)] pub struct Hidden; } "
            "use crate::defs::Hidden; struct Consumer { value: Hidden }",
        "mod defs { pub struct Clash; } struct Clash; "
            "use crate::defs::Clash; struct Consumer { value: Clash }",
        "use crate::B as A; use crate::A as B; "
            "struct Consumer { value: A }"
    };
    size_t index;

    for (index = 0u; index < CM_ARRAY_LEN(rejected_sources); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmImportResolver imports;
        CmImportResult import_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "import-reject/lib.rs",
            (const unsigned char *)rejected_sources[index],
            strlen(rejected_sources[index]), &root) == CM_SOURCE_OK,
            "could not add rejected import fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_import_resolver_init(&imports);
        import_result = cm_import_resolve(&imports, &graph,
            graph_result.revision);
        check(graph_result.error_count == 0u
            && import_result.error_count != 0u,
            "rejected import fixture did not fail import resolution");

        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        sentinel_name = cm_hir_intern(&hir, "import-sentinel");
        sentinel_span.source = 991u;
        sentinel_span.start = 2u;
        sentinel_span.end = 3u;
        check(cm_hir_create_crate(&hir, sentinel_name, CM_HIR_EDITION_2024,
            sentinel_span, &sentinel_crate, &sentinel_root) == CM_HIR_OK,
            "could not create import failure sentinel");
        cm_hir_lower_options_init(&options);
        result = cm_hir_lower_module_graph(&hir, &graph,
            graph_result.revision, &imports, &map, &options);
        check(result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
            && result.crate_id == CM_HIR_CRATE_NONE
            && result.root_module == CM_HIR_MODULE_NONE
            && result.lowered_item_count == 0u
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.items.len == 0u && hir.types.len == 0u
            && hir.definitions.len == 1u
            && cm_interner_length(&hir.strings) == 1u
            && hir_name_is(&hir, sentinel_name, "import-sentinel")
            && cm_hir_get_crate(&hir, sentinel_crate) != NULL
            && cm_hir_get_module(&hir, sentinel_root) != NULL
            && cm_hir_module_map_count(&map) == 0u,
            "import resolution failure mutated existing HIR state");
        result = cm_hir_lower_module_graph(&hir, &graph,
            graph_result.revision, NULL, &map, &options);
        check(result.error_count == 1u
            && result.first_error.kind == CM_HIR_LOWER_INVALID_ARGUMENT
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.definitions.len == 1u
            && cm_hir_module_map_count(&map) == 0u,
            "null import resolver was not rejected before HIR mutation");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_import_resolver_destroy(&imports);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_import_graph_identity(void)
{
    static const unsigned char source[] = "struct Item;\n";
    CmSourceSet first_sources;
    CmSourceSet second_sources;
    CmSourceId first_root;
    CmSourceId second_root;
    CmModuleGraph first_graph;
    CmModuleGraph second_graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult first;
    CmModuleGraphResult second;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&first_sources);
    cm_source_set_init(&second_sources);
    cm_module_graph_init(&first_graph);
    cm_module_graph_init(&second_graph);
    check(cm_source_add_memory(&first_sources, "identity-a/lib.rs", source,
        sizeof(source) - 1u, &first_root) == CM_SOURCE_OK
        && cm_source_add_memory(&second_sources, "identity-b/lib.rs", source,
            sizeof(source) - 1u, &second_root) == CM_SOURCE_OK,
        "could not add import graph identity fixtures");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    first = cm_module_graph_build(&first_graph, &first_sources, first_root,
        &graph_options);
    second = cm_module_graph_build(&second_graph, &second_sources, second_root,
        &graph_options);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &first_graph, first.revision);
    check(first.error_count == 0u && second.error_count == 0u
        && first.revision == second.revision
        && import_result.error_count == 0u,
        "graph identity fixtures do not share a successful numeric revision");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = cm_hir_lower_module_graph(&hir, &second_graph, second.revision,
        &imports, &map, &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_STALE_GRAPH
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "equal revisions from distinct graphs were treated as one snapshot");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&second_graph);
    cm_module_graph_destroy(&first_graph);
    cm_source_set_destroy(&second_sources);
    cm_source_set_destroy(&first_sources);
}

static void test_generated_failure_is_transactional(void)
{
    static const char *const rejected_sources[] = {
        "#![allow(dead_code)] "
            "macro_rules! make { () => { struct G { x: Missing } } } "
            "make!();",
        "macro_rules! make { () => { enum G { A = 1 } } } make!();",
        "macro_rules! make { () => { struct G<T> where T: Copy { x: T } "
            "} } make!();",
        "mod defs { pub fn value(); } use crate::defs::value as Imported; "
            "struct Consumer { value: Imported }",
        "struct Outer; mod child { macro_rules! make { () => { "
            "struct Inner { value: Outer } } } make!(); }",
        "trait T { type A; } "
            "impl T for u8 { type A = u8; } "
            "impl T for u8 { type A = u16; }",
        "trait T { type A; } impl T for u8 {}",
        "type Cycle = Cycle; trait T { type A; } "
            "impl T for u8 { type A = Cycle; }",
        "type First = u8; type Second = First; trait T { type A; } "
            "impl T for First { type A = u8; } "
            "impl T for Second { type A = u16; }",
        "struct Wrapper<T>; type Alias<T> = Wrapper<T>; "
            "trait Trait { type Assoc; } "
            "impl<T> Trait for Wrapper<T> { type Assoc = T; } "
            "impl<U> Trait for Alias<U> { type Assoc = U; }",
        "trait Direct: Direct {}",
        "trait Base {} trait Duplicate: Base + Base {}",
        "trait First: Second {} trait Second: Third {} "
            "trait Third: First {}",
        "trait Consumer: NotATrait {} struct NotATrait;",
        "trait T {} struct S; impl const T for S {}",
        "trait Good<T> {} fn staged<F>() where "
            "for<'a> F: Good<&'a u8> + Missing<&'a u8> {}"
    };
    static const CmHirLowerErrorKind expected_errors[] = {
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_ALIAS_CYCLE,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_IMPL,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_UNSUPPORTED_ITEM,
        CM_HIR_LOWER_UNRESOLVED_PATH
    };
    size_t index;

    for (index = 0u; index < sizeof(rejected_sources)
            / sizeof(rejected_sources[0]); ++index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;
        CmHirStatus hir_status;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "generated-transaction/lib.rs",
            (const unsigned char *)rejected_sources[index],
            strlen(rejected_sources[index]), &root) == CM_SOURCE_OK,
            "could not add generated transactional rejection fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        check(graph_result.error_count == 0u,
            "transactional rejection fixture did not build a graph");

        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        sentinel_name = cm_hir_intern(&hir, "sentinel");
        sentinel_span.source = 777u;
        sentinel_span.start = 1u;
        sentinel_span.end = 2u;
        hir_status = cm_hir_create_crate(&hir, sentinel_name,
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root);
        check(hir_status == CM_HIR_OK,
            "could not create pre-existing HIR sentinel");
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph,
            graph_result.revision, &map, &options);
        if (result.error_count != 1u
            || result.first_error.kind != expected_errors[index]) {
            fprintf(stderr,
                "transactional rejection %lu: count=%lu kind=%s "
                "message=%s\n", (unsigned long)index,
                (unsigned long)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message);
        }
        check(result.error_count == 1u
            && result.first_error.kind == expected_errors[index]
            && (index != 9u
                || strstr(result.first_error.message,
                    "overlapping ordered generic impl candidates") != NULL)
            && result.crate_id == CM_HIR_CRATE_NONE
            && result.root_module == CM_HIR_MODULE_NONE
            && result.lowered_item_count == 0u
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.items.len == 0u && hir.bodies.len == 0u
            && hir.types.len == 0u && hir.generic_parameters.len == 0u
            && hir.definitions.len == 1u
            && cm_interner_length(&hir.strings) == 1u
            && hir_name_is(&hir, sentinel_name, "sentinel")
            && cm_hir_get_crate(&hir, sentinel_crate) != NULL
            && cm_hir_get_module(&hir, sentinel_root) != NULL
            && cm_hir_module_map_count(&map) == 0u,
            "post-preflight failure did not rewind HIR and module-map state");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_consumed_macro_definition(void)
{
    static const unsigned char source[] =
        "macro_rules! generated { () => {} }\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "empty-macro/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add consumed macro fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph,
        graph_result.revision, &map, &options);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 0u && hir.items.len == 0u
        && hir.crates.len == 1u && hir.modules.len == 1u
        && cm_hir_module_map_count(&map) == 1u,
        "consumed macro definition did not produce a valid empty crate");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_nonempty_module_map_rejection_preserves_state(void)
{
    static const unsigned char source[] = "struct Item;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    CmInternId sentinel_name;
    CmHirCrateId sentinel_crate;
    CmHirModuleId sentinel_root;
    CmHirModuleMapEntry entry;
    CmSpan sentinel_span;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "nonempty-map/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add nonempty module-map fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    check(graph_result.error_count == 0u,
        "nonempty module-map graph did not build");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    sentinel_name = cm_hir_intern(&hir, "nonempty-map-sentinel");
    sentinel_span.source = 994u;
    sentinel_span.start = 9u;
    sentinel_span.end = 10u;
    check(cm_hir_create_crate(&hir, sentinel_name, CM_HIR_EDITION_2024,
        sentinel_span, &sentinel_crate, &sentinel_root) == CM_HIR_OK,
        "could not create nonempty module-map sentinel");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        graph_result.root, &hir, sentinel_root) == CM_HIR_MODULE_MAP_OK,
        "could not pre-bind caller module map");
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_ARGUMENT
        && result.first_error.hir_status == CM_HIR_INVALID_ARGUMENT
        && result.crate_id == CM_HIR_CRATE_NONE
        && result.root_module == CM_HIR_MODULE_NONE
        && result.lowered_item_count == 0u
        && hir.crates.len == 1u && hir.modules.len == 1u
        && hir.items.len == 0u && hir.bodies.len == 0u
        && hir.types.len == 0u && hir.generic_parameters.len == 0u
        && hir.definitions.len == 1u
        && cm_interner_length(&hir.strings) == 1u
        && hir_name_is(&hir, sentinel_name, "nonempty-map-sentinel")
        && cm_hir_get_crate(&hir, sentinel_crate) != NULL
        && cm_hir_get_module(&hir, sentinel_root) != NULL
        && cm_hir_module_map_count(&map) == 1u
        && cm_hir_module_map_get(&map, &graph, graph_result.revision,
            &hir, 0u, &entry) == CM_HIR_MODULE_MAP_OK
        && entry.module == graph_result.root
        && entry.hir_module == sentinel_root,
        "nonempty caller module map rejection changed map or existing HIR");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_stale_graph_rejection(void)
{
    static const unsigned char source[] =
        "macro_rules! make { () => { pub struct Current; } } make!();\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult first;
    CmModuleGraphResult second;
    CmModuleGraphResult failed;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "stale/lib.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add stale-revision fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    first = cm_module_graph_build(&graph, &sources, root, &graph_options);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &graph, first.revision);
    second = cm_module_graph_build(&graph, &sources, root, &graph_options);
    check(first.error_count == 0u && second.error_count == 0u
        && import_result.error_count == 0u
        && import_result.revision == first.revision
        && first.revision != CM_MODULE_GRAPH_REVISION_NONE
        && second.revision != first.revision,
        "successful rebuild did not advance the fixture revision");

    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = cm_hir_lower_module_graph(&hir, &graph, second.revision,
        &imports, &map, &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_STALE_GRAPH
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "stale import resolver mutated HIR or the module map");
    import_result = cm_import_resolve(&imports, &graph, second.revision);
    result = cm_hir_lower_module_graph(&hir, &graph, second.revision,
        &imports, &map, &options);
    check(result.error_count == 0u && result.crate_id != CM_HIR_CRATE_NONE
        && import_result.error_count == 0u
        && cm_hir_module_map_count(&map) == 1u,
        "current graph revision did not lower successfully");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    failed = cm_module_graph_build(&graph, &sources, (CmSourceId)999u,
        &graph_options);
    check(failed.error_count == 1u && failed.revision != second.revision,
        "failed rebuild did not advance the fixture revision");
    result = cm_hir_lower_module_graph(&hir, &graph, failed.revision,
        &imports, &map, &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "current failed graph mutated HIR or the module map");
    result = cm_hir_lower_module_graph(&hir, &graph, second.revision,
        &imports, &map, &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_STALE_GRAPH
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "failed rebuild did not reject the prior revision before mutation");
    result = cm_hir_lower_module_graph(&hir, &graph,
        CM_MODULE_GRAPH_REVISION_NONE, &imports, &map, &options);
    check(result.error_count == 1u
        && result.first_error.kind == CM_HIR_LOWER_INVALID_ARGUMENT
        && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
        "zero graph revision was not rejected before mutation");
    check(strcmp(cm_hir_lower_error_kind_name(CM_HIR_LOWER_STALE_GRAPH),
        "stale graph") == 0, "stale lowering error name differs");
    check(strcmp(cm_hir_lower_error_kind_name(CM_HIR_LOWER_INVALID_IMPL),
        "invalid impl") == 0, "invalid impl lowering error name differs");

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_prelude_supertrait_resolution(void)
{
    static const unsigned char success_source[] =
        "#[prelude_import] use crate::prelude::*;\n"
        "mod consumer { pub trait FullOps: Sized {} }\n"
        "mod prelude { pub use crate::marker::Sized; }\n"
        "mod marker { pub trait Sized {} }\n";
    static const unsigned char shadow_source[] =
        "#[prelude_import] use crate::prelude::*;\n"
        "mod consumer { struct Sized; pub trait FullOps: Sized {} }\n"
        "mod prelude { pub use crate::marker::Sized; }\n"
        "mod marker { pub trait Sized {} }\n";
    static const unsigned char ambiguous_source[] =
        "#[prelude_import] use crate::prelude::*;\n"
        "mod consumer { pub trait FullOps: Clash {} }\n"
        "mod prelude { pub use crate::left::*; pub use crate::right::*; }\n"
        "mod left { pub trait Clash {} }\n"
        "mod right { pub trait Clash {} }\n";
    const unsigned char *fixtures[3];
    size_t fixture_lengths[3];
    size_t fixture_index;

    fixtures[0] = success_source;
    fixtures[1] = shadow_source;
    fixtures[2] = ambiguous_source;
    fixture_lengths[0] = sizeof(success_source) - 1u;
    fixture_lengths[1] = sizeof(shadow_source) - 1u;
    fixture_lengths[2] = sizeof(ambiguous_source) - 1u;
    for (fixture_index = 0u; fixture_index < 3u; ++fixture_index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmModuleGraphResult graph_result;
        CmCfgSet cfg;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources, "prelude-supertrait/lib.rs",
            fixtures[fixture_index], fixture_lengths[fixture_index], &root)
            == CM_SOURCE_OK, "could not add prelude-supertrait fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        check(graph_result.error_count == 0u,
            "prelude-supertrait graph did not build");
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        if (fixture_index == 0u) {
            const CmHirItem *full_ops;
            const CmHirItem *sized;

            full_ops = find_hir_item_anywhere(&hir, "FullOps");
            sized = find_hir_item_anywhere(&hir, "Sized");
            check(result.error_count == 0u && full_ops != NULL
                && sized != NULL && full_ops->kind == CM_HIR_ITEM_TRAIT
                && sized->kind == CM_HIR_ITEM_TRAIT
                && full_ops->data.trait_item.supertrait_count == 1u
                && cm_hir_def_id_equal(
                    full_ops->data.trait_item.supertraits[0]
                        .trait_type.definition,
                    sized->definition),
                "forward prelude supertrait lost exact trait identity");
        } else if (fixture_index == 1u) {
            check(result.error_count == 1u
                && result.first_error.kind == CM_HIR_LOWER_WRONG_NAMESPACE
                && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
                "local wrong-namespace shadow escaped or mutated HIR");
        } else {
            check(result.error_count == 1u
                && result.first_error.kind == CM_HIR_LOWER_RESOLVER_FAILURE
                && hir_is_empty(&hir) && cm_hir_module_map_count(&map) == 0u,
                "ambiguous prelude escaped or mutated HIR");
        }
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_supertrait_associated_equalities(void)
{
    static const unsigned char forward_source[] =
        "pub trait FullOps<T>: ops::Shr<u32, Output = Self, Item = T> {}\n"
        "mod ops { pub trait Shr<Rhs> { type Item; type Output; } }\n";
    static const unsigned char reverse_source[] =
        "mod ops { pub trait Shr<Rhs> { type Item; type Output; } }\n"
        "pub trait FullOps<T>: ops::Shr<u32, Output = Self, Item = T> {}\n";
    static const unsigned char *const accepted[] = {
        forward_source, reverse_source
    };
    static const size_t accepted_lengths[] = {
        sizeof(forward_source) - 1u, sizeof(reverse_source) - 1u
    };
    static const char *const rejected[] = {
        "trait Shr<Rhs> { type Output; } "
            "trait Bad: Shr<u8, u16, Output = Self> {}",
        "trait Shr<Rhs> { type Output; } "
            "trait Bad: Shr<Output = Self> {}",
        "trait Shr<Rhs> { fn Output(); } "
            "trait Bad: Shr<u8, Output = Self> {}",
        "trait Shr<Rhs> { type Output; } "
            "trait Bad: Shr<u8, Missing = Self> {}",
        "trait Shr<Rhs> { type Output; } "
            "trait Bad: Shr<u8, Output = Self, Output = u8> {}",
        "trait Shr<Rhs> { type Output<T>; } "
            "trait Bad: Shr<u8, Output = Self> {}",
        "trait Left { type Output; } trait Right { type Output; } "
            "trait Both: Left + Right {} "
            "trait Bad: Both<Output = Self> {}",
        "trait Marker {} trait Shr<Rhs> { type Output; } "
            "trait Bad: Shr<u8, Output: Marker> {}"
    };
    static const CmHirLowerErrorKind rejected_kinds[] = {
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_WRONG_NAMESPACE,
        CM_HIR_LOWER_UNRESOLVED_PATH,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC,
        CM_HIR_LOWER_INVALID_TRAIT,
        CM_HIR_LOWER_UNSUPPORTED_GENERIC
    };
    size_t fixture_index;

    for (fixture_index = 0u;
         fixture_index < sizeof(accepted) / sizeof(accepted[0]);
         ++fixture_index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;
        const CmHirItem *full_ops;
        const CmHirItem *shr;
        const CmHirItem *output;
        const CmHirItem *item;
        const CmHirSupertrait *supertrait;
        const CmHirGenericParam *parameter;
        const CmHirType *rhs;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "supertrait-equality/lib.rs", accepted[fixture_index],
            accepted_lengths[fixture_index], &root) == CM_SOURCE_OK,
            "could not add supertrait equality fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        full_ops = find_hir_item_anywhere(&hir, "FullOps");
        shr = find_hir_item_anywhere(&hir, "Shr");
        output = shr == NULL ? NULL : find_hir_associated_item(&hir,
            shr->definition, CM_HIR_ITEM_TYPE_ALIAS, "Output");
        item = shr == NULL ? NULL : find_hir_associated_item(&hir,
            shr->definition, CM_HIR_ITEM_TYPE_ALIAS, "Item");
        supertrait = full_ops == NULL
                || full_ops->kind != CM_HIR_ITEM_TRAIT
                || full_ops->data.trait_item.supertrait_count != 1u
            ? NULL : &full_ops->data.trait_item.supertraits[0];
        parameter = full_ops == NULL
                || full_ops->generic_parameter_count != 1u
            ? NULL : cm_hir_get_generic_param(&hir,
                full_ops->generic_parameter_start);
        rhs = supertrait == NULL
                || supertrait->trait_type.argument_count != 1u
                || supertrait->trait_type.arguments[0].kind
                    != CM_HIR_GENERIC_ARG_TYPE
            ? NULL : cm_hir_get_type(&hir,
                supertrait->trait_type.arguments[0].data.type);
        check(graph_result.error_count == 0u && result.error_count == 0u
            && full_ops != NULL && shr != NULL && output != NULL
            && item != NULL && supertrait != NULL && parameter != NULL
            && cm_hir_def_id_equal(supertrait->trait_type.definition,
                shr->definition)
            && rhs != NULL && rhs->kind == CM_HIR_TYPE_INTEGER_KIND
            && rhs->data.integer_type.kind == CM_HIR_INT_U32
            && supertrait->equality_count == 2u
            && supertrait->equalities != NULL
            && cm_hir_def_id_equal(
                supertrait->equalities[0].associated_type,
                output->definition)
            && hir_type_is_self(&hir, supertrait->equalities[0].value,
                full_ops->definition)
            && cm_hir_def_id_equal(
                supertrait->equalities[1].associated_type,
                item->definition)
            && hir_type_is_parameter(&hir,
                supertrait->equalities[1].value,
                full_ops->generic_parameter_start)
            && source_span_is(&sources, supertrait->span,
                "ops::Shr<u32, Output = Self, Item = T>")
            && source_span_is(&sources, supertrait->equalities[0].span,
                "Output = Self")
            && source_span_is(&sources, supertrait->equalities[1].span,
                "Item = T"),
            "supertrait equality lost ordered arguments, DefIds, or spans");
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }

    for (fixture_index = 0u;
         fixture_index < sizeof(rejected) / sizeof(rejected[0]);
         ++fixture_index) {
        CmSourceSet sources;
        CmSourceId root;
        CmModuleGraph graph;
        CmModuleGraphOptions graph_options;
        CmCfgSet cfg;
        CmModuleGraphResult graph_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerOptions options;
        CmHirLowerResult result;
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;
        size_t saved_definitions;
        size_t saved_items;
        size_t saved_strings;
        size_t saved_types;

        cm_source_set_init(&sources);
        cm_module_graph_init(&graph);
        check(cm_source_add_memory(&sources,
            "supertrait-equality-reject/lib.rs",
            (const unsigned char *)rejected[fixture_index],
            strlen(rejected[fixture_index]), &root) == CM_SOURCE_OK,
            "could not add rejected supertrait equality fixture");
        cm_cfg_set_init(&cfg);
        cm_module_graph_options_init(&graph_options);
        graph_options.cfg = &cfg;
        graph_result = cm_module_graph_build(&graph, &sources, root,
            &graph_options);
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        cm_hir_lower_options_init(&options);
        sentinel_span.source = 91u;
        sentinel_span.start = 2u;
        sentinel_span.end = 7u;
        sentinel_name = cm_interner_intern(&hir.strings,
            (const unsigned char *)"sentinel", 8u);
        check(cm_hir_create_crate(&hir, sentinel_name,
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root) == CM_HIR_OK,
            "could not seed supertrait rollback fixture");
        saved_definitions = hir.definitions.len;
        saved_items = hir.items.len;
        saved_strings = cm_interner_length(&hir.strings);
        saved_types = hir.types.len;
        result = lower_module_graph(&hir, &graph, graph_result.revision,
            &map, &options);
        check(graph_result.error_count == 0u && result.error_count == 1u
            && result.first_error.kind == rejected_kinds[fixture_index]
            && cm_hir_module_map_count(&map) == 0u
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.definitions.len == saved_definitions
            && hir.items.len == saved_items
            && hir.types.len == saved_types
            && cm_interner_length(&hir.strings) == saved_strings,
            "unsupported supertrait equality escaped or mutated HIR");
        (void)sentinel_crate;
        (void)sentinel_root;
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
    }
}

static void test_core_trait_alias_declaration(void)
{
    static const unsigned char source[] =
        "pub trait Pointee { type Metadata; }\n"
        "pub trait PointeeSized {}\n"
        "pub trait Thin = Pointee<Metadata = ()> + PointeeSized;\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *pointee;
    const CmHirItem *pointee_sized;
    const CmHirItem *metadata;
    const CmHirItem *thin;
    const CmHirTraitAliasBound *pointee_bound;
    const CmHirTraitAliasBound *sized_bound;
    const CmHirType *metadata_value;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "ptr/metadata.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add the core trait-alias fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    pointee = find_hir_item_anywhere(&hir, "Pointee");
    pointee_sized = find_hir_item_anywhere(&hir, "PointeeSized");
    thin = find_hir_item_anywhere(&hir, "Thin");
    metadata = pointee == NULL ? NULL : find_hir_associated_item(&hir,
        pointee->definition, CM_HIR_ITEM_TYPE_ALIAS, "Metadata");
    pointee_bound = thin == NULL
            || thin->kind != CM_HIR_ITEM_TRAIT_ALIAS
            || thin->data.trait_alias_item.bound_count != 2u
        ? NULL : &thin->data.trait_alias_item.bounds[0];
    sized_bound = pointee_bound == NULL
        ? NULL : &thin->data.trait_alias_item.bounds[1];
    metadata_value = pointee_bound == NULL
            || pointee_bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
            || pointee_bound->data.trait_bound.equality_count != 1u
        ? NULL : cm_hir_get_type(&hir,
            pointee_bound->data.trait_bound.equalities[0].value);
    check(graph_result.error_count == 0u && result.error_count == 0u
        && pointee != NULL && pointee->kind == CM_HIR_ITEM_TRAIT
        && pointee_sized != NULL
        && pointee_sized->kind == CM_HIR_ITEM_TRAIT
        && metadata != NULL && thin != NULL && pointee_bound != NULL
        && sized_bound != NULL
        && pointee_bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
        && cm_hir_def_id_equal(pointee_bound->data.trait_bound
                .trait_type.definition,
            pointee->definition)
        && pointee_bound->data.trait_bound.equality_count == 1u
        && cm_hir_def_id_equal(pointee_bound->data.trait_bound
                .equalities[0].associated_type,
            metadata->definition)
        && metadata_value != NULL
        && metadata_value->kind == CM_HIR_TYPE_UNIT_KIND
        && sized_bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT
        && cm_hir_def_id_equal(sized_bound->data.trait_bound
                .trait_type.definition,
            pointee_sized->definition)
        && source_span_is(&sources, thin->span,
            "pub trait Thin = Pointee<Metadata = ()> + PointeeSized;")
        && source_span_is(&sources, pointee_bound->span,
            "Pointee<Metadata = ()>")
        && source_span_is(&sources,
            pointee_bound->data.trait_bound.equalities[0].span,
            "Metadata = ()")
        && source_span_is(&sources, sized_bound->span, "PointeeSized"),
        "core Thin alias lost its ordered RHS identities or spans");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_core_auto_trait_negative_impl_cluster(void)
{
    static const unsigned char source[] =
        "pub trait PointeeSized {}\n"
        "pub unsafe auto trait Send {}\n"
        "impl<T: PointeeSized> !Send for *const T {}\n"
        "impl<T: PointeeSized> !Send for *mut T {}\n";
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions options;
    CmHirLowerResult result;
    const CmHirItem *pointee_sized;
    const CmHirItem *send;
    const CmHirItem *const_impl;
    const CmHirItem *mut_impl;
    size_t index;
    char *dump;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    check(cm_source_add_memory(&sources, "marker.rs", source,
        sizeof(source) - 1u, &root) == CM_SOURCE_OK,
        "could not add the core auto-trait fixture");
    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&options);
    result = lower_module_graph(&hir, &graph, graph_result.revision, &map,
        &options);
    pointee_sized = find_hir_item_anywhere(&hir, "PointeeSized");
    send = find_hir_item_anywhere(&hir, "Send");
    const_impl = NULL;
    mut_impl = NULL;
    for (index = 0u; index < hir.items.len; ++index) {
        const CmHirItem *candidate;
        const CmHirType *self_type;

        candidate = (const CmHirItem *)cm_vec_at_const(&hir.items, index);
        if (candidate == NULL || candidate->kind != CM_HIR_ITEM_IMPL) {
            continue;
        }
        self_type = cm_hir_get_type(&hir,
            candidate->data.impl_item.self_type);
        if (self_type == NULL
            || self_type->kind != CM_HIR_TYPE_RAW_POINTER_KIND) {
            continue;
        }
        if (self_type->data.raw_pointer_type.mutability
                == CM_HIR_MUTABLE) {
            mut_impl = candidate;
        } else {
            const_impl = candidate;
        }
    }
    check(graph_result.error_count == 0u && result.error_count == 0u
        && result.lowered_item_count == 4u
        && pointee_sized != NULL
        && pointee_sized->kind == CM_HIR_ITEM_TRAIT
        && send != NULL && send->kind == CM_HIR_ITEM_TRAIT
        && send->data.trait_item.is_auto
        && send->data.trait_item.safety == CM_HIR_UNSAFE
        && const_impl != NULL && mut_impl != NULL,
        "core Send/negative-pointer cluster did not lower exactly");
    if (send != NULL && pointee_sized != NULL) {
        const CmHirItem *impls[2];

        impls[0] = const_impl;
        impls[1] = mut_impl;
        for (index = 0u; index < 2u; ++index) {
            const CmHirItem *impl_item;
            const CmHirGenericParam *parameter;
            const CmHirTraitPredicate *predicate;
            const CmHirType *subject;

            impl_item = impls[index];
            parameter = impl_item == NULL
                    || impl_item->generic_parameter_count != 1u
                ? NULL : cm_hir_get_generic_param(&hir,
                    impl_item->generic_parameter_start);
            predicate = impl_item == NULL
                    || impl_item->predicate_count != 1u
                ? NULL : &impl_item->predicates[0];
            subject = predicate == NULL ? NULL
                : cm_hir_get_type(&hir, predicate->subject);
            check(impl_item != NULL
                && impl_item->data.impl_item.is_negative
                && impl_item->data.impl_item.has_trait
                && impl_item->data.impl_item.safety == CM_HIR_SAFE
                && cm_hir_def_id_equal(
                    impl_item->data.impl_item.trait_type.definition,
                    send->definition)
                && impl_item->data.impl_item.trait_type.argument_count == 0u
                && impl_item->data.impl_item.trait_type.arguments == NULL
                && parameter != NULL
                && parameter->kind == CM_HIR_GENERIC_TYPE
                && parameter->index == 0u
                && cm_hir_def_id_equal(parameter->owner,
                    impl_item->definition)
                && predicate != NULL
                && cm_hir_def_id_equal(predicate->trait_type.definition,
                    pointee_sized->definition)
                && subject != NULL
                && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
                && subject->data.parameter_type.parameter
                    == impl_item->generic_parameter_start
                && source_span_is(&sources, predicate->span,
                    "PointeeSized")
                && source_span_is(&sources,
                    cm_hir_get_type(&hir,
                        impl_item->data.impl_item.self_type)->span,
                    index == 0u ? "*const T" : "*mut T")
                && source_span_is(&sources, impl_item->span,
                    index == 0u
                        ? "impl<T: PointeeSized> !Send for *const T {}"
                        : "impl<T: PointeeSized> !Send for *mut T {}"),
                "negative auto-trait impl lost polarity, generic, "
                "predicate, type, target, or span identity");
        }
    }
    dump = dump_hir(&hir);
    check(dump != NULL
        && strncmp(dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(dump, "safety=unsafe auto=1") != NULL
        && text_count_between(dump, dump + strlen(dump),
            "safety=safe negative=1") == 2u,
        "hir-v27 dump erased auto-trait or negative-impl headers");
    free(dump);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

int main(void)
{
    test_imported_graph_paths();
    test_primitive_import_paths();
    test_cross_file_graph();
    test_include_spliced_item_provenance();
    test_include_spliced_module_provenance();
    test_cross_source_method_body_provenance();
    test_nested_method_effective_children();
    test_generated_method_effective_children();
    test_rustc_as_vec_into_iter_fixture();
    test_rustc_future_ready_pending_fixture();
    test_rustc_try_family_fixture();
    test_rustc_into_iterator_deref_fixture();
    test_rustc_iterator_methods_fixture();
    test_rustc_iterator_generic_methods_fixture();
    test_method_completeness_is_transactional();
    test_preflight_rejections();
    test_safe_extern_c_declarations();
    test_unadjusted_extern_declaration();
    test_foreign_type_declaration();
    test_foreign_type_generic_use_rejection();
    test_source_const();
    test_trait_associated_const_declarations();
    test_generated_trait_impl_consts();
    test_source_static_with_named_array_length();
    test_enum_variant_glob_import();
    test_macro_import_identity();
    test_typed_const_generic_declarations();
    test_const_generic_trait_method_declaration();
    test_attributed_import_is_structural();
    test_separate_namespaces();
    test_imported_trait_projection();
    test_cross_trait_projection_default_prebinding();
    test_imported_trait_impl_selection();
    test_ordered_generic_impl_selection();
    test_inherent_method_bound_lifetime_binder();
    test_generic_trait_impl_method();
    test_lifetime_generic_default_trait_method();
    test_adt_generic_type_defaults();
    test_lifetime_generic_trait_outlives();
    test_associated_type_lifetime_bounds_graph();
    test_adt_generic_type_defaults_fail_closed();
    test_generic_associated_type_declaration();
    test_transitive_generic_self_projection_graph_rollback();
    test_attributed_async_trait_method();
    test_predicate_prefix_lifetime_binder();
    test_generated_declarations();
    test_generated_declarative_macro_item();
    test_generated_inline_module();
    test_generated_anonymous_consts();
    test_inherent_associated_const();
    test_dependency_generated_declarations();
    test_hir_library_artifact_types();
    test_generated_preflight_rejections();
    test_import_failures_are_transactional();
    test_import_graph_identity();
    test_module_map_mutation_is_transactional();
    test_generated_failure_is_transactional();
    test_consumed_macro_definition();
    test_nonempty_module_map_rejection_preserves_state();
    test_stale_graph_rejection();
    test_prelude_supertrait_resolution();
    test_supertrait_associated_equalities();
    test_core_trait_alias_declaration();
    test_core_auto_trait_negative_impl_cluster();
    if (failures == 0) puts("HIR graph lowering tests: ok");
    return failures == 0 ? 0 : 1;
}
