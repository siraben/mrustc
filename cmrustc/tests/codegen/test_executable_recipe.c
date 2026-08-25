#define _POSIX_C_SOURCE 200809L

#include "cm/codegen/executable_recipe.h"
#include "cm/hir/executable_materialize.h"
#include "cm/hir/lower.h"
#include "cm/hir/semantic_barrier.h"
#include "cm/sha256.h"
#include "cm/source.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestMetadata {
    CmHirExecutableMetadata metadata;
    CmHirArtifactSourceEntry sources[2];
    CmHirExecutableModule modules[1];
    CmHirExecutableTrait traits[1];
    CmHirExecutableType types[2];
    CmHirExecutableImpl impls[1];
    CmHirExecutableValue values[2];
    uint32_t recipe_parameters[1];
    uint32_t native_parameters[1];
    CmHirExecutablePredicate predicates[1];
    CmHirExecutableNamespaceEntry entries[3];
    CmHirExecutableBody bodies[1];
    CmHirExecutableLinkObject objects[1];
    CmHirExecutableLinkSymbol symbols[1];
} TestMetadata;

typedef struct TestProgram {
    TestMetadata wire;
    CmHirContext hir;
    CmHirLibraryArtifact artifact;
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmHirModuleMap modules;
    CmSemanticBarrier barrier;
    CmSemanticAdmission admission;
    CmHirCrateId local_crate;
    const CmHirItem *wrapper;
    const CmHirItem *recipe;
    CmHirItem *recipe_impl;
    CmHirExpr *call;
} TestProgram;

static const CmTargetDesc test_target = {
    "x86_64-unknown-linux-gnu", "x86_64", "linux", "gnu", "", "unknown",
    "unix", 64u, CM_ENDIAN_LITTLE, NULL, 0u, NULL, 0u
};

static void test_hash(const void *bytes, size_t length,
    CmHirArtifactDigest *digest)
{
    CmSha256 sha;
    cm_sha256_init(&sha);
    cm_sha256_update(&sha, bytes, length);
    cm_sha256_final(&sha, digest->bytes);
}

static void test_metadata_init(TestMetadata *fixture)
{
    static const unsigned char native[] =
        "unsigned object_probe(unsigned x){return x+1;}\n";
    static const unsigned char source[] =
        "pub trait Present {} impl Present for u32 {} "
        "pub fn bounded<T: Present>(x:T)->T{x}\n";
    CmHirExecutableMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirExecutableString)S("g3_provider");
    metadata->crate_disambiguator = (CmHirExecutableString)S("recipe-test");
    metadata->edition = UINT32_C(2021);
    metadata->target_descriptor = (CmHirExecutableString)S(
        "x86_64-unknown-linux-gnu;e-m:e-p:64:64");
    metadata->panic_strategy = (CmHirExecutableString)S("abort");
    fixture->sources[0].logical_path.data = "native/object.c";
    fixture->sources[0].logical_path.length = sizeof("native/object.c") - 1u;
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

    fixture->recipe_parameters[0] = 2u;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirExecutableString)S("bounded");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].kind = CM_HIR_EXEC_VALUE_RECIPE;
    fixture->values[0].generic_name = (CmHirExecutableString)S("T");
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->recipe_parameters;
    fixture->values[0].return_type = 2u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].execution_local = 1u;
    fixture->native_parameters[0] = 1u;
    fixture->values[1].owner_module = 1u;
    fixture->values[1].name = (CmHirExecutableString)S("object_probe");
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
    fixture->bodies[0].parameter_index = 0u;
    fixture->bodies[0].parameter_type = 2u;
    fixture->bodies[0].return_type = 2u;
    metadata->bodies = fixture->bodies;
    metadata->body_count = 1u;
    fixture->objects[0].archive_member_name =
        (CmHirExecutableString)S("provider.o");
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
        (CmHirExecutableString)S("provider_object_probe");
    metadata->symbols = fixture->symbols;
    metadata->symbol_count = 1u;
    assert(cm_hir_executable_metadata_compute_identity(metadata,
        &metadata->link_manifest_digest, &metadata->artifact_identity)
        == CM_HIR_EXEC_METADATA_OK);
}

static int test_name_is(const CmHirContext *hir, CmInternId name,
    const char *expected)
{
    const CmInternedString *text = cm_interner_get(&hir->strings, name);
    size_t length = strlen(expected);
    return text != NULL && text->len == length
        && memcmp(text->bytes, expected, length) == 0;
}

static void test_find_structures(TestProgram *program)
{
    size_t index;

    for (index = 0u; index < program->hir.items.len; ++index) {
        CmHirItem *item = (CmHirItem *)cm_vec_at(&program->hir.items, index);
        if (item->kind == CM_HIR_ITEM_FUNCTION
            && item->definition.crate_id == program->local_crate
            && test_name_is(&program->hir, item->name, "consumer_probe")) {
            const CmHirBody *body;
            const CmHirExpr *block;
            program->wrapper = item;
            body = cm_hir_get_body(&program->hir,
                item->data.function_item.body);
            block = cm_hir_get_expr(&program->hir, body->root_expression);
            program->call = (CmHirExpr *)cm_hir_get_expr(&program->hir,
                block->data.block.tail_expression);
        } else if (item->kind == CM_HIR_ITEM_FUNCTION
            && item->definition.crate_id != program->local_crate
            && test_name_is(&program->hir, item->name, "bounded")) {
            program->recipe = item;
        } else if (item->kind == CM_HIR_ITEM_IMPL
            && item->definition.crate_id != program->local_crate) {
            program->recipe_impl = item;
        }
    }
    assert(program->wrapper != NULL && program->recipe != NULL
        && program->recipe_impl != NULL && program->call != NULL);
}

static void test_program_init(TestProgram *program)
{
    static const unsigned char source[] =
        "#![feature(no_core)]\n#![no_core]\n#![no_main]\n"
        "use provider::bounded;\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn consumer_probe(x:u32)->u32 {"
        "bounded::<u32>(x)}\n";
    const CmHirLibraryArtifact *libraries[1];
    CmHirExecutableMaterializeResult materialized;
    CmSourceId root_source;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lowered;
    CmSemanticBarrierResult barrier_result;
    CmSemanticAdmissionResult admission_result;

    memset(program, 0, sizeof(*program));
    test_metadata_init(&program->wire);
    cm_hir_context_init(&program->hir);
    cm_hir_library_artifact_init(&program->artifact);
    cm_source_set_init(&program->sources);
    cm_module_graph_init(&program->graph);
    cm_cfg_set_init(&program->cfg);
    cm_import_resolver_init(&program->imports);
    cm_hir_module_map_init(&program->modules);
    materialized = cm_hir_executable_metadata_materialize(&program->hir,
        &program->artifact, &program->wire.metadata, "provider", 77u);
    assert(materialized.status == CM_HIR_EXEC_MATERIALIZE_OK);
    assert(cm_source_add_memory(&program->sources, "consumer.rs", source,
        sizeof(source) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &program->cfg;
    graph_result = cm_module_graph_build(&program->graph, &program->sources,
        root_source, &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&program->imports, &program->graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "g3_consumer";
    libraries[0] = &program->artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lowered = cm_hir_lower_module_graph(&program->hir, &program->graph,
        graph_result.revision, &program->imports, &program->modules,
        &lower_options);
    assert(lowered.error_count == 0u);
    program->local_crate = lowered.crate_id;
    barrier_result = cm_semantic_barrier_init_structural(&program->barrier,
        &program->hir, program->local_crate, &program->graph,
        graph_result.revision, &program->imports, &program->modules);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_typed(&program->barrier,
        &program->graph, graph_result.revision, &program->imports,
        &program->modules);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_marked(&program->barrier);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    barrier_result = cm_semantic_barrier_advance_regions(&program->barrier);
    assert(barrier_result.status == CM_SEMANTIC_BARRIER_OK);
    admission_result = cm_semantic_admit_regions_local_crate(
        &program->admission, &program->barrier);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    test_find_structures(program);
}

static void test_program_destroy(TestProgram *program)
{
    cm_semantic_admission_destroy(&program->admission);
    cm_semantic_barrier_destroy(&program->barrier);
    cm_hir_module_map_destroy(&program->modules);
    cm_import_resolver_destroy(&program->imports);
    cm_module_graph_destroy(&program->graph);
    cm_source_set_destroy(&program->sources);
    cm_hir_library_artifact_destroy(&program->artifact);
    cm_hir_context_destroy(&program->hir);
}

static void test_reject_unchanged(TestProgram *program, CmStrBuf *output)
{
    CmExecutableRecipeEmitStatus status;
    size_t before = output->len;
    status = cm_c_emit_executable_recipe_program(&program->hir,
        &program->admission, program->local_crate, &test_target, output);
    assert(status != CM_EXECUTABLE_RECIPE_EMIT_OK && output->len == before
        && strcmp(cm_str_buf_c_str(output), "sentinel") == 0);
}

static void test_negative_mutations(TestProgram *program)
{
    CmStrBuf output;
    CmHirDefId saved_callee;
    CmHirImplPolarity saved_polarity;
    CmHirBody *recipe_body;
    unsigned char saved_identity[CM_HIR_ARTIFACT_IDENTITY_SIZE];

    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "sentinel");
    saved_callee = program->call->data.call.callee;
    program->call->data.call.callee = cm_hir_def_id_none();
    test_reject_unchanged(program, &output);
    program->call->data.call.callee = saved_callee;

    saved_polarity = program->recipe_impl->data.impl_item.polarity;
    program->recipe_impl->data.impl_item.polarity = CM_HIR_IMPL_NEGATIVE;
    test_reject_unchanged(program, &output);
    program->recipe_impl->data.impl_item.polarity = saved_polarity;

    recipe_body = (CmHirBody *)cm_hir_get_body(&program->hir,
        program->recipe->data.function_item.body);
    memcpy(saved_identity,
        recipe_body->origin.data.metadata_recipe.artifact_identity,
        sizeof(saved_identity));
    memset(recipe_body->origin.data.metadata_recipe.artifact_identity, 0,
        sizeof(saved_identity));
    test_reject_unchanged(program, &output);
    memcpy(recipe_body->origin.data.metadata_recipe.artifact_identity,
        saved_identity, sizeof(saved_identity));
    cm_str_buf_destroy(&output);
}

static void test_compile_and_run(const char *compiler, const char *flags,
    const CmStrBuf *source)
{
    char directory[] = "/tmp/cmrustc-recipe-XXXXXX";
    char source_path[256];
    char harness_path[256];
    char executable_path[256];
    char command[1024];
    FILE *file;
    int length;

    assert(mkdtemp(directory) != NULL);
    length = snprintf(source_path, sizeof(source_path), "%s/recipe.c",
        directory);
    assert(length > 0 && (size_t)length < sizeof(source_path));
    length = snprintf(harness_path, sizeof(harness_path), "%s/harness.c",
        directory);
    assert(length > 0 && (size_t)length < sizeof(harness_path));
    length = snprintf(executable_path, sizeof(executable_path), "%s/probe",
        directory);
    assert(length > 0 && (size_t)length < sizeof(executable_path));
    file = fopen(source_path, "wb");
    assert(file != NULL
        && fwrite(source->data, 1u, source->len, file) == source->len
        && fclose(file) == 0);
    file = fopen(harness_path, "wb");
    assert(file != NULL
        && fputs("#include <stdint.h>\n"
            "extern uint32_t consumer_probe(uint32_t);\n"
            "int main(void){return consumer_probe(41u)==41u?0:1;}\n",
            file) >= 0
        && fclose(file) == 0);
    length = snprintf(command, sizeof(command),
        "%s %s -o %s %s %s && %s", compiler, flags, executable_path,
        source_path, harness_path, executable_path);
    assert(length > 0 && (size_t)length < sizeof(command)
        && system(command) == 0);
    assert(unlink(executable_path) == 0);
    assert(unlink(harness_path) == 0);
    assert(unlink(source_path) == 0);
    assert(rmdir(directory) == 0);
}

static void test_emit_runtime_and_stale(void)
{
    TestProgram program;
    CmStrBuf output;
    CmStrBuf rejected;
    CmExecutableRecipeEmitStatus status;
    CmHirType type;
    CmHirTypeId type_id;

    test_program_init(&program);
    test_negative_mutations(&program);
    cm_str_buf_init(&output);
    status = cm_c_emit_executable_recipe_program(&program.hir,
        &program.admission, program.local_crate, &test_target, &output);
    assert(status == CM_EXECUTABLE_RECIPE_EMIT_OK
        && strstr(cm_str_buf_c_str(&output),
            "uint32_t consumer_probe(uint32_t value)") != NULL
        && strstr(cm_str_buf_c_str(&output),
            "return cmrustc_g3_recipe_0(value);") != NULL);
    test_compile_and_run("gcc",
        "-std=c99 -pedantic-errors -Wall -Wextra -Werror", &output);
    test_compile_and_run("tcc", "-std=c99 -Wall -Werror", &output);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span.source = 91u;
    type.span.start = 1u;
    type.span.end = 2u;
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&program.hir, &type, &type_id) == CM_HIR_OK
        && !cm_semantic_admission_is_current(&program.admission));
    cm_str_buf_init(&rejected);
    cm_str_buf_append(&rejected, "sentinel");
    test_reject_unchanged(&program, &rejected);
    cm_str_buf_destroy(&rejected);
    cm_str_buf_destroy(&output);
    test_program_destroy(&program);
}

int main(void)
{
    test_emit_runtime_and_stale();
    return 0;
}
