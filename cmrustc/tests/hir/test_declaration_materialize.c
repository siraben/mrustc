#include "cm/hir/declaration_materialize.h"
#include "cm/hir/lower.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationString cfgs[2];
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[3];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[3];
} TestFixture;

typedef struct ContextLengths {
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t expressions;
    size_t types;
    size_t generics;
    size_t definitions;
    size_t strings;
} ContextLengths;

static void fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator = (CmHirDeclarationString)S("gate-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->cfgs[0] = (CmHirDeclarationString)S("target_arch=x86_64");
    fixture->cfgs[1] =
        (CmHirDeclarationString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 2u;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].index = 0u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("X");
    metadata->generics = fixture->generics;
    metadata->generic_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->parameters[0] = 3u;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("needs");
    fixture->values[0].source_ordinal = 2u;
    fixture->values[0].generic_start = 2u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 1u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->predicate_arguments[0] = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("GateReexport");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[2].name = fixture->values[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static CmHirDeclarationMaterializeExpectation expectation_for(
    const CmHirDeclarationMetadata *metadata)
{
    CmHirDeclarationMaterializeExpectation expectation;
    memset(&expectation, 0, sizeof(expectation));
    expectation.crate_name = metadata->crate_name;
    expectation.crate_disambiguator = metadata->crate_disambiguator;
    expectation.edition = metadata->edition;
    expectation.target_triple = metadata->target_triple;
    expectation.data_layout = metadata->data_layout;
    expectation.panic_strategy = metadata->panic_strategy;
    expectation.cfgs = metadata->cfgs;
    expectation.cfg_count = metadata->cfg_count;
    return expectation;
}

static ContextLengths context_lengths(const CmHirContext *context)
{
    ContextLengths lengths;
    lengths.crates = context->crates.len;
    lengths.modules = context->modules.len;
    lengths.items = context->items.len;
    lengths.bodies = context->bodies.len;
    lengths.expressions = context->expressions.len;
    lengths.types = context->types.len;
    lengths.generics = context->generic_parameters.len;
    lengths.definitions = context->definitions.len;
    lengths.strings = cm_interner_length(&context->strings);
    return lengths;
}

static void assert_context_lengths(const CmHirContext *context,
    ContextLengths expected)
{
    ContextLengths actual = context_lengths(context);
    assert(actual.crates == expected.crates);
    assert(actual.modules == expected.modules);
    assert(actual.items == expected.items);
    assert(actual.bodies == expected.bodies);
    assert(actual.expressions == expected.expressions);
    assert(actual.types == expected.types);
    assert(actual.generics == expected.generics);
    assert(actual.definitions == expected.definitions);
    assert(actual.strings == expected.strings);
}

static CmHirLibraryBinding lookup_binding(const CmHirLibraryArtifact *artifact,
    const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static const CmHirItem *find_item(const CmHirContext *context,
    CmHirItemKind kind, const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &context->items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&context->strings, item->name);
        if (item != NULL && item->kind == kind && item_name != NULL
            && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) return item;
    }
    return NULL;
}

static void assert_gate_predicate(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId gate)
{
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmHirType *argument;
    const CmHirGenericParam *parameter;
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->generic_parameter_count == 1u
        && item->predicate_count == 1u);
    predicate = &item->predicates[0];
    assert(cm_hir_def_id_equal(predicate->trait_type.definition, gate)
        && predicate->trait_type.argument_count == 1u
        && predicate->trait_type.arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE);
    subject = cm_hir_get_type(context, predicate->subject);
    argument = cm_hir_get_type(context,
        predicate->trait_type.arguments[0].data.type);
    parameter = cm_hir_get_generic_param(context,
        item->generic_parameter_start);
    assert(subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == item->generic_parameter_start
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner, item->definition)
        && argument != NULL && argument->kind == CM_HIR_TYPE_INTEGER_KIND
        && argument->data.integer_type.kind == CM_HIR_INT_U8);
}

static void test_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId gate)
{
    static const unsigned char source_text[] =
        "use dep::GateReexport;\n"
        "pub fn direct<X: dep::Gate<u8>>(_x: X) {}\n"
        "pub fn via_alias<X: GateReexport<u8>>(_x: X) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *via_alias;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct");
    via_alias = find_item(context, CM_HIR_ITEM_FUNCTION, "via_alias");
    assert_gate_predicate(context, direct, gate);
    assert_gate_predicate(context, via_alias, gate);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_artifact_identity_same(const CmHirLibraryArtifact *artifact,
    const CmHirLibraryArtifactIdentity *expected)
{
    CmHirLibraryArtifactIdentity actual;
    assert(cm_hir_library_artifact_identity(artifact, &actual));
    assert(actual.context == expected->context
        && actual.crate_id == expected->crate_id
        && cm_hir_def_id_equal(actual.root_definition,
            expected->root_definition)
        && strcmp(actual.extern_name, expected->extern_name) == 0);
}

static void assert_expectation_rejected(CmHirContext *context,
    CmHirLibraryArtifact *artifact, const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    ContextLengths lengths, const CmHirLibraryArtifactIdentity *identity)
{
    CmHirDeclarationMaterializeResult result;
    result = cm_hir_declaration_metadata_materialize(context, artifact,
        metadata, expectation, "replacement", 91u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA
        && result.metadata_status
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert_context_lengths(context, lengths);
    assert_artifact_identity_same(artifact, identity);
}

static void test_materialize_decode_and_consume(void)
{
    TestFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryBinding canonical;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue needs;
    CmHirLibraryPathSegment value_path[2];
    const CmHirItem *gate_item;
    const CmHirItem *needs_item;
    const CmHirGenericParam *trait_parameter;
    ContextLengths lengths;
    CmHirLibraryArtifactIdentity identity;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationString saved_string;
    uint8_t saved_byte;
    size_t saved_count;
    uint32_t saved_owner;

    fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 77u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 1u);
    canonical = lookup_binding(&artifact, "Gate");
    reexport = lookup_binding(&artifact, "GateReexport");
    assert(canonical.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && reexport.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(canonical.definition, reexport.definition));
    gate_item = find_item(&context, CM_HIR_ITEM_TRAIT, "Gate");
    needs_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "needs");
    assert(gate_item != NULL && gate_item->generic_parameter_count == 1u
        && cm_hir_def_id_equal(gate_item->definition, canonical.definition));
    trait_parameter = cm_hir_get_generic_param(&context,
        gate_item->generic_parameter_start);
    assert(trait_parameter != NULL && trait_parameter->is_relaxed_sized == 1
        && cm_hir_def_id_equal(trait_parameter->owner,
            gate_item->definition));
    assert_gate_predicate(&context, needs_item, canonical.definition);
    value_path[0].bytes = (const unsigned char *)"dep";
    value_path[0].length = 3u;
    value_path[1].bytes = (const unsigned char *)"needs";
    value_path[1].length = 5u;
    memset(&needs, 0, sizeof(needs));
    assert(cm_hir_library_artifact_lookup_value(&artifact, value_path, 2u,
        &needs) == CM_HIR_LIBRARY_OK
        && needs.data.function.predicate_count == 1u
        && needs.data.function.nominal_reference_count == 1u
        && cm_hir_def_id_equal(needs.data.function.nominal_references[0]
                .definition,
            canonical.definition));

    test_fresh_consumer(&context, &artifact, canonical.definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    wrong = expectation;
    wrong.crate_name = (CmHirDeclarationString)S("wrongcrate");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.crate_disambiguator = (CmHirDeclarationString)S("wrong-disamb");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.edition = CM_HIR_DECL_EDITION_2024;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.target_triple = (CmHirDeclarationString)S("aarch64-unknown-linux-gnu");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.data_layout = (CmHirDeclarationString)S("e-p:32:32");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.panic_strategy = CM_HIR_DECL_PANIC_UNWIND;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.cfg_count = 1u;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    {
        CmHirDeclarationString wrong_cfgs[2];
        wrong_cfgs[0] = (CmHirDeclarationString)S("target_arch=aarch64");
        wrong_cfgs[1] = decoded.cfgs[1];
        wrong.cfgs = wrong_cfgs;
        assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
            lengths, &identity);
    }

    saved_owner = decoded.predicates[0].owner_value;
    decoded.predicates[0].owner_value = 2u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 92u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].owner_value = saved_owner;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_string = decoded.modules[0].name;
    decoded.modules[0].name = (CmHirDeclarationString)S("wrongroot");
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 93u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.modules[0].name = saved_string;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_byte = decoded.namespace_entries[1].target_kind;
    decoded.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_VALUE;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 94u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.namespace_entries[1].target_kind = saved_byte;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_count = decoded.predicates[0].argument_count;
    decoded.predicates[0].argument_count = 0u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 95u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].argument_count = (uint32_t)saved_count;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 96u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

int main(void)
{
    test_materialize_decode_and_consume();
    return 0;
}
