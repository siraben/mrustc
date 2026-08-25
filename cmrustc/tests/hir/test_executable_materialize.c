#include "cm/hir/executable_materialize.h"
#include "cm/hir/trait_solver.h"
#include "cm/hir/typeck.h"
#include "cm/sha256.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestFixture {
    CmHirExecutableMetadata metadata;
    CmHirArtifactSourceEntry sources[2];
    CmHirExecutableModule modules[1];
    CmHirExecutableTrait traits[1];
    CmHirExecutableType types[2];
    CmHirExecutableImpl impls[1];
    CmHirExecutableValue values[2];
    uint32_t recipe_parameters[2];
    uint32_t native_parameters[1];
    CmHirExecutablePredicate predicates[1];
    CmHirExecutableNamespaceEntry entries[3];
    CmHirExecutableBody bodies[1];
    CmHirExecutableLinkObject objects[1];
    CmHirExecutableLinkSymbol symbols[1];
} TestFixture;

static void test_hash(const void *bytes, size_t length,
    CmHirArtifactDigest *digest)
{
    CmSha256 sha;
    cm_sha256_init(&sha);
    cm_sha256_update(&sha, bytes, length);
    cm_sha256_final(&sha, digest->bytes);
}

static void test_fixture_init(TestFixture *fixture)
{
    static const unsigned char source[] =
        "pub trait Present {} impl Present for u32 {}\n"
        "pub fn bounded<T: Present>(prefix:u32,x:T)->T{x}\n";
    static const unsigned char native[] =
        "unsigned bump(unsigned value){return value+1;}\n";
    CmHirExecutableMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirExecutableString)S("g3_dep");
    metadata->crate_disambiguator = (CmHirExecutableString)S("materialize-v1");
    metadata->edition = UINT32_C(2021);
    metadata->target_descriptor = (CmHirExecutableString)S(
        "x86_64-unknown-linux-gnu;e-m:e-p:64:64");
    metadata->panic_strategy = (CmHirExecutableString)S("abort");
    fixture->sources[0].logical_path.data = "native/bump.c";
    fixture->sources[0].logical_path.length = sizeof("native/bump.c") - 1u;
    fixture->sources[0].contents.data = native;
    fixture->sources[0].contents.length = sizeof(native) - 1u;
    fixture->sources[1].logical_path.data = "src/lib.rs";
    fixture->sources[1].logical_path.length = sizeof("src/lib.rs") - 1u;
    fixture->sources[1].contents.data = source;
    fixture->sources[1].contents.length = sizeof(source) - 1u;
    metadata->source_entries = fixture->sources;
    metadata->source_entry_count = 2u;
    assert(cm_hir_artifact_source_closure_digest(fixture->sources, 2u,
        &metadata->source_digest) == CM_HIR_ARTIFACT_IDENTITY_OK);

    fixture->modules[0].name = metadata->crate_name;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;
    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirExecutableString)S("Present");
    fixture->traits[0].source_ordinal = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;
    fixture->types[0].kind = CM_HIR_EXEC_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_EXEC_PRIMITIVE_U32;
    fixture->types[1].kind = CM_HIR_EXEC_TYPE_VALUE_GENERIC;
    fixture->types[1].owner_value = 1u;
    metadata->types = fixture->types;
    metadata->type_count = 2u;
    fixture->impls[0].owner_module = 1u;
    fixture->impls[0].source_ordinal = 2u;
    fixture->impls[0].trait_local = 1u;
    fixture->impls[0].self_type = 1u;
    metadata->impls = fixture->impls;
    metadata->impl_count = 1u;

    fixture->recipe_parameters[0] = 1u;
    fixture->recipe_parameters[1] = 2u;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirExecutableString)S("bounded");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].kind = CM_HIR_EXEC_VALUE_RECIPE;
    fixture->values[0].generic_name = (CmHirExecutableString)S("T");
    fixture->values[0].parameter_count = 2u;
    fixture->values[0].parameter_types = fixture->recipe_parameters;
    fixture->values[0].return_type = 2u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].execution_local = 1u;
    fixture->native_parameters[0] = 1u;
    fixture->values[1].owner_module = 1u;
    fixture->values[1].name = (CmHirExecutableString)S("bump");
    fixture->values[1].source_ordinal = 4u;
    fixture->values[1].kind = CM_HIR_EXEC_VALUE_NATIVE_OBJECT;
    fixture->values[1].parameter_count = 1u;
    fixture->values[1].parameter_types = fixture->native_parameters;
    fixture->values[1].return_type = 1u;
    fixture->values[1].execution_local = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 2u;
    fixture->predicates[0].trait_local = 1u;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->entries[0].owner_module = 1u;
    fixture->entries[0].namespace_kind = CM_HIR_EXEC_NAMESPACE_TYPE;
    fixture->entries[0].name = fixture->traits[0].name;
    fixture->entries[0].target_kind = CM_HIR_EXEC_NAMESPACE_TRAIT;
    fixture->entries[0].target_local = 1u;
    fixture->entries[0].export_ordinal = 1u;
    fixture->entries[1].owner_module = 1u;
    fixture->entries[1].namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->entries[1].name = fixture->values[0].name;
    fixture->entries[1].target_kind = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->entries[1].target_local = 1u;
    fixture->entries[1].export_ordinal = 3u;
    fixture->entries[2].owner_module = 1u;
    fixture->entries[2].namespace_kind = CM_HIR_EXEC_NAMESPACE_VALUE;
    fixture->entries[2].name = fixture->values[1].name;
    fixture->entries[2].target_kind = CM_HIR_EXEC_NAMESPACE_VALUE_TARGET;
    fixture->entries[2].target_local = 2u;
    fixture->entries[2].export_ordinal = 4u;
    metadata->namespace_entries = fixture->entries;
    metadata->namespace_count = 3u;
    fixture->bodies[0].owner_value = 1u;
    fixture->bodies[0].parameter_index = 1u;
    fixture->bodies[0].parameter_type = 2u;
    fixture->bodies[0].return_type = 2u;
    metadata->bodies = fixture->bodies;
    metadata->body_count = 1u;
    fixture->objects[0].archive_member_name =
        (CmHirExecutableString)S("g3_dep.o");
    fixture->objects[0].byte_length = 4u;
    fixture->objects[0].object_bytes = "OBJ!";
    fixture->objects[0].object_bytes_length = 4u;
    test_hash("OBJ!", 4u, &fixture->objects[0].object_digest);
    fixture->objects[0].symbol_start = 1u;
    fixture->objects[0].symbol_count = 1u;
    metadata->objects = fixture->objects;
    metadata->object_count = 1u;
    fixture->symbols[0].owner_value = 2u;
    fixture->symbols[0].object_local = 1u;
    fixture->symbols[0].external_symbol =
        (CmHirExecutableString)S("cmrustc_g3_bump");
    metadata->symbols = fixture->symbols;
    metadata->symbol_count = 1u;
    assert(cm_hir_executable_metadata_compute_identity(metadata,
        &metadata->link_manifest_digest, &metadata->artifact_identity)
        == CM_HIR_EXEC_METADATA_OK);
    assert(cm_hir_executable_metadata_validate(metadata)
        == CM_HIR_EXEC_METADATA_OK);
}

static CmHirLibraryValue test_lookup_value(
    const CmHirLibraryArtifact *artifact, const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryValue value;
    path[0].bytes = (const unsigned char *)"provider";
    path[0].length = sizeof("provider") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_OK);
    return value;
}

static CmHirLibraryBinding test_lookup_trait(
    const CmHirLibraryArtifact *artifact)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"provider";
    path[0].length = sizeof("provider") - 1u;
    path[1].bytes = (const unsigned char *)"Present";
    path[1].length = sizeof("Present") - 1u;
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_TRAIT);
    return binding;
}

static void test_materialize_recipe_and_impl(void)
{
    TestFixture fixture;
    CmHirContext hir;
    CmHirLibraryArtifact artifact;
    CmHirExecutableMaterializeResult result;
    CmHirLibraryValue recipe;
    CmHirLibraryValue native;
    CmHirLibraryBinding trait;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirBody *body;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckTypeId self_type;
    CmTypeckNamedType query;
    CmTraitSelectionResult selected;
    static const unsigned char consumer_source[] =
        "use provider::bounded;\n";
    CmSourceSet consumer_sources;
    CmSourceId consumer_root_source;
    CmModuleGraph consumer_graph;
    CmModuleGraphOptions graph_options;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirLibraryPathSegment local_name;
    CmHirLibraryImport imported;

    test_fixture_init(&fixture);
    cm_hir_context_init(&hir);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_executable_metadata_materialize(&hir, &artifact,
        &fixture.metadata, "provider", 77u);
    assert(result.status == CM_HIR_EXEC_MATERIALIZE_OK
        && result.module_count == 1u
        && result.public_type_entry_count == 1u
        && result.public_value_entry_count == 2u);
    recipe = test_lookup_value(&artifact, "bounded");
    native = test_lookup_value(&artifact, "bump");
    trait = test_lookup_trait(&artifact);
    assert(recipe.data.function.generic_parameter_count == 1u
        && recipe.data.function.predicate_count == 1u
        && recipe.data.function.nominal_reference_count == 1u
        && native.data.function.generic_parameter_count == 0u);
    definition = cm_hir_lookup_definition(&hir, recipe.definition);
    assert(definition != NULL && definition->state == CM_HIR_DEFINITION_BOUND);
    item = cm_hir_get_item(&hir, definition->entity.item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->data.function_item.body != CM_HIR_BODY_NONE);
    body = cm_hir_get_body(&hir, item->data.function_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->source == 77u && body->source_expression_id == 0u
        && body->origin.kind == CM_HIR_BODY_ORIGIN_METADATA_RECIPE
        && body->origin.data.metadata_recipe.recipe_index == 1u
        && body->origin.data.metadata_recipe.argument_index == 1u
        && memcmp(body->origin.data.metadata_recipe.artifact_identity,
            fixture.metadata.artifact_identity.bytes,
            CM_HIR_ARTIFACT_IDENTITY_SIZE) == 0);

    cm_source_set_init(&consumer_sources);
    cm_module_graph_init(&consumer_graph);
    cm_cfg_set_init(&cfg);
    assert(cm_source_add_memory(&consumer_sources, "consumer.rs",
        consumer_source, sizeof(consumer_source) - 1u,
        &consumer_root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&consumer_graph, &consumer_sources,
        consumer_root_source, &graph_options);
    assert(graph_result.error_count == 0u
        && graph_result.root != CM_MODULE_NONE);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &consumer_graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    local_name.bytes = (const unsigned char *)"bounded";
    local_name.length = sizeof("bounded") - 1u;
    memset(&imported, 0, sizeof(imported));
    assert(cm_hir_library_artifact_resolve_value_import(&artifact, &imports,
        &consumer_graph, graph_result.revision, graph_result.root,
        &local_name, &imported) == CM_HIR_LIBRARY_OK
        && imported.binding.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && cm_hir_def_id_equal(imported.binding.definition,
            recipe.definition));
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&consumer_graph);
    cm_source_set_destroy(&consumer_sources);

    memset(&index, 0, sizeof(index));
    assert(cm_trait_impl_index_init(&index, &hir, result.crate_id,
        CM_TRAIT_IMPL_UNIVERSE_OPEN) == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&typeck, &hir);
    assert(cm_typeck_import_hir_type(&typeck,
        recipe.data.function.parameter_types[0], &self_type) == CM_TYPECK_OK);
    memset(&query, 0, sizeof(query));
    query.definition = trait.definition;
    selected = cm_trait_solver_select(&index, &typeck, self_type, &query);
    assert(selected.kind == CM_TRAIT_SOLVER_PROVEN
        && selected.proof_origin == CM_TRAIT_PROOF_IMPL);
    cm_typeck_context_destroy(&typeck);
    cm_trait_impl_index_destroy(&index);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&hir);
}

static void test_mutation_rejects_transactionally(void)
{
    TestFixture fixture;
    CmHirContext hir;
    CmHirLibraryArtifact artifact;
    CmHirExecutableMaterializeResult result;
    CmHirLibraryArtifactIdentity before;
    CmHirLibraryArtifactIdentity after;
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t expressions;

    test_fixture_init(&fixture);
    cm_hir_context_init(&hir);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_executable_metadata_materialize(&hir, &artifact,
        &fixture.metadata, "provider", 81u);
    assert(result.status == CM_HIR_EXEC_MATERIALIZE_OK);
    assert(cm_hir_library_artifact_identity(&artifact, &before));
    crates = hir.crates.len;
    modules = hir.modules.len;
    items = hir.items.len;
    bodies = hir.bodies.len;
    expressions = hir.expressions.len;
    fixture.bodies[0].return_type = 1u;
    result = cm_hir_executable_metadata_materialize(&hir, &artifact,
        &fixture.metadata, "replacement", 82u);
    assert(result.status == CM_HIR_EXEC_MATERIALIZE_INVALID_METADATA
        && result.metadata_status != CM_HIR_EXEC_METADATA_OK
        && hir.crates.len == crates && hir.modules.len == modules
        && hir.items.len == items && hir.bodies.len == bodies
        && hir.expressions.len == expressions);
    assert(cm_hir_library_artifact_identity(&artifact, &after)
        && before.context == after.context
        && before.crate_id == after.crate_id
        && cm_hir_def_id_equal(before.root_definition,
            after.root_definition)
        && strcmp(after.extern_name, "provider") == 0);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&hir);
}

int main(void)
{
    test_materialize_recipe_and_impl();
    test_mutation_rejects_transactionally();
    return 0;
}
