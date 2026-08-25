#include "cm/hir/executable_capture.h"
#include "cm/hir/lower.h"
#include "cm/hir/semantic_barrier.h"
#include "cm/source.h"

#include <assert.h>
#include <string.h>

typedef struct TestFixture {
    CmSourceSet sources;
    CmSourceId source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap modules;
    CmSemanticBarrier barrier;
    CmSemanticAdmission admission;
    CmHirArtifactConfig config;
    CmHirArtifactSourceEntry source_entry;
    const unsigned char *source_bytes;
    size_t source_length;
} TestFixture;

static void test_fixture_init(TestFixture *fixture, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmSemanticBarrierResult barrier_result;
    CmSemanticAdmissionResult admission_result;
    const CmTargetDesc *target;
    memset(fixture, 0, sizeof(*fixture));
    fixture->source_bytes = (const unsigned char *)source;
    fixture->source_length = strlen(source);
    cm_source_set_init(&fixture->sources);
    assert(cm_source_add_memory(&fixture->sources, "src/lib.rs",
        fixture->source_bytes, fixture->source_length, &fixture->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->source, &graph_options);
    assert(fixture->graph_result.error_count == 0u);
    cm_import_resolver_init(&fixture->imports);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->modules);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "shape_provider";
    lower_options.edition = CM_HIR_EDITION_2021;
    lower_result = cm_hir_lower_module_graph(&fixture->hir, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports, &fixture->modules,
        &lower_options);
    assert(lower_result.error_count == 0u && fixture->hir.crates.len == 1u);
    barrier_result = cm_semantic_barrier_init_structural(&fixture->barrier,
        &fixture->hir, 1u, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports,
        &fixture->modules);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_typed(&fixture->barrier,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->modules);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_marked(&fixture->barrier);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_regions(&fixture->barrier);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    admission_result = cm_semantic_admit_regions_local_crate(
        &fixture->admission, &fixture->barrier);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    target = cm_target_default();
    assert(target != NULL);
    fixture->cfg.environment.target_arch = target->architecture;
    fixture->cfg.environment.target_os = target->operating_system;
    fixture->cfg.environment.target_env = target->environment;
    fixture->cfg.environment.target_abi = target->abi;
    fixture->cfg.environment.target_vendor = target->vendor;
    fixture->cfg.environment.target_family = target->family;
    fixture->cfg.environment.target_pointer_width = target->pointer_bits == 32u
        ? "32" : "64";
    fixture->cfg.environment.target_endian =
        target->endian == CM_ENDIAN_LITTLE ? "little" : "big";
    fixture->cfg.environment.target_features = target->target_features;
    fixture->cfg.environment.target_feature_count =
        target->target_feature_count;
    fixture->cfg.environment.entries = target->cfg_entries;
    fixture->cfg.environment.entry_count = target->cfg_entry_count;
    cm_hir_artifact_config_init(&fixture->config);
    assert(cm_hir_artifact_config_build(target, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &fixture->cfg, &fixture->config)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    fixture->source_entry.logical_path.data = "src/lib.rs";
    fixture->source_entry.logical_path.length = sizeof("src/lib.rs") - 1u;
    fixture->source_entry.contents.data = fixture->source_bytes;
    fixture->source_entry.contents.length = fixture->source_length;
}

static void test_fixture_destroy(TestFixture *fixture)
{
    cm_hir_artifact_config_destroy(&fixture->config);
    cm_semantic_admission_destroy(&fixture->admission);
    cm_semantic_barrier_destroy(&fixture->barrier);
    cm_hir_module_map_destroy(&fixture->modules);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmHirExecutableCaptureInput test_input(TestFixture *fixture)
{
    static const unsigned char disambiguator[] = "capture-test-v1";
    static const unsigned char member[] = "provider.o";
    static const unsigned char object[] = { 0x7fu, 'E', 'L', 'F' };
    CmHirExecutableCaptureInput input;
    memset(&input, 0, sizeof(input));
    input.hir = &fixture->hir;
    input.crate_id = 1u;
    input.regions_admission = &fixture->admission;
    input.configuration = &fixture->config;
    input.crate_disambiguator.data = disambiguator;
    input.crate_disambiguator.length = sizeof(disambiguator) - 1u;
    input.source_entries = &fixture->source_entry;
    input.source_entry_count = 1u;
    input.archive_member_name.data = member;
    input.archive_member_name.length = sizeof(member) - 1u;
    input.object_bytes.data = object;
    input.object_bytes.length = sizeof(object);
    return input;
}

static void test_capture_exact_shape_and_determinism(void)
{
    static const char source[] =
        "#![no_main]\n"
        "#![feature(no_core)]\n"
        "#![no_core]\n"
        "pub trait Signal {}\n"
        "impl Signal for u32 {}\n"
        "pub fn relay<T: Signal>(prefix: u32, value: T) -> T { value }\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn exported(value: u32) -> u32 { value }\n";
    TestFixture fixture;
    CmHirExecutableCaptureInput input;
    CmHirExecutableMetadata first;
    CmHirExecutableMetadata second;
    CmHirExecutableCaptureResult result;
    CmByteBuf first_bytes;
    CmByteBuf second_bytes;
    test_fixture_init(&fixture, source);
    input = test_input(&fixture);
    cm_hir_executable_metadata_init(&first);
    cm_hir_executable_metadata_init(&second);
    result = cm_hir_executable_metadata_capture(&input, &first);
    assert(result.status == CM_HIR_EXEC_CAPTURE_OK
        && result.trait_count == 1u && result.impl_count == 1u
        && result.recipe_count == 1u
        && result.native_object_value_count == 1u);
    assert(first.value_count == 2u && first.body_count == 1u
        && first.symbol_count == 1u && first.object_count == 1u
        && first.values[0].kind == CM_HIR_EXEC_VALUE_NATIVE_OBJECT
        && first.values[1].kind == CM_HIR_EXEC_VALUE_RECIPE
        && first.bodies[0].parameter_index == 1u);
    result = cm_hir_executable_metadata_capture(&input, &second);
    assert(result.status == CM_HIR_EXEC_CAPTURE_OK);
    cm_byte_buf_init(&first_bytes);
    cm_byte_buf_init(&second_bytes);
    assert(cm_hir_executable_metadata_encode(&first, &first_bytes)
        == CM_HIR_EXEC_METADATA_OK);
    assert(cm_hir_executable_metadata_encode(&second, &second_bytes)
        == CM_HIR_EXEC_METADATA_OK);
    assert(first_bytes.len == second_bytes.len
        && memcmp(first_bytes.data, second_bytes.data, first_bytes.len) == 0);
    cm_byte_buf_destroy(&second_bytes);
    cm_byte_buf_destroy(&first_bytes);
    cm_hir_executable_metadata_destroy(&second);
    cm_hir_executable_metadata_destroy(&first);
    test_fixture_destroy(&fixture);
}

static void test_capture_rejects_extra_shape_atomically(void)
{
    static const char source[] =
        "#![feature(no_core)]\n"
        "#![no_core]\n"
        "#![no_main]\n"
        "pub trait Signal {}\n"
        "impl Signal for u32 {}\n"
        "pub fn relay<T: Signal>(value: T) -> T { value }\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn exported(value: u32) -> u32 { value }\n"
        "fn hidden(value: u32) -> u32 { value }\n";
    TestFixture fixture;
    CmHirExecutableCaptureInput input;
    CmHirExecutableMetadata output;
    CmHirExecutableCaptureResult result;
    unsigned char sentinel = 'x';
    test_fixture_init(&fixture, source);
    input = test_input(&fixture);
    cm_hir_executable_metadata_init(&output);
    output.crate_name.data = &sentinel;
    output.crate_name.length = 1u;
    result = cm_hir_executable_metadata_capture(&input, &output);
    assert(result.status == CM_HIR_EXEC_CAPTURE_UNSUPPORTED_HIR
        && result.rejected_item != CM_HIR_ITEM_NONE
        && output.crate_name.data == &sentinel
        && output.crate_name.length == 1u);
    cm_hir_executable_metadata_destroy(&output);
    test_fixture_destroy(&fixture);
}

static void test_capture_rejects_non_regions_authority(void)
{
    static const char source[] =
        "#![feature(no_core)]\n#![no_core]\n#![no_main]\n"
        "pub trait Signal {} impl Signal for u32 {}\n"
        "pub fn relay<T: Signal>(value: T) -> T { value }\n"
        "#[no_mangle] pub extern \"C\" fn exported(value:u32)->u32{value}\n";
    TestFixture fixture;
    CmHirExecutableCaptureInput input;
    CmHirExecutableMetadata output;
    CmSemanticAdmission empty;
    memset(&empty, 0, sizeof(empty));
    test_fixture_init(&fixture, source);
    input = test_input(&fixture);
    input.regions_admission = &empty;
    cm_hir_executable_metadata_init(&output);
    assert(cm_hir_executable_metadata_capture(&input, &output).status
        == CM_HIR_EXEC_CAPTURE_INVALID_AUTHORITY);
    cm_hir_executable_metadata_destroy(&output);
    test_fixture_destroy(&fixture);
}

static void test_capture_rejects_duplicate_envelope_attribute(void)
{
    static const char source[] =
        "#![feature(no_core)]\n#![no_core]\n#![no_main]\n#![no_main]\n"
        "pub trait Signal {} impl Signal for u32 {}\n"
        "pub fn relay<T: Signal>(value: T) -> T { value }\n"
        "#[no_mangle] pub extern \"C\" fn exported(value:u32)->u32{value}\n";
    TestFixture fixture;
    CmHirExecutableCaptureInput input;
    CmHirExecutableMetadata output;
    test_fixture_init(&fixture, source);
    input = test_input(&fixture);
    cm_hir_executable_metadata_init(&output);
    assert(cm_hir_executable_metadata_capture(&input, &output).status
        == CM_HIR_EXEC_CAPTURE_UNSUPPORTED_HIR);
    cm_hir_executable_metadata_destroy(&output);
    test_fixture_destroy(&fixture);
}

int main(void)
{
    unsigned int status;
    test_capture_exact_shape_and_determinism();
    test_capture_rejects_extra_shape_atomically();
    test_capture_rejects_non_regions_authority();
    test_capture_rejects_duplicate_envelope_attribute();
    for (status = 0u;
        status <= (unsigned int)CM_HIR_EXEC_CAPTURE_METADATA_FAILURE;
        ++status) {
        assert(strcmp(cm_hir_executable_capture_status_name(
            (CmHirExecutableCaptureStatus)status), "unknown") != 0);
    }
    return 0;
}
