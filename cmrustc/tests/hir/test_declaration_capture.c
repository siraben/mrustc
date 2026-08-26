#include "cm/hir/declaration_capture.h"
#include "cm/hir/lower.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct CaptureFixture {
    CmSourceSet sources;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmImportResolver imports;
    CmHirModuleMap modules;
    CmHirContext hir;
    CmModuleGraphResult graph_result;
    CmHirLowerResult lower_result;
    CmHirArtifactConfig config;
} CaptureFixture;

static const unsigned char fixture_source[] =
    "pub trait Gate<T: ?Sized> {}\n"
    "pub use Gate as GateReexport;\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static CmHirArtifactBytes test_bytes(const char *text)
{
    CmHirArtifactBytes bytes;
    bytes.data = (const unsigned char *)text;
    bytes.length = strlen(text);
    return bytes;
}

static void fixture_init(CaptureFixture *fixture, int with_noise)
{
    CmSourceId root;
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_import_resolver_init(&fixture->imports);
    cm_hir_module_map_init(&fixture->modules);
    cm_hir_context_init(&fixture->hir);
    cm_hir_artifact_config_init(&fixture->config);
    if (with_noise) {
        CmSourceId ignored_source;
        CmHirCrateId ignored_crate;
        CmHirModuleId ignored_module;
        CmHirType type;
        CmHirTypeId ignored_type;
        static const unsigned char ignored[] = "pub struct Noise;\n";
        assert(cm_source_add_memory(&fixture->sources, "noise.rs", ignored,
            sizeof(ignored) - 1u, &ignored_source) == CM_SOURCE_OK);
        assert(cm_hir_create_crate(&fixture->hir,
            cm_hir_intern(&fixture->hir, "noise"), CM_HIR_EDITION_2024,
            (CmSpan){ ignored_source, 0u, 1u }, &ignored_crate,
            &ignored_module) == CM_HIR_OK);
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_UNIT_KIND;
        type.span = (CmSpan){ ignored_source, 0u, 1u };
        assert(cm_hir_add_type(&fixture->hir, &type, &ignored_type)
            == CM_HIR_OK);
    }
    assert(cm_source_add_memory(&fixture->sources,
        "v30-trait-provider.rs", fixture_source,
        sizeof(fixture_source) - 1u, &root) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2024;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, root, &graph_options);
    assert(fixture->graph_result.error_count == 0u);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(import_result.error_count == 0u
        && import_result.revision == fixture->graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "v30_provider";
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.source = root;
    fixture->lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->modules, &lower_options);
    assert(fixture->lower_result.error_count == 0u);
    fixture->config.edition = UINT32_C(2024);
    fixture->config.panic_strategy = test_bytes("abort");
}

static void fixture_destroy(CaptureFixture *fixture)
{
    cm_hir_artifact_config_destroy(&fixture->config);
    cm_hir_module_map_destroy(&fixture->modules);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
    cm_hir_context_destroy(&fixture->hir);
}

static CmHirDeclarationCaptureInput capture_input(
    const CaptureFixture *fixture)
{
    CmHirDeclarationCaptureInput input;
    memset(&input, 0, sizeof(input));
    input.hir = &fixture->hir;
    input.crate_id = fixture->lower_result.crate_id;
    input.graph = &fixture->graph;
    input.revision = fixture->graph_result.revision;
    input.imports = &fixture->imports;
    input.modules = &fixture->modules;
    input.configuration = &fixture->config;
    input.crate_disambiguator = test_bytes("capture-test-disambiguator");
    input.target_triple = test_bytes("x86_64-unknown-linux-gnu");
    input.data_layout = test_bytes("e-p:64:64-i64:64-n8:16:32:64-S128");
    return input;
}

static const CmHirItem *find_item(const CaptureFixture *fixture,
    const char *name, CmHirItemId *out_id)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&fixture->hir.strings, item->name);
        if (item != NULL
            && item->definition.crate_id == fixture->lower_result.crate_id
            && item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            *out_id = (CmHirItemId)(index + 1u);
            return item;
        }
    }
    return NULL;
}

static void assert_exact_descriptor(const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *gate;
    const CmHirDeclarationNamespaceEntry *alias;
    const CmHirDeclarationNamespaceEntry *needs;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u);
    assert(metadata->trait_count == 1u && metadata->generic_count == 2u);
    assert(metadata->type_count == 3u && metadata->value_count == 1u);
    assert(metadata->predicate_count == 1u && metadata->namespace_count == 3u);
    assert(metadata->generics[0].owner_kind == CM_HIR_DECL_GENERIC_NOMINAL
        && metadata->generics[0].owner_local == 1u
        && metadata->generics[0].index == 0u
        && metadata->generics[0].is_relaxed_sized == 1u);
    assert(metadata->generics[1].owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && metadata->generics[1].owner_local == 1u
        && metadata->generics[1].index == 0u
        && metadata->generics[1].is_relaxed_sized == 0u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT);
    assert(metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_U8);
    assert(metadata->types[2].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[2].generic_local == 2u);
    assert(metadata->values[0].parameter_count == 0u
        && metadata->values[0].parameter_types == NULL
        && metadata->values[0].return_type == 1u
        && metadata->values[0].has_body == 1u
        && metadata->values[0].predicate_start == 1u
        && metadata->values[0].predicate_count == 1u);
    assert(metadata->predicates[0].owner_value == 1u
        && metadata->predicates[0].ordinal == 0u
        && metadata->predicates[0].subject_type == 3u
        && metadata->predicates[0].trait_local == 1u
        && metadata->predicates[0].argument_count == 1u
        && metadata->predicates[0].argument_types[0] == 2u);
    gate = &metadata->namespace_entries[0];
    alias = &metadata->namespace_entries[1];
    needs = &metadata->namespace_entries[2];
    assert(gate->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alias->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && alias->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate->target_local == 1u && alias->target_local == 1u);
    assert(needs->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && needs->target_kind == CM_HIR_DECL_TARGET_VALUE
        && needs->target_local == 1u);
}

static void test_fixture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput first_input;
    CmHirDeclarationCaptureInput noisy_input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    fixture_init(&first, 0);
    fixture_init(&noisy, 1);
    first_input = capture_input(&first);
    noisy_input = capture_input(&noisy);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    result = cm_hir_declaration_metadata_capture(&first_input,
        &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "capture failed: %s metadata=%s library=%s "
            "item=%u type=%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_type);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.trait_count == 1u && result.value_count == 1u);
    result = cm_hir_declaration_metadata_capture(&noisy_input,
        &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    assert_exact_descriptor(&first_metadata);
    assert_exact_descriptor(&noisy_metadata);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&noisy_bytes);
    assert(cm_hir_declaration_metadata_encode(&first_metadata, &first_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&noisy_metadata, &noisy_bytes)
            == CM_HIR_DECL_METADATA_OK
        && first_bytes.len == noisy_bytes.len
        && memcmp(first_bytes.data, noisy_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&noisy_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_declaration_metadata_destroy(&noisy_metadata);
    cm_hir_declaration_metadata_destroy(&first_metadata);
    fixture_destroy(&noisy);
    fixture_destroy(&first);
}

static void test_failure_is_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId needs_id;
    const CmHirItem *needs_const;
    CmHirItem *needs;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    needs_const = find_item(&fixture, "needs", &needs_id);
    assert(needs_const != NULL);
    needs = (CmHirItem *)needs_const;
    needs->data.function_item.signature.safety = CM_HIR_UNSAFE;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == needs_id
        && metadata.values == saved_values
        && metadata.namespace_entries == saved_namespace);
    needs->data.function_item.signature.safety = CM_HIR_SAFE;
    input.revision += UINT64_C(1);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY
        && metadata.values == saved_values
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_fixture_and_determinism();
    test_failure_is_atomic();
    return 0;
}
