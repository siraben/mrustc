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
    "mod layout {\n"
    "  #[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
    "  #[deprecated(since = \"1.52.0\", note = \"use LayoutError\")]\n"
    "  pub type LayoutErr = LayoutError;\n"
    "  #[stable(feature = \"alloc_layout_error\", since = \"1.50.0\")]\n"
    "  #[non_exhaustive]\n"
    "  #[derive(Clone, PartialEq, Eq, Debug)]\n"
    "  pub struct LayoutError;\n"
    "}\n"
    "#[stable(feature = \"alloc_layout\", since = \"1.28.0\")]\n"
    "#[deprecated(since = \"1.52.0\", note = \"use LayoutError\")]\n"
    "#[allow(deprecated, deprecated_in_future)]\n"
    "pub use layout::LayoutErr;\n"
    "#[stable(feature = \"alloc_layout_error\", since = \"1.50.0\")]\n"
    "pub use layout::LayoutError;\n"
    "#[unstable(feature = \"allocator_api\", issue = \"32838\")]\n"
    "#[derive(Copy, Clone, PartialEq, Eq, Debug)]\n"
    "pub struct AllocError;\n"
    "pub use AllocError as AllocAlias;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub use Gate as GateReexport;\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char const_fixture_source[] =
    "#[stable(feature = \"rust1\", since = \"1.0.0\")]\n"
    "pub const MAX: char = char::MAX;\n"
    "#[unstable(feature = \"next_char\", issue = \"none\")]\n"
    "pub const NEXT: usize = usize::MAX;\n"
    "#[deprecated(since = \"1.1.0\", note = \"old\")]\n"
    "pub const OLD: char = char::MAX;\n"
    "#[deprecated(since = \"1.1.0\", note = \"renamed\")]\n"
    "pub use MAX as RENAMED;\n"
    "pub trait Gate<T: ?Sized> {}\n"
    "pub fn needs<X: Gate<u8>>() {}\n";

static const unsigned char default_enum_fixture_source[] =
    "#[rustc_diagnostic_item = \"mir_basic_block\"]\n"
    "pub enum BasicBlock { Normal, Cleanup }\n"
    "#[rustc_diagnostic_item = \"mir_unwind_terminate_reason\"]\n"
    "pub enum UnwindTerminateReason { Abi, InCleanup }\n"
    "pub use UnwindTerminateReason::{\n"
    "  Abi as ReasonAbi, InCleanup as ReasonInCleanup\n"
    "};\n"
    "pub trait Gate<T: ?Sized> {}\n"
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

static void const_fixture_init(CaptureFixture *fixture, int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-const-provider.rs",
        const_fixture_source, sizeof(const_fixture_source) - 1u);
}

static void default_enum_fixture_init(CaptureFixture *fixture,
    int with_noise)
{
    fixture_init_source(fixture, with_noise, "v30-default-enum-provider.rs",
        default_enum_fixture_source,
        sizeof(default_enum_fixture_source) - 1u);
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

static const CmHirModule *find_module(const CaptureFixture *fixture,
    const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < fixture->hir.modules.len; ++index) {
        const CmHirModule *module = (const CmHirModule *)cm_vec_at_const(
            &fixture->hir.modules, index);
        const CmInternedString *module_name = module == NULL ? NULL
            : cm_interner_get(&fixture->hir.strings, module->name);
        if (module != NULL && module_name != NULL
            && module_name->len == length
            && memcmp(module_name->bytes, name, length) == 0) return module;
    }
    return NULL;
}

static CmHirImport *find_unique_attributed_import(CaptureFixture *fixture)
{
    CmHirImport *result = NULL;
    size_t module_index;
    for (module_index = 0u; module_index < fixture->hir.modules.len;
            ++module_index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(
            &fixture->hir.modules, module_index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != fixture->lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *candidate = &module->imports[import_index];
            if (candidate->attribute_count == 0u) continue;
            assert(result == NULL);
            result = candidate;
        }
    }
    return result;
}

static int declaration_string_is(CmHirDeclarationString value,
    const char *text)
{
    size_t length = strlen(text);
    return value.length == length
        && memcmp(value.data, text, length) == 0;
}

static const CmHirDeclarationNamespaceEntry *find_namespace_entry(
    const CmHirDeclarationMetadata *metadata, uint32_t owner_module,
    uint8_t namespace_kind, const char *name)
{
    size_t index;
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        if (entry->owner_module == owner_module
            && entry->namespace_kind == namespace_kind
            && declaration_string_is(entry->name, name)) return entry;
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
    const CmHirDeclarationNamespaceEntry *layout_err_direct;
    const CmHirDeclarationNamespaceEntry *layout_err_export;
    const CmHirDeclarationNamespaceEntry *layout_error_direct;
    const CmHirDeclarationNamespaceEntry *layout_error_export;
    const CmHirDeclarationNamespaceEntry *needs;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 2u && metadata->root_module == 1u
        && metadata->modules[1].parent_module == 1u
        && declaration_string_is(metadata->modules[1].name, "layout"));
    assert(metadata->trait_count == 1u && metadata->generic_count == 2u);
    assert(metadata->item_count == 3u
        && metadata->items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && metadata->items[0].owner_module == 1u
        && metadata->items[0].visibility.kind
            == CM_HIR_DECL_VISIBILITY_PUBLIC
        && metadata->items[0].visibility.restriction_module == 0u
        && metadata->items[0].source_ordinal == 3u
        && metadata->items[0].alias_target_type == 0u
        && declaration_string_is(metadata->items[0].name, "AllocError")
        && metadata->items[1].kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
        && metadata->items[1].owner_module == 2u
        && metadata->items[1].source_ordinal == 0u
        && metadata->items[1].alias_target_type == 4u
        && declaration_string_is(metadata->items[1].name, "LayoutErr")
        && metadata->items[2].kind == CM_HIR_DECL_ITEM_STRUCT
        && metadata->items[2].owner_module == 2u
        && metadata->items[2].source_ordinal == 1u
        && metadata->items[2].alias_target_type == 0u
        && declaration_string_is(metadata->items[2].name, "LayoutError"));
    assert(metadata->type_count == 4u && metadata->value_count == 1u);
    assert(metadata->predicate_count == 1u
        && metadata->namespace_count == 11u);
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
        && metadata->types[2].generic_local == 2u
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_NAMED_ADT
        && metadata->types[3].item_local == 3u);
    assert(metadata->values[0].kind == CM_HIR_DECL_VALUE_FUNCTION
        && metadata->values[0].declared_type == 0u
        && metadata->values[0].mutability == 0u
        && metadata->values[0].parameter_count == 0u
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
    alloc_alias_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AllocAlias");
    alloc_error_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AllocError");
    gate = find_namespace_entry(metadata, 1u, CM_HIR_DECL_NAMESPACE_TYPE,
        "Gate");
    gate_alias = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "GateReexport");
    layout_err_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutErr");
    layout_error_export = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutError");
    alloc_alias_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "AllocAlias");
    alloc_error_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "AllocError");
    needs = find_namespace_entry(metadata, 1u, CM_HIR_DECL_NAMESPACE_VALUE,
        "needs");
    layout_err_direct = find_namespace_entry(metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutErr");
    layout_error_direct = find_namespace_entry(metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "LayoutError");
    assert(alloc_alias_type != NULL && alloc_error_type != NULL
        && gate != NULL && gate_alias != NULL && layout_err_export != NULL
        && layout_error_export != NULL && alloc_alias_value != NULL
        && alloc_error_value != NULL && needs != NULL
        && layout_err_direct != NULL && layout_error_direct != NULL);
    assert(alloc_alias_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_alias_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_type->target_local == 1u
        && alloc_alias_type->export_ordinal == 4u
        && alloc_error_type->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && alloc_error_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_type->target_local == 1u
        && alloc_error_type->export_ordinal == 3u);
    assert(gate->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate_alias->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
        && gate->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate_alias->target_kind == CM_HIR_DECL_TARGET_NOMINAL
        && gate->target_local == 1u && gate_alias->target_local == 1u
        && gate->export_ordinal == 5u
        && gate_alias->export_ordinal == 6u);
    assert(layout_err_export->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_err_export->target_local == 2u
        && layout_err_export->export_ordinal == 1u
        && layout_err_direct->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_err_direct->target_local == 2u
        && layout_err_direct->export_ordinal == 0u
        && layout_error_export->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_error_export->target_local == 3u
        && layout_error_export->export_ordinal == 2u
        && layout_error_direct->target_kind == CM_HIR_DECL_TARGET_ITEM
        && layout_error_direct->target_local == 3u
        && layout_error_direct->export_ordinal == 1u
        && find_namespace_entry(metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutErr") == NULL
        && find_namespace_entry(metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutError") == NULL
        && find_namespace_entry(metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutErr") == NULL
        && find_namespace_entry(metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "LayoutError") == NULL);
    assert(alloc_alias_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_alias_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_alias_value->target_local == 1u
        && alloc_alias_value->export_ordinal == 4u
        && alloc_error_value->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && alloc_error_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && alloc_error_value->target_local == 1u
        && alloc_error_value->export_ordinal == 3u);
    assert(needs->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
        && needs->target_kind == CM_HIR_DECL_TARGET_VALUE
        && needs->target_local == 1u && needs->export_ordinal == 7u);
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
        && result.trait_count == 1u && result.item_count == 3u
        && result.value_count == 1u && result.namespace_count == 11u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION
        && result.projected_semantic_attribute_count == 11u);
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
    const CmHirDeclarationNamespaceEntry *plain_type;
    const CmHirDeclarationNamespaceEntry *plain_value;
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
    plain_type = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Plain");
    plain_value = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "Plain");
    assert(plain_type != NULL && plain_value != NULL
        && plain_type->target_kind == CM_HIR_DECL_TARGET_ITEM
        && plain_value->target_kind == CM_HIR_DECL_TARGET_ITEM
        && plain_type->target_local == plain_value->target_local
        && plain_type->export_ordinal == plain_value->export_ordinal);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_module_attribute_projection_and_provenance(void)
{
    static const unsigned char plain_source[] =
        "pub mod child {}\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char attributed_source[] =
        "#![allow(dead_code)]\n"
        "#[allow(non_snake_case)]\n"
        "pub mod child { #![allow(unused)] }\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture plain;
    CaptureFixture attributed;
    CmHirDeclarationCaptureInput plain_input;
    CmHirDeclarationCaptureInput attributed_input;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf plain_bytes;
    CmByteBuf attributed_bytes;
    CmHirCrate *crate_value;
    CmHirModule *child;
    CmHirAttribute *saved_child_attributes;
    CmInternId saved_metadata;
    CmSpan saved_span;
    CmHirDeclarationModule *saved_modules;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init_source(&plain, 0, "module-attributes.rs", plain_source,
        sizeof(plain_source) - 1u);
    fixture_init_source(&attributed, 0, "module-attributes.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    plain_input = capture_input(&plain);
    attributed_input = capture_input(&attributed);
    cm_hir_declaration_metadata_init(&plain_metadata);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    result = cm_hir_declaration_metadata_capture(&plain_input,
        &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE);
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    cm_byte_buf_init(&plain_bytes);
    cm_byte_buf_init(&attributed_bytes);
    assert(cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && plain_bytes.len == attributed_bytes.len
        && memcmp(plain_bytes.data, attributed_bytes.data,
            plain_bytes.len) == 0);

    saved_modules = attributed_metadata.modules;
    saved_namespace = attributed_metadata.namespace_entries;
    crate_value = (CmHirCrate *)cm_hir_get_crate(&attributed.hir,
        attributed.lower_result.crate_id);
    child = (CmHirModule *)find_module(&attributed, "child");
    assert(crate_value != NULL && crate_value->inner_attribute_count == 1u
        && crate_value->inner_attributes != NULL && child != NULL
        && child->outer_attribute_count == 1u
        && child->inner_attribute_count == 1u
        && child->inner_attributes != NULL);

    crate_value->inner_attribute_count = 0u;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_MODULES
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attribute_count = 1u;

    saved_metadata = crate_value->inner_attributes[0].metadata;
    crate_value->inner_attributes[0].metadata = CM_INTERN_ID_NONE;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attributes[0].metadata = saved_metadata;

    saved_span = crate_value->inner_attributes[0].span;
    crate_value->inner_attributes[0].span.start = 2u;
    crate_value->inner_attributes[0].span.end = 1u;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && result.has_rejected_span
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    crate_value->inner_attributes[0].span = saved_span;

    saved_child_attributes = child->inner_attributes;
    child->inner_attributes = NULL;
    result = cm_hir_declaration_metadata_capture(&attributed_input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID
        && attributed_metadata.modules == saved_modules
        && attributed_metadata.namespace_entries == saved_namespace);
    child->inner_attributes = saved_child_attributes;

    cm_byte_buf_destroy(&attributed_bytes);
    cm_byte_buf_destroy(&plain_bytes);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    fixture_destroy(&attributed);
    fixture_destroy(&plain);
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

static void test_non_exhaustive_authorizes_missing_constructor_mate(void)
{
    static const unsigned char non_exhaustive_source[] =
        "#[non_exhaustive]\n"
        "pub struct SealedUnit;\n"
        "pub use SealedUnit as SealedAlias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    const CmHirDeclarationNamespaceEntry *sealed;
    const CmHirDeclarationNamespaceEntry *sealed_alias;

    fixture_init_source(&fixture, 0, "non-exhaustive-unit.rs",
        non_exhaustive_source, sizeof(non_exhaustive_source) - 1u);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 1u
        && metadata.item_count == 1u
        && metadata.items[0].kind == CM_HIR_DECL_ITEM_STRUCT
        && declaration_string_is(metadata.items[0].name, "SealedUnit"));
    sealed = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "SealedUnit");
    sealed_alias = find_namespace_entry(&metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "SealedAlias");
    assert(sealed != NULL && sealed_alias != NULL
        && sealed->target_kind == CM_HIR_DECL_TARGET_ITEM
        && sealed_alias->target_kind == CM_HIR_DECL_TARGET_ITEM
        && sealed->target_local == 1u && sealed_alias->target_local == 1u
        && find_namespace_entry(&metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "SealedUnit") == NULL
        && find_namespace_entry(&metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "SealedAlias") == NULL
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_char_shaped_reexport_projection(void)
{
    static const unsigned char attributed_source[] =
        "mod ascii_char {\n"
        "  #[non_exhaustive]\n"
        "  pub struct AsciiChar;\n"
        "}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char plain_source[] =
        "mod ascii_char {\n"
        "  #[non_exhaustive]\n"
        "  pub struct AsciiChar;\n"
        "}\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture attributed;
    CaptureFixture plain;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata attributed_metadata;
    CmHirDeclarationMetadata plain_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf attributed_bytes;
    CmByteBuf plain_bytes;
    const CmHirDeclarationNamespaceEntry *definition;
    const CmHirDeclarationNamespaceEntry *reexport;
    fixture_init_source(&attributed, 0, "ascii-char-projection.rs",
        attributed_source, sizeof(attributed_source) - 1u);
    fixture_init_source(&plain, 0, "ascii-char-projection.rs", plain_source,
        sizeof(plain_source) - 1u);
    cm_hir_declaration_metadata_init(&attributed_metadata);
    cm_hir_declaration_metadata_init(&plain_metadata);
    input = capture_input(&attributed);
    result = cm_hir_declaration_metadata_capture(&input,
        &attributed_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 3u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION);
    definition = find_namespace_entry(&attributed_metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AsciiChar");
    reexport = find_namespace_entry(&attributed_metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Char");
    assert(definition != NULL && reexport != NULL
        && definition->target_kind == CM_HIR_DECL_TARGET_ITEM
        && reexport->target_kind == CM_HIR_DECL_TARGET_ITEM
        && definition->target_local == reexport->target_local
        && find_namespace_entry(&attributed_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "Char") == NULL);
    input = capture_input(&plain);
    result = cm_hir_declaration_metadata_capture(&input, &plain_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 1u);
    cm_byte_buf_init(&attributed_bytes);
    cm_byte_buf_init(&plain_bytes);
    assert(cm_hir_declaration_metadata_encode(&attributed_metadata,
            &attributed_bytes) == CM_HIR_DECL_METADATA_OK
        && cm_hir_declaration_metadata_encode(&plain_metadata, &plain_bytes)
            == CM_HIR_DECL_METADATA_OK
        && attributed_bytes.len == plain_bytes.len
        && memcmp(attributed_bytes.data, plain_bytes.data,
            attributed_bytes.len) == 0);
    cm_byte_buf_destroy(&plain_bytes);
    cm_byte_buf_destroy(&attributed_bytes);
    cm_hir_declaration_metadata_destroy(&plain_metadata);
    cm_hir_declaration_metadata_destroy(&attributed_metadata);
    fixture_destroy(&plain);
    fixture_destroy(&attributed);
}

static void test_ascii_char_enum_projection_and_determinism(void)
{
    static const unsigned char source[] =
        "mod ascii_char {\n"
        "  #[derive(Copy, Clone, Eq, PartialEq)]\n"
        "  #[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "  #[repr(u8)]\n"
        "  pub enum AsciiChar {\n"
        "    #[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "    Null = 0,\n"
        "    #[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "    StartOfHeading = 1,\n"
        "  }\n"
        "}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    const CmHirDeclarationNamespaceEntry *definition;
    const CmHirDeclarationNamespaceEntry *reexport;
    fixture_init_source(&first, 0, "ascii-char-enum.rs", source,
        sizeof(source) - 1u);
    fixture_init_source(&noisy, 1, "ascii-char-enum.rs", source,
        sizeof(source) - 1u);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "enum capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s item=%u span=%u:%u-%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_span.source,
            (unsigned int)result.rejected_span.start,
            (unsigned int)result.rejected_span.end);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 1u && result.namespace_count == 4u
        && result.projected_semantic_attribute_count == 6u
        && first_metadata.item_count == 1u
        && first_metadata.items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && first_metadata.items[0].enum_repr_primitive
            == CM_HIR_DECL_PRIMITIVE_U8
        && first_metadata.items[0].diagnostic_item.data == NULL
        && first_metadata.items[0].diagnostic_item.length == 0u
        && first_metadata.items[0].variant_count == 2u
        && first_metadata.items[0].variants != NULL
        && first_metadata.items[0].variants[0].kind
            == CM_HIR_DECL_VARIANT_UNIT
        && first_metadata.items[0].variants[0].source_ordinal == 0u
        && first_metadata.items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_PRIMITIVE_ISIZE
        && first_metadata.items[0].variants[0].discriminant_low == 0u
        && first_metadata.items[0].variants[0].discriminant_high == 0u
        && declaration_string_is(first_metadata.items[0].variants[0].name,
            "Null")
        && first_metadata.items[0].variants[1].source_ordinal == 1u
        && first_metadata.items[0].variants[1].discriminant_low == 1u
        && declaration_string_is(first_metadata.items[0].variants[1].name,
            "StartOfHeading"));
    definition = find_namespace_entry(&first_metadata, 2u,
        CM_HIR_DECL_NAMESPACE_TYPE, "AsciiChar");
    reexport = find_namespace_entry(&first_metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "Char");
    assert(definition != NULL && reexport != NULL
        && definition->target_kind == CM_HIR_DECL_TARGET_ITEM
        && reexport->target_kind == CM_HIR_DECL_TARGET_ITEM
        && definition->target_local == 1u
        && reexport->target_local == definition->target_local
        && find_namespace_entry(&first_metadata, 2u,
            CM_HIR_DECL_NAMESPACE_VALUE, "AsciiChar") == NULL
        && find_namespace_entry(&first_metadata, 1u,
            CM_HIR_DECL_NAMESPACE_VALUE, "Char") == NULL);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 6u);
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

static void assert_default_enum_descriptor(
    const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *basic_block;
    const CmHirDeclarationNamespaceEntry *unwind;
    const CmHirDeclarationNamespaceEntry *reason_abi_type;
    const CmHirDeclarationNamespaceEntry *reason_abi_value;
    const CmHirDeclarationNamespaceEntry *reason_cleanup_type;
    const CmHirDeclarationNamespaceEntry *reason_cleanup_value;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u
        && metadata->item_count == 2u
        && metadata->namespace_count == 8u
        && metadata->items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata->items[0].owner_module == 1u
        && metadata->items[0].source_ordinal == 0u
        && declaration_string_is(metadata->items[0].name, "BasicBlock")
        && metadata->items[0].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_RUST
        && declaration_string_is(metadata->items[0].diagnostic_item,
            "mir_basic_block")
        && metadata->items[0].variant_count == 2u
        && declaration_string_is(metadata->items[0].variants[0].name,
            "Normal")
        && metadata->items[0].variants[0].source_ordinal == 0u
        && metadata->items[0].variants[0].discriminant_primitive
            == CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
        && metadata->items[0].variants[0].discriminant_low == 0u
        && metadata->items[0].variants[0].discriminant_high == 0u
        && declaration_string_is(metadata->items[0].variants[1].name,
            "Cleanup")
        && metadata->items[0].variants[1].source_ordinal == 1u
        && metadata->items[0].variants[1].discriminant_primitive
            == CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
        && metadata->items[0].variants[1].discriminant_low == 0u
        && metadata->items[0].variants[1].discriminant_high == 0u
        && metadata->items[1].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata->items[1].owner_module == 1u
        && metadata->items[1].source_ordinal == 1u
        && declaration_string_is(metadata->items[1].name,
            "UnwindTerminateReason")
        && metadata->items[1].enum_repr_primitive
            == CM_HIR_DECL_ENUM_REPR_RUST
        && declaration_string_is(metadata->items[1].diagnostic_item,
            "mir_unwind_terminate_reason")
        && metadata->items[1].variant_count == 2u
        && declaration_string_is(metadata->items[1].variants[0].name,
            "Abi")
        && declaration_string_is(metadata->items[1].variants[1].name,
            "InCleanup"));
    basic_block = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "BasicBlock");
    unwind = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "UnwindTerminateReason");
    reason_abi_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "ReasonAbi");
    reason_abi_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "ReasonAbi");
    reason_cleanup_type = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_TYPE, "ReasonInCleanup");
    reason_cleanup_value = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "ReasonInCleanup");
    assert(basic_block != NULL && unwind != NULL
        && reason_abi_type != NULL && reason_abi_value != NULL
        && reason_cleanup_type != NULL && reason_cleanup_value != NULL
        && basic_block->target_kind == CM_HIR_DECL_TARGET_ITEM
        && basic_block->target_local == 1u
        && unwind->target_kind == CM_HIR_DECL_TARGET_ITEM
        && unwind->target_local == 2u
        && reason_abi_type->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_abi_value->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_abi_type->target_local == 3u
        && reason_abi_value->target_local == 3u
        && reason_abi_type->export_ordinal
            == reason_abi_value->export_ordinal
        && reason_cleanup_type->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_cleanup_value->target_kind
            == CM_HIR_DECL_TARGET_ENUM_VARIANT
        && reason_cleanup_type->target_local == 4u
        && reason_cleanup_value->target_local == 4u
        && reason_cleanup_type->export_ordinal
            == reason_cleanup_value->export_ordinal);
}

static void test_default_enum_variant_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    default_enum_fixture_init(&first, 0);
    default_enum_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    if (result.status != CM_HIR_DECL_CAPTURE_OK) {
        fprintf(stderr, "default enum capture failed: %s stage=%s reason=%s "
            "metadata=%s library=%s binding=%u ast=%u namespace=%u "
            "item=%u def=%u:%u span=%u:%u-%u\n",
            cm_hir_declaration_capture_status_name(result.status),
            cm_hir_declaration_capture_stage_name(result.failure_stage),
            cm_hir_declaration_capture_reason_name(result.failure_reason),
            cm_hir_declaration_metadata_status_name(result.metadata_status),
            cm_hir_library_status_name(result.library_status),
            (unsigned int)result.rejected_binding_kind,
            (unsigned int)result.rejected_ast_item_kind,
            (unsigned int)result.rejected_namespace_kind,
            (unsigned int)result.rejected_item,
            (unsigned int)result.rejected_definition.crate_id,
            (unsigned int)result.rejected_definition.index,
            (unsigned int)result.rejected_span.source,
            (unsigned int)result.rejected_span.start,
            (unsigned int)result.rejected_span.end);
    }
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.item_count == 2u && result.namespace_count == 8u
        && result.projected_semantic_attribute_count == 0u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 0u);
    assert_default_enum_descriptor(&first_metadata);
    assert_default_enum_descriptor(&noisy_metadata);
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

static void test_default_enum_hostile_mutations_are_atomic(void)
{
    static const char *const rejected_sources[] = {
        "#[rustc_diagnostic_item = \"\"]\n"
        "pub enum Bad { One }\n",
        "#[rustc_diagnostic_item = \"bad-item\"]\n"
        "pub enum Bad { One }\n",
        "#[rustc_diagnostic_item = \"bad_explicit\"]\n"
        "pub enum Bad { One = 0 }\n",
        "#[rustc_diagnostic_item = \"bad_attr\"]\n"
        "pub enum Bad { #[unstable(feature = \"bad\", issue = \"none\")] One }\n",
        "#[rustc_diagnostic_item = \"bad_tuple\"]\n"
        "pub enum Bad { One(u8) }\n"
    };
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    const CmHirItem *item_const;
    CmHirItem *item;
    CmHirItemId item_id;
    CmInternId saved_metadata;
    CmHirAttribute *saved_attributes;
    CmSpan saved_attribute_span;
    CmHirDefId saved_variant_definition;
    CmHirAggregateForm saved_variant_form;
    CmHirImportBinding *reason_abi_value = NULL;
    CmHirDefId reason_cleanup_definition;
    int saved_has_discriminant;
    size_t index;
    default_enum_fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    item_const = find_item(&good, "BasicBlock", &item_id);
    assert(item_const != NULL && item_const->kind == CM_HIR_ITEM_ENUM
        && item_const->attribute_count == 1u
        && item_const->attributes != NULL
        && item_const->data.enum_item.variant_count == 2u);
    item = (CmHirItem *)item_const;

#define ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.items == saved_items \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&good.hir,
        "rustc_diagnostic_item = \"forged-name\"");
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;

    item->attributes[0].expansion_depth = 1u;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].expansion_depth = 0u;

    saved_attribute_span = item->attributes[0].span;
    item->attributes[0].span.start += 1u;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes[0].span = saved_attribute_span;

    saved_attributes = item->attributes;
    item->attributes = NULL;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->attributes = saved_attributes;

    saved_has_discriminant =
        item->data.enum_item.variants[0].has_discriminant;
    item->data.enum_item.variants[0].has_discriminant = 1;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].has_discriminant =
        saved_has_discriminant;

    saved_variant_definition =
        item->data.enum_item.variants[0].definition;
    item->data.enum_item.variants[0].definition =
        item->data.enum_item.variants[1].definition;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].definition = saved_variant_definition;

    saved_variant_form = item->data.enum_item.variants[0].form;
    item->data.enum_item.variants[0].form = CM_HIR_AGGREGATE_TUPLE;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    item->data.enum_item.variants[0].form = saved_variant_form;

    memset(&reason_cleanup_definition, 0, sizeof(reason_cleanup_definition));
    for (index = 0u; index < good.hir.modules.len; ++index) {
        CmHirModule *module = (CmHirModule *)cm_vec_at(&good.hir.modules,
            index);
        uint32_t import_index;
        if (module == NULL
            || module->crate_id != good.lower_result.crate_id) continue;
        for (import_index = 0u; import_index < module->import_count;
                ++import_index) {
            CmHirImport *import = &module->imports[import_index];
            uint32_t binding_index;
            for (binding_index = 0u; binding_index < import->binding_count;
                    ++binding_index) {
                CmHirImportBinding *binding =
                    &import->bindings[binding_index];
                const CmInternedString *name = cm_interner_get(
                    &good.hir.strings, binding->name);
                if (name != NULL && name->len == strlen("ReasonAbi")
                    && memcmp(name->bytes, "ReasonAbi", name->len) == 0
                    && binding->namespace_kind == CM_HIR_NAMESPACE_VALUE)
                    reason_abi_value = binding;
                if (name != NULL && name->len == strlen("ReasonInCleanup")
                    && memcmp(name->bytes, "ReasonInCleanup",
                        name->len) == 0
                    && binding->namespace_kind == CM_HIR_NAMESPACE_VALUE)
                    reason_cleanup_definition = binding->target;
            }
        }
    }
    assert(reason_abi_value != NULL
        && !cm_hir_def_id_is_none(reason_cleanup_definition)
        && !cm_hir_def_id_equal(reason_abi_value->target,
            reason_cleanup_definition));
    saved_variant_definition = reason_abi_value->target;
    reason_abi_value->target = reason_cleanup_definition;
    ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
    reason_abi_value->target = saved_variant_definition;

    for (index = 0u;
            index < sizeof(rejected_sources) / sizeof(rejected_sources[0]);
            ++index) {
        CaptureFixture rejected;
        char source[2048];
        int written = snprintf(source, sizeof(source), "%s"
            "pub trait Gate<T: ?Sized> {}\n"
            "pub fn needs<X: Gate<u8>>() {}\n", rejected_sources[index]);
        assert(written > 0 && (size_t)written < sizeof(source));
        fixture_init_source(&rejected, 0, "bad-default-enum.rs",
            (const unsigned char *)source, (size_t)written);
        input = capture_input(&rejected);
        ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE();
        fixture_destroy(&rejected);
    }
    assert_default_enum_descriptor(&metadata);
#undef ASSERT_DEFAULT_ENUM_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_ascii_char_128_variant_projection(void)
{
    char source[32768];
    size_t cursor = 0u;
    uint32_t index;
    int written;
    CaptureFixture fixture;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    written = snprintf(source + cursor, sizeof(source) - cursor,
        "mod ascii_char {\n"
        "#[derive(Copy, Clone, Eq, PartialEq)]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "#[repr(u8)]\n"
        "pub enum AsciiChar {\n");
    assert(written > 0 && (size_t)written < sizeof(source) - cursor);
    cursor += (size_t)written;
    for (index = 0u; index < 128u; ++index) {
        written = snprintf(source + cursor, sizeof(source) - cursor,
            "#[unstable(feature = \"ascii_char_variants\", "
            "issue = \"110998\")] Variant%u = %u,\n",
            (unsigned int)index, (unsigned int)index);
        assert(written > 0 && (size_t)written < sizeof(source) - cursor);
        cursor += (size_t)written;
    }
    written = snprintf(source + cursor, sizeof(source) - cursor,
        "}\n}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n");
    assert(written > 0 && (size_t)written < sizeof(source) - cursor);
    cursor += (size_t)written;
    fixture_init_source(&fixture, 0, "ascii-char-128.rs",
        (const unsigned char *)source, cursor);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 132u
        && metadata.item_count == 1u
        && metadata.items[0].kind == CM_HIR_DECL_ITEM_ENUM
        && metadata.items[0].variant_count == 128u
        && metadata.items[0].variants[127].source_ordinal == 127u
        && metadata.items[0].variants[127].discriminant_low == 127u
        && declaration_string_is(metadata.items[0].variants[127].name,
            "Variant127")
        && cm_hir_declaration_metadata_validate(&metadata)
            == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void fixture_init_enum_case(CaptureFixture *fixture,
    const char *path, const char *repr_attribute,
    const char *item_stability_attribute, const char *variant_source,
    const char *extra_module_item)
{
    char source[8192];
    int written = snprintf(source, sizeof(source),
        "mod ascii_char {\n"
        "#[derive(Copy, Clone, Eq, PartialEq)]\n"
        "#[%s]\n"
        "#[%s]\n"
        "pub enum AsciiChar {\n%s}\n%s}\n"
        "#[doc(alias(\"AsciiChar\"))]\n"
        "#[unstable(feature = \"ascii_char\", issue = \"110998\")]\n"
        "pub use ascii_char::AsciiChar as Char;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        item_stability_attribute, repr_attribute, variant_source,
        extra_module_item);
    assert(written > 0 && (size_t)written < sizeof(source));
    fixture_init_source(fixture, 0, path, (const unsigned char *)source,
        (size_t)written);
}

static void assert_enum_failure_is_atomic(CaptureFixture *fixture,
    CmHirDeclarationMetadata *metadata, CmHirDeclarationItem *saved_items,
    CmHirDeclarationNamespaceEntry *saved_namespace)
{
    CmHirDeclarationCaptureInput input = capture_input(fixture);
    CmHirDeclarationCaptureResult result =
        cm_hir_declaration_metadata_capture(&input, metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata->items == saved_items
        && metadata->namespace_entries == saved_namespace);
}

static void test_enum_cfg_source_ordinal_and_atomic_negatives(void)
{
    static const char good_variants[] =
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "Null = 0,\n"
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "StartOfHeading = 1,\n";
    static const char cfg_variants[] =
        "#[cfg(any())]\n"
        "Hidden = 99,\n"
        "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
        "Null = 0,\n";
    static const struct {
        const char *path;
        const char *repr_attribute;
        const char *item_attribute;
        const char *variants;
        const char *extra;
    } rejected[] = {
        { "enum-tuple.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Tuple(u8) = 0,\n", "" },
        { "enum-named.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Named { value: u8 } = 0,\n", "" },
        { "enum-implicit.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "Implicit,\n", "" },
        { "enum-range.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[unstable(feature = \"ascii_char_variants\", issue = \"110998\")]\n"
          "TooLarge = 256,\n", "" },
        { "enum-repr.rs", "repr(u16)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          good_variants, "" },
        { "enum-item-attr.rs", "repr(u8)",
          "stable(feature = \"ascii_char\", since = \"1.0.0\")",
          good_variants, "" },
        { "enum-variant-attr.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[stable(feature = \"ascii_char_variants\", since = \"1.0.0\")]\n"
          "Null = 0,\n", "" },
        { "enum-generated-variant-attr.rs", "repr(u8)",
          "unstable(feature = \"ascii_char\", issue = \"110998\")",
          "#[cfg_attr(all(), unstable(feature = \"ascii_char_variants\", "
          "issue = \"110998\"))]\nNull = 0,\n", "" }
    };
    CaptureFixture good;
    CaptureFixture cfg;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirItemId item_id;
    CmHirItemId needs_id;
    const CmHirItem *item_const;
    const CmHirItem *needs_const;
    CmHirItem *item;
    CmHirItem *needs;
    CmHirDefId saved_definition;
    uint64_t saved_discriminant;
    CmSpan saved_span;
    CmInternId saved_metadata;
    size_t index;
    fixture_init_enum_case(&good, "enum-atomic.rs", "repr(u8)",
        "unstable(feature = \"ascii_char\", issue = \"110998\")",
        good_variants, "");
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    item_const = find_item(&good, "AsciiChar", &item_id);
    needs_const = find_item(&good, "needs", &needs_id);
    assert(item_const != NULL && item_const->kind == CM_HIR_ITEM_ENUM
        && item_const->data.enum_item.variant_count == 2u
        && needs_const != NULL);
    item = (CmHirItem *)item_const;
    needs = (CmHirItem *)needs_const;

    saved_discriminant =
        item->data.enum_item.variants[0].discriminant.data.value.low_bits;
    item->data.enum_item.variants[0].discriminant.data.value.low_bits = 2u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->data.enum_item.variants[0].discriminant.data.value.low_bits =
        saved_discriminant;

    saved_span = item->data.enum_item.variants[0].span;
    item->data.enum_item.variants[0].span.start += 1u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->data.enum_item.variants[0].span = saved_span;

    saved_metadata = item->attributes[2].metadata;
    item->attributes[2].metadata = cm_hir_intern(&good.hir, "repr(u16)");
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->attributes[2].metadata = saved_metadata;

    item->attributes[1].expansion_depth = 1u;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    item->attributes[1].expansion_depth = 0u;

    /* A forged VALUE may not borrow the enum ITEM identity. */
    saved_definition = needs->definition;
    needs->definition = item->definition;
    assert_enum_failure_is_atomic(&good, &metadata, saved_items,
        saved_namespace);
    needs->definition = saved_definition;

    for (index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
            ++index) {
        CaptureFixture bad;
        fixture_init_enum_case(&bad, rejected[index].path,
            rejected[index].repr_attribute, rejected[index].item_attribute,
            rejected[index].variants, rejected[index].extra);
        assert_enum_failure_is_atomic(&bad, &metadata, saved_items,
            saved_namespace);
        fixture_destroy(&bad);
    }

    fixture_init_enum_case(&cfg, "enum-cfg-ordinal.rs", "repr(u8)",
        "unstable(feature = \"ascii_char\", issue = \"110998\")",
        cfg_variants, "");
    item_const = find_item(&cfg, "AsciiChar", &item_id);
    /* The graph has one effective variant at raw source ordinal 1, while
     * current graph-backed lowering retains both raw variants. Capture must
     * reject that live-HIR/effective-graph census mismatch atomically. */
    assert(item_const != NULL
        && item_const->data.enum_item.variant_count == 2u);
    assert_enum_failure_is_atomic(&cfg, &metadata, saved_items,
        saved_namespace);
    fixture_destroy(&cfg);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void assert_reexport_projection_failure(CaptureFixture *fixture,
    CmHirDeclarationMetadata *metadata,
    CmHirDeclarationItem *saved_items,
    CmHirDeclarationNamespaceEntry *saved_namespace,
    uint32_t rejected_attribute)
{
    CmHirDeclarationCaptureInput input = capture_input(fixture);
    CmHirDeclarationCaptureResult result;
    CmHirImport *import = find_unique_attributed_import(fixture);
    CmSpan expected_span;
    assert(import != NULL && import->attributes != NULL
        && rejected_attribute < import->attribute_count);
    expected_span = import->attributes[rejected_attribute].span;
    result = cm_hir_declaration_metadata_capture(&input, metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && result.has_rejected_binding && result.has_rejected_target
        && result.rejected_binding_kind == CM_HIR_LIBRARY_BINDING_TYPE
        && result.rejected_ast_item_kind == CM_AST_ITEM_STRUCT
        && result.rejected_namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && result.rejected_definition.crate_id
            == fixture->lower_result.crate_id
        && result.rejected_definition.index != CM_HIR_DEF_INDEX_NONE
        && result.rejected_source_item.source == import->span.source
        && result.rejected_source_item.item == import->source_item
        && result.has_rejected_span
        && result.rejected_span.source == expected_span.source
        && result.rejected_span.start == expected_span.start
        && result.rejected_span.end == expected_span.end
        && metadata->items == saved_items
        && metadata->namespace_entries == saved_namespace);
}

static void test_reexport_alias_spelling_and_duplicate_negatives(void)
{
    static const char *const sources[] = {
        "pub struct Unit;\n"
        "#[doc(alias = \"Unit\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"not-an-identifier\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[doc(alias(\"Unit\"))]\n"
        "#[doc(alias(\"Other\"))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n",
        "pub struct Unit;\n"
        "#[unstable(feature = \"unit\", issue = \"none\")]\n"
        "#[unstable(feature = \"other\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n"
    };
    static const uint32_t rejected_attributes[] = { 0u, 0u, 0u, 1u, 1u };
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    size_t index;
    fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    for (index = 0u; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        CaptureFixture rejected;
        fixture_init_source(&rejected, 0, "bad-doc-alias.rs",
            (const unsigned char *)sources[index], strlen(sources[index]));
        assert_reexport_projection_failure(&rejected, &metadata, saved_items,
            saved_namespace, rejected_attributes[index]);
        fixture_destroy(&rejected);
    }
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

static void test_reexport_provenance_and_generated_negatives(void)
{
    static const unsigned char source[] =
        "pub struct Unit;\n"
        "#[doc(alias(\"Unit\"))]\n"
        "#[unstable(feature = \"unit\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char generated_attribute_source[] =
        "pub struct Unit;\n"
        "#[cfg_attr(all(), doc(alias(\"Unit\")))]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture fixture;
    CaptureFixture generated_attribute;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmHirImport *import;
    CmInternId saved_metadata;
    CmSpan saved_span;
    fixture_init_source(&fixture, 0, "reexport-provenance.rs", source,
        sizeof(source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&fixture);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 2u);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;
    import = find_unique_attributed_import(&fixture);
    assert(import != NULL && import->attribute_count == 2u
        && import->attributes != NULL);

    saved_metadata = import->attributes[0].metadata;
    import->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "doc(alias(\"Forged\"))");
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].metadata = saved_metadata;

    saved_span = import->attributes[0].span;
    import->attributes[0].span.start += 1u;
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 0u);
    import->attributes[0].span = saved_span;

    import->attributes[1].expansion_depth = 1u;
    assert_reexport_projection_failure(&fixture, &metadata, saved_items,
        saved_namespace, 1u);
    import->attributes[1].expansion_depth = 0u;

    fixture_init_source(&generated_attribute, 0,
        "generated-attribute-reexport.rs", generated_attribute_source,
        sizeof(generated_attribute_source) - 1u);
    assert_reexport_projection_failure(&generated_attribute, &metadata,
        saved_items, saved_namespace, 0u);
    assert(cm_hir_declaration_metadata_validate(&metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&generated_attribute);
    fixture_destroy(&fixture);
}

static void test_alias_and_reexport_attributes_fail_closed_atomically(void)
{
    static const unsigned char bad_alias_attribute_source[] =
        "pub struct Unit;\n"
        "#[repr(C)]\n"
        "pub type Alias = Unit;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char bad_reexport_attribute_source[] =
        "pub struct Unit;\n"
        "#[doc(hidden)]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char conflicting_reexport_stability_source[] =
        "pub struct Unit;\n"
        "#[stable(feature = \"unit\", since = \"1.0.0\")]\n"
        "#[unstable(feature = \"unit_next\", issue = \"none\")]\n"
        "pub use Unit as Alias;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char bad_alias_target_source[] =
        "pub type Alias = u8;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture bad_alias_attribute;
    CaptureFixture bad_reexport_attribute;
    CaptureFixture conflicting_reexport_stability;
    CaptureFixture bad_alias_target;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init(&good, 0);
    fixture_init_source(&bad_alias_attribute, 0, "bad-alias-attr.rs",
        bad_alias_attribute_source, sizeof(bad_alias_attribute_source) - 1u);
    fixture_init_source(&bad_reexport_attribute, 0,
        "bad-reexport-attr.rs", bad_reexport_attribute_source,
        sizeof(bad_reexport_attribute_source) - 1u);
    fixture_init_source(&conflicting_reexport_stability, 0,
        "conflicting-reexport-stability.rs",
        conflicting_reexport_stability_source,
        sizeof(conflicting_reexport_stability_source) - 1u);
    fixture_init_source(&bad_alias_target, 0, "bad-alias-target.rs",
        bad_alias_target_source, sizeof(bad_alias_target_source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;

    input = capture_input(&bad_alias_attribute);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);

    assert_reexport_projection_failure(&bad_reexport_attribute, &metadata,
        saved_items, saved_namespace, 0u);

    assert_reexport_projection_failure(&conflicting_reexport_stability,
        &metadata, saved_items, saved_namespace, 1u);

    input = capture_input(&bad_alias_target);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&bad_alias_target);
    fixture_destroy(&conflicting_reexport_stability);
    fixture_destroy(&bad_reexport_attribute);
    fixture_destroy(&bad_alias_attribute);
    fixture_destroy(&good);
}

static void test_constructor_omission_authority_is_not_forgeable(void)
{
    static const unsigned char omitted_source[] =
        "#[non_exhaustive]\n"
        "pub struct Omitted;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char paired_source[] =
        "#[stable(feature = \"paired\", since = \"1.0.0\")]\n"
        "pub struct Paired;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    CaptureFixture good;
    CaptureFixture omitted;
    CaptureFixture paired;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    CmHirItem *item;
    CmInternId saved_metadata;
    CmHirDeclarationItem *saved_items;
    CmHirDeclarationNamespaceEntry *saved_namespace;

    fixture_init(&good, 0);
    fixture_init_source(&omitted, 0, "omitted-constructor.rs",
        omitted_source, sizeof(omitted_source) - 1u);
    fixture_init_source(&paired, 0, "paired-constructor.rs", paired_source,
        sizeof(paired_source) - 1u);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_items = metadata.items;
    saved_namespace = metadata.namespace_entries;

    item = (CmHirItem *)find_item(&omitted, "Omitted", &item_id);
    assert(item != NULL && item->attribute_count == 1u
        && item->attributes != NULL);
    item->attribute_count = 0u;
    input = capture_input(&omitted);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item == item_id
        && result.failure_reason
            == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attribute_count = 1u;
    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&omitted.hir,
        "stable(feature = \"forged\", since = \"1.0.0\")");
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attributes[0].metadata = saved_metadata;

    item = (CmHirItem *)find_item(&paired, "Paired", &item_id);
    assert(item != NULL && item->attribute_count == 1u);
    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&paired.hir,
        "non_exhaustive");
    input = capture_input(&paired);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status != CM_HIR_DECL_CAPTURE_OK
        && metadata.items == saved_items
        && metadata.namespace_entries == saved_namespace);
    item->attributes[0].metadata = saved_metadata;
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&paired);
    fixture_destroy(&omitted);
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
        && result.namespace_count == 11u
        && result.item_count == 3u
        && result.projected_semantic_attribute_count == 11u);
    assert_exact_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void assert_const_descriptor(const CmHirDeclarationMetadata *metadata)
{
    const CmHirDeclarationNamespaceEntry *direct;
    const CmHirDeclarationNamespaceEntry *renamed;
    assert(cm_hir_declaration_metadata_validate(metadata)
        == CM_HIR_DECL_METADATA_OK);
    assert(metadata->module_count == 1u && metadata->root_module == 1u
        && metadata->trait_count == 1u && metadata->item_count == 0u
        && metadata->value_count == 4u && metadata->generic_count == 2u
        && metadata->predicate_count == 1u && metadata->type_count == 5u
        && metadata->namespace_count == 6u);
    assert(metadata->types[0].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[0].primitive == CM_HIR_DECL_PRIMITIVE_UNIT
        && metadata->types[1].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[1].primitive == CM_HIR_DECL_PRIMITIVE_CHAR
        && metadata->types[2].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[2].primitive == CM_HIR_DECL_PRIMITIVE_U8
        && metadata->types[3].kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && metadata->types[3].primitive == CM_HIR_DECL_PRIMITIVE_USIZE
        && metadata->types[4].kind == CM_HIR_DECL_TYPE_GENERIC
        && metadata->types[4].generic_local == 2u);
    assert(metadata->values[0].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[0].name, "MAX")
        && metadata->values[0].source_ordinal == 0u
        && metadata->values[0].generic_start == 0u
        && metadata->values[0].generic_count == 0u
        && metadata->values[0].predicate_start == 0u
        && metadata->values[0].predicate_count == 0u
        && metadata->values[0].parameter_count == 0u
        && metadata->values[0].parameter_types == NULL
        && metadata->values[0].return_type == 0u
        && metadata->values[0].declared_type == 2u
        && metadata->values[0].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[0].has_body == 1u);
    assert(metadata->values[1].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[1].name, "NEXT")
        && metadata->values[1].source_ordinal == 1u
        && metadata->values[1].declared_type == 4u
        && metadata->values[1].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[1].has_body == 1u
        && metadata->values[2].kind == CM_HIR_DECL_VALUE_CONST
        && declaration_string_is(metadata->values[2].name, "OLD")
        && metadata->values[2].source_ordinal == 2u
        && metadata->values[2].declared_type == 2u
        && metadata->values[2].mutability == CM_HIR_DECL_IMMUTABLE
        && metadata->values[2].has_body == 1u);
    assert(metadata->values[3].kind == CM_HIR_DECL_VALUE_FUNCTION
        && declaration_string_is(metadata->values[3].name, "needs")
        && metadata->values[3].source_ordinal == 5u
        && metadata->values[3].generic_count == 1u
        && metadata->values[3].predicate_count == 1u
        && metadata->values[3].declared_type == 0u
        && metadata->values[3].mutability == 0u
        && metadata->values[3].return_type == 1u
        && metadata->values[3].has_body == 1u);
    assert(metadata->generics[1].owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && metadata->generics[1].owner_local == 4u
        && metadata->predicates[0].owner_value == 4u
        && metadata->predicates[0].subject_type == 5u
        && metadata->predicates[0].argument_types[0] == 3u);
    direct = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "MAX");
    renamed = find_namespace_entry(metadata, 1u,
        CM_HIR_DECL_NAMESPACE_VALUE, "RENAMED");
    assert(direct != NULL && renamed != NULL
        && direct->target_kind == CM_HIR_DECL_TARGET_VALUE
        && renamed->target_kind == CM_HIR_DECL_TARGET_VALUE
        && direct->target_local == 1u && renamed->target_local == 1u
        && direct->export_ordinal == 0u && renamed->export_ordinal == 3u);
}

static void test_char_const_capture_and_determinism(void)
{
    CaptureFixture first;
    CaptureFixture noisy;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata first_metadata;
    CmHirDeclarationMetadata noisy_metadata;
    CmHirDeclarationCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf noisy_bytes;
    const_fixture_init(&first, 0);
    const_fixture_init(&noisy, 1);
    cm_hir_declaration_metadata_init(&first_metadata);
    cm_hir_declaration_metadata_init(&noisy_metadata);
    input = capture_input(&first);
    result = cm_hir_declaration_metadata_capture(&input, &first_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.value_count == 4u && result.namespace_count == 6u
        && result.semantic_attributes
            == CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION
        && result.projected_semantic_attribute_count == 4u);
    input = capture_input(&noisy);
    result = cm_hir_declaration_metadata_capture(&input, &noisy_metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK
        && result.projected_semantic_attribute_count == 4u);
    assert_const_descriptor(&first_metadata);
    assert_const_descriptor(&noisy_metadata);
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

static void test_char_const_hostile_mutations_are_atomic(void)
{
    CaptureFixture fixture;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureResult result;
    CmHirItemId item_id;
    CmHirItem *item;
    CmHirBody *body;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    CmInternId saved_metadata;
    CmHirAttribute *saved_attributes;
    uint32_t saved_source_expression;
    CmSourceId saved_source;
    CmSpan saved_body_span;
    CmHirType mismatched;
    CmHirTypeId mismatched_id;
    CmHirTypeId saved_type;
    const CmHirType *saved_type_value;
    CmHirBodyId saved_body_id;
    const_fixture_init(&fixture, 1);
    input = capture_input(&fixture);
    cm_hir_declaration_metadata_init(&metadata);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    item = (CmHirItem *)find_item(&fixture, "MAX", &item_id);
    assert(item != NULL && item->kind == CM_HIR_ITEM_CONST
        && item->attribute_count == 1u
        && item->data.value_item.body != CM_HIR_BODY_NONE);
    body = (CmHirBody *)cm_hir_get_body(&fixture.hir,
        item->data.value_item.body);
    assert(body != NULL);
    saved_body_id = item->data.value_item.body;

#define ASSERT_CONST_ATOMIC_FAILURE() do { \
    result = cm_hir_declaration_metadata_capture(&input, &metadata); \
    assert(result.status != CM_HIR_DECL_CAPTURE_OK \
        && metadata.values == saved_values \
        && metadata.namespace_entries == saved_namespace); \
} while (0)

    saved_source_expression = body->source_expression_id;
    body->source_expression_id = UINT32_MAX;
    ASSERT_CONST_ATOMIC_FAILURE();
    body->source_expression_id = saved_source_expression;

    saved_source = body->source;
    saved_body_span = body->span;
    body->source = 1u;
    body->span.source = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    body->source = saved_source;
    body->span = saved_body_span;

    saved_metadata = item->attributes[0].metadata;
    item->attributes[0].metadata = cm_hir_intern(&fixture.hir,
        "stable(feature = \"forged\", since = \"1.0.0\")");
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes[0].metadata = saved_metadata;

    item->attributes[0].expansion_depth = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes[0].expansion_depth = 0u;

    saved_attributes = item->attributes;
    item->attributes = NULL;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->attributes = saved_attributes;

    item->predicate_count = 1u;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->predicate_count = 0u;

    saved_type = item->data.value_item.type;
    saved_type_value = cm_hir_get_type(&fixture.hir, saved_type);
    assert(saved_type_value != NULL
        && saved_type_value->kind == CM_HIR_TYPE_CHAR_KIND);
    memset(&mismatched, 0, sizeof(mismatched));
    mismatched.kind = CM_HIR_TYPE_INTEGER_KIND;
    mismatched.data.integer_type.kind = CM_HIR_INT_USIZE;
    mismatched.span = saved_type_value->span;
    assert(cm_hir_add_type(&fixture.hir, &mismatched, &mismatched_id)
        == CM_HIR_OK);
    item->data.value_item.type = mismatched_id;
    body->expected_type = mismatched_id;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.type = saved_type;
    body->expected_type = saved_type;

    item->data.value_item.mutability = CM_HIR_MUTABLE;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.mutability = CM_HIR_IMMUTABLE;

    item->data.value_item.body = CM_HIR_BODY_NONE;
    ASSERT_CONST_ATOMIC_FAILURE();
    item->data.value_item.body = saved_body_id;

    assert_const_descriptor(&metadata);
#undef ASSERT_CONST_ATOMIC_FAILURE
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&fixture);
}

static void test_char_const_attributes_fail_closed_atomically(void)
{
    static const unsigned char stable_unstable[] =
        "#[stable(feature = \"one\", since = \"1.0.0\")]\n"
        "#[unstable(feature = \"two\", issue = \"none\")]\n"
        "pub const C: char = char::MAX;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    static const unsigned char unknown[] =
        "#[unknown_projection]\n"
        "pub const C: char = char::MAX;\n"
        "pub trait Gate<T: ?Sized> {}\n"
        "pub fn needs<X: Gate<u8>>() {}\n";
    const unsigned char *sources[2];
    size_t lengths[2];
    CaptureFixture good;
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationValue *saved_values;
    CmHirDeclarationNamespaceEntry *saved_namespace;
    size_t index;
    sources[0] = stable_unstable;
    sources[1] = unknown;
    lengths[0] = sizeof(stable_unstable) - 1u;
    lengths[1] = sizeof(unknown) - 1u;
    const_fixture_init(&good, 0);
    cm_hir_declaration_metadata_init(&metadata);
    input = capture_input(&good);
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    assert(result.status == CM_HIR_DECL_CAPTURE_OK);
    saved_values = metadata.values;
    saved_namespace = metadata.namespace_entries;
    for (index = 0u; index < 2u; ++index) {
        CaptureFixture bad;
        fixture_init_source(&bad, 0, "bad-const-attributes.rs",
            sources[index], lengths[index]);
        input = capture_input(&bad);
        result = cm_hir_declaration_metadata_capture(&input, &metadata);
        assert(result.status == CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR
            && result.failure_stage == CM_HIR_DECL_CAPTURE_STAGE_ITEMS
            && result.failure_reason
                == CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED
            && metadata.values == saved_values
            && metadata.namespace_entries == saved_namespace);
        fixture_destroy(&bad);
    }
    assert_const_descriptor(&metadata);
    cm_hir_declaration_metadata_destroy(&metadata);
    fixture_destroy(&good);
}

int main(void)
{
    test_default_enum_variant_capture_and_determinism();
    test_default_enum_hostile_mutations_are_atomic();
    test_char_const_capture_and_determinism();
    test_char_const_hostile_mutations_are_atomic();
    test_char_const_attributes_fail_closed_atomically();
    test_fixture_and_determinism();
    test_failure_is_atomic();
    test_zero_item_gate_path();
    test_plain_unit_struct_has_exact_empty_attribute_profile();
    test_module_attribute_projection_and_provenance();
    test_item_shape_diagnostic();
    test_non_exhaustive_authorizes_missing_constructor_mate();
    test_char_shaped_reexport_projection();
    test_ascii_char_enum_projection_and_determinism();
    test_ascii_char_128_variant_projection();
    test_enum_cfg_source_ordinal_and_atomic_negatives();
    test_reexport_alias_spelling_and_duplicate_negatives();
    test_reexport_provenance_and_generated_negatives();
    test_alias_and_reexport_attributes_fail_closed_atomically();
    test_constructor_omission_authority_is_not_forgeable();
    test_many_private_bindings_do_not_consume_public_cap();
    return 0;
}
