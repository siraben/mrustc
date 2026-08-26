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
    "#[unstable(feature = \"allocator_api\", issue = \"32838\")]\n"
    "#[derive(Copy, Clone, PartialEq, Eq, Debug)]\n"
    "pub struct AllocError;\n"
    "pub use AllocError as AllocAlias;\n"
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

static void fixture_init_source(CaptureFixture *fixture, int with_noise,
    const char *path, const unsigned char *source, size_t source_length)
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
    assert(cm_source_add_memory(&fixture->sources, path, source,
        source_length, &root) == CM_SOURCE_OK);
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

static void fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-trait-provider.rs",
        fixture_source, sizeof(fixture_source) - 1u);
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
    const CmHirDeclarationNamespaceEntry *alloc_alias_type;
    const CmHirDeclarationNamespaceEntry *alloc_error_type;
    const CmHirDeclarationNamespaceEntry *gate;
    const CmHirDeclarationNamespaceEntry *gate_alias;
    const CmHirDeclarationNamespaceEntry *alloc_alias_value;
    const CmHirDeclarationNamespaceEntry *alloc_error_value;
    const CmHirDeclarationNamespaceEntry *needs;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u);
    assert(metadata->trait_count == 1u && metadata->generic_count == 2u);
    assert(metadata->item_count == 1u
        && metadata->items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && metadata->items[0].owner_module == 1u
        && metadata->items[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PUBLIC
        && metadata->items[0].visibility.restriction_module == 0u
        && metadata->items[0].source_ordinal == 0u
        && metadata->items[0].name.length == strlen("AllocError")
        && memcmp(metadata->items[0].name.data, "AllocError",
            strlen("AllocError")) == 0);
    assert(metadata->type_count == 3u && metadata->value_count == 1u);
    assert(metadata->predicate_count == 1u && metadata->namespace_count == 7u);
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
    alloc_alias_type = &metadata->namespace_entries[0];
    alloc_error_type = &metadata->namespace_entries[1];
    gate = &metadata->namespace_entries[2];
    gate_alias = &metadata->namespace_entries[3];
    alloc_alias_value = &metadata->namespace_entries[4];
    alloc_error_value = &metadata->namespace_entries[5];
    needs = &metadata->namespace_entries[6];
    assert(alloc_alias_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_alias_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_type->target_local == 1u
        && alloc_alias_type->export_ordinal == 1u
        && alloc_error_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_error_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_type->target_local == 1u
        && alloc_error_type->export_ordinal == 0u);
    assert(gate->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate_alias->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate_alias->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate->target_local == 1u && gate_alias->target_local == 1u
        && gate->export_ordinal == 2u
        && gate_alias->export_ordinal == 3u);
    assert(alloc_alias_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_alias_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_value->target_local == 1u
        && alloc_alias_value->export_ordinal == 1u
        && alloc_error_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_error_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_value->target_local == 1u
        && alloc_error_value->export_ordinal == 0u);
    assert(needs->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && needs->target_kind == CM_HIR_DECL_TARGET_VALUE
        && needs->target_local == 1u && needs->export_ordinal == 4u);
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
        fprintf(stderr, "capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s item=%u type=%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_type);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NONE
        && result.failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE
        && result.trait_count == 1u && result.item_count == 1u
        && result.value_count == 1u && result.namespace_count == 7u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_ALLOWLISTED_UNIT_STRUCT
        && result.projected_semantic_attribute_count == 2u);
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
    static const char *const rejected_attributes[] = {
        "repr(C)",
        "lang = \"alloc_error\"",
        "rustc_layout_scalar_valid_range_start(0)",
        "no_mangle",
        "unknown_projection"
    };
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId needs_id;
    CmHirItemId alloc_id;
    const CmHirItem *needs_const;
    const CmHirItem *alloc_const;
    CmHirItem *needs;
    CmHirItem *alloc;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmInternId saved_attribute_metadata;
    size_t rejected_index;
    fixture_init(&fixture, 0);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    needs_const = find_item(&fixture, "needs", &needs_id);
    assert(needs_const != NULL);
    needs = (CmHirItem *)needs_const;
    needs->data.function_item.signature.safety = CM_HIR_UNSAFE;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == needs_id
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.has_rejected_span
        && metadata.values == saved_values
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    needs->data.function_item.signature.safety = CM_HIR_SAFE;

    alloc_const = find_item(&fixture, "AllocError", &alloc_id);
    assert(alloc_const != NULL && alloc_const->attribute_count == 2u
        && alloc_const->attributes != NULL);
    alloc = (CmHirItem *)alloc_const;
    alloc->data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.values == saved_values && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;

    alloc->generic_parameter_count = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->generic_parameter_count = 0u;

    alloc->predicate_count = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == alloc_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->predicate_count = 0u;

    saved_attribute_metadata = alloc->attributes[0].metadata;
    for (rejected_index = 0u;
            rejected_index < sizeof(rejected_attributes)
                / sizeof(rejected_attributes[0]); ++rejected_index) {
        alloc->attributes[0].metadata = cm_hir_intern(&fixture.hir,
            rejected_attributes[rejected_index]);
        result = cm_hir_declaration_metadata_capture(&input, &metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.rejected_item == alloc_id
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && result.has_rejected_span && metadata.items == saved_items
            && metadata.namespace_entries == saved_namespace
            && strcmp(cm_hir_declaration_capture_reason_name(
                result.failure_reason),
                "item-attribute-projection-unsupported") == 0);
    }
    alloc->attributes[0].metadata = saved_attribute_metadata;

    alloc->attributes[1].expansion_depth = 1u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attributes[1].expansion_depth = 0u;

    saved_attribute_metadata = alloc->attributes[1].metadata;
    alloc->attributes[1].metadata = alloc->attributes[0].metadata;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attributes[1].metadata = saved_attribute_metadata;

    alloc->attribute_count = 0u;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    alloc->attribute_count = 2u;

    input.revision += UINT64_C(1);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH
        && metadata.values == saved_values
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_zero_item_gate_path(void)
{
    static const unsigned char gate_source[] =
        "pub trait Gate<T: ?Sized> {}\n"
        "pub use Gate as GateReexport;\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    fixture_init_source(&fixture, 0, "zero-item-gate.rs", gate_source,
        sizeof(gate_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 0u && metadata.item_count == 0u
        && metadata.items == NULL && result.namespace_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        && result.projected_semantic_attribute_count == 0u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_plain_unit_struct_has_exact_empty_attribute_profile(void)
{
    static const unsigned char plain_source[] =
        "pub struct Plain;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    fixture_init_source(&fixture, 0, "plain-unit.rs", plain_source,
        sizeof(plain_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && metadata.item_count == 1u
        && result.namespace_count == 4u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        && result.projected_semantic_attribute_count == 0u
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_item_shape_diagnostic(void)
{
    static const unsigned char unsupported_source[] =
        "pub struct Blocked(pub u8);\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    const CmSourceFile *source;

    fixture_init_source(&fixture, 0, "unsupported-public-struct.rs",
        unsupported_source, sizeof(unsupported_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        || result.failure_stage != CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        || result.failure_reason
            != CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED) {
        fprintf(stderr, "item diagnostic status=%s stage=%s reason=%s "
            "binding=%u ast=%u def=%u:%u item=%u:%u span=%d\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            (unsigned int)result.rejected_binding_kind,
            (unsigned int)result.rejected_ast_item_kind,
            (unsigned int)result.rejected_definition.crate_id,
            (unsigned int)result.rejected_definition.index,
            (unsigned int)result.rejected_source_item.source,
            (unsigned int)result.rejected_source_item.item,
            result.has_rejected_span);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.rejected_binding_kind == CM_HIR_LIBRARY_BINDING_TYPE
        && result.rejected_ast_item_kind == CM_AST_ITEM_STRUCT
        && result.rejected_namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && result.rejected_definition.crate_id
            == fixture.lower_result.crate_id
        && result.rejected_definition.index != CM_HIR_DEF_INDEX_NONE
        && result.rejected_source_item.source != 0u
        && result.rejected_source_item.item != CM_AST_ITEM_NONE
        && result.has_rejected_span);
    source = cm_source_get(&fixture.sources, result.rejected_span.source);
    assert(source != NULL
        && strcmp(source->path, "unsupported-public-struct.rs") == 0
        && result.rejected_span.start == 0u
        && metadata.modules == NULL && metadata.module_count == 0u);
    assert(strcmp(cm_hir_declaration_capture_stage_name(
            result.failure_stage), "items") == 0
        && strcmp(cm_hir_declaration_capture_reason_name(
            result.failure_reason), "item-shape-unsupported") == 0);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_non_exhaustive_constructor_mate_is_required(void)
{
    static const unsigned char non_exhaustive_source[] =
        "#[non_exhaustive]\n"
        "pub struct SealedUnit;\n"
        "pub use SealedUnit as SealedAlias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture blocked;
    CmHirDeclarationCaptureInput good_input;
    CmHirDeclarationCaptureInput blocked_input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmResolveModuleInfo root_module;
    CmResolveEffectiveItem alias_effective;

    fixture_init(&good, 0);
    fixture_init_source(&blocked, 0, "non-exhaustive-unit.rs",
        non_exhaustive_source, sizeof(non_exhaustive_source) - 1u);
    good_input = capture_input(&good);
    blocked_input = capture_input(&blocked);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&good_input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    assert(cm_module_graph_get_module_at(&blocked.graph, 0u, &root_module)
        && cm_module_graph_get_effective_item(&blocked.graph,
            blocked.graph_result.revision, root_module.id, 1u,
            &alias_effective) == CM_RESOLVE_VIEW_OK);

    result = cm_hir_declaration_metadata_capture(&blocked_input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID
        && result.has_rejected_binding && result.has_rejected_target
        && result.rejected_binding_kind == CM_HIR_LIBRARY_BINDING_TYPE
        && result.rejected_ast_item_kind == CM_AST_ITEM_STRUCT
        && result.rejected_namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && result.rejected_source_item.source
            == alias_effective.declaration.source
        && result.rejected_source_item.item
            == alias_effective.declaration.item
        && result.has_rejected_span
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&blocked);
    fixture_destroy(&good);
}

static void test_many_private_bindings_do_not_consume_public_cap(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf source;
    char declaration[64];
    size_t index;
    int length;

    cm_byte_buf_init(&source);
    cm_byte_buf_append(&source, fixture_source, sizeof(fixture_source) - 1u);
    for (index = 0u; index < 2048u; ++index) {
        length = snprintf(declaration, sizeof(declaration),
            "fn private_%lu() {}\n", (unsigned long)index);
        assert(length > 0 && (size_t)length < sizeof(declaration));
        cm_byte_buf_append(&source, declaration, (size_t)length);
    }
    fixture_init_source(&fixture, 0, "many-private.rs", source.data,
        source.len);
    cm_byte_buf_destroy(&source);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NONE
        && result.failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE
        && result.namespace_count == 7u
        && result.item_count == 1u
        && result.projected_semantic_attribute_count == 2u);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_fixture_and_determinism();
    test_failure_is_atomic();
    test_zero_item_gate_path();
    test_plain_unit_struct_has_exact_empty_attribute_profile();
    test_item_shape_diagnostic();
    test_non_exhaustive_constructor_mate_is_required();
    test_many_private_bindings_do_not_consume_public_cap();
    return 0;
}
