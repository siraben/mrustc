#include "cm/hir/admission.h"
#include "cm/hir/lower.h"
#include "cm/source.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    CmSourceSet sources;
    CmSourceId source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap modules;
} Fixture;

static void fixture_init(Fixture *f, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    memset(f, 0, sizeof(*f));
    cm_source_set_init(&f->sources);
    assert(cm_source_add_memory(&f->sources, "admission/lib.rs",
        (const unsigned char *)source, strlen(source), &f->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&f->cfg);
    cm_module_graph_init(&f->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &f->cfg;
    f->graph_result = cm_module_graph_build(&f->graph, &f->sources,
        f->source, &graph_options);
    assert(f->graph_result.error_count == 0u);
    cm_import_resolver_init(&f->imports);
    import_result = cm_import_resolve(&f->imports, &f->graph,
        f->graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&f->hir);
    cm_hir_module_map_init(&f->modules);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "admission_test";
    lower_options.edition = CM_HIR_EDITION_2021;
    lower_result = cm_hir_lower_module_graph(&f->hir, &f->graph,
        f->graph_result.revision, &f->imports, &f->modules, &lower_options);
    assert(lower_result.error_count == 0u && f->hir.crates.len == 1u);
}

static void fixture_destroy(Fixture *f)
{
    cm_hir_module_map_destroy(&f->modules);
    cm_hir_context_destroy(&f->hir);
    cm_import_resolver_destroy(&f->imports);
    cm_module_graph_destroy(&f->graph);
    cm_source_set_destroy(&f->sources);
}

static CmSemanticAdmissionResult admit(Fixture *f,
    CmSemanticAdmission *admission)
{
    return cm_semantic_admit_local_crate(admission, &f->hir, 1u,
        &f->graph, f->graph_result.revision, &f->imports, &f->modules);
}

static void test_success_and_stale(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmHirType type;
    CmHirTypeId type_id;
    fixture_init(&f, "pub fn first() -> i32 { 7 } fn second() -> i32 { 9 }");
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_OK
        && result.item_result.status == CM_SEMANTIC_ITEM_OK
        && result.body_result.status == CM_SEMANTIC_BODY_OK
        && result.session_status == CM_TRAIT_SOLVER_PROVEN
        && cm_semantic_admission_is_current(&admission)
        && cm_semantic_admission_hir(&admission) == &f.hir
        && cm_semantic_admission_crate(&admission) == 1u
        && cm_semantic_admission_generation(&admission)
            == f.hir.semantic_generation);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ f.source, 0u, 1u };
    assert(cm_hir_add_type(&f.hir, &type, &type_id) == CM_HIR_OK);
    assert(!cm_semantic_admission_is_current(&admission)
        && cm_semantic_admission_hir(&admission) == NULL
        && cm_semantic_admission_generation(&admission) == 0u);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_body_failure_rolls_back(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    size_t expressions, types;
    uint64_t generation;
    fixture_init(&f, "fn first() -> i32 { 7 } fn bad() -> i32 { true }");
    expressions = f.hir.expressions.len;
    types = f.hir.types.len;
    generation = f.hir.semantic_generation;
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_BODY_FAILURE
        && admission.state == NULL && f.hir.expressions.len == expressions
        && f.hir.types.len == types
        && f.hir.semantic_generation > generation);
    assert(cm_hir_get_body(&f.hir, 1u)->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&f.hir, 2u)->state == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&f);
}

static void test_semantic_failure_rolls_back(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    size_t expressions, types;

    fixture_init(&f,
        "trait Missing {} "
        "fn bounded<T: Missing>(x: T) -> T { x } "
        "fn bad(x: u32) -> u32 { bounded::<u32>(x) }");
    expressions = f.hir.expressions.len;
    types = f.hir.types.len;
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_BODY_FAILURE
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_OK
        && result.item_result.status == CM_SEMANTIC_ITEM_OK
        && result.session_status == CM_TRAIT_SOLVER_PROVEN
        && result.body_result.status != CM_SEMANTIC_BODY_OK
        && admission.state == NULL
        && f.hir.expressions.len == expressions
        && f.hir.types.len == types
        && cm_hir_get_body(&f.hir, 1u)->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&f.hir, 2u)->state == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&f);
}

static void test_invalid_api(void)
{
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    unsigned int status;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_local_crate(&admission, NULL, 1u, NULL,
        CM_MODULE_GRAPH_REVISION_NONE, NULL, NULL);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    for (status = 0u; status <= (unsigned int)CM_SEMANTIC_ADMISSION_HIR_FAILURE;
         ++status)
        assert(strcmp(cm_semantic_admission_status_name(
            (CmSemanticAdmissionStatus)status), "unknown") != 0);
}

int main(void)
{
    test_success_and_stale();
    test_body_failure_rolls_back();
    test_semantic_failure_rolls_back();
    test_invalid_api();
    puts("hir semantic admission tests passed");
    return 0;
}
