#include "cm/hir/admission.h"
#include "cm/hir/lower.h"
#include "cm/mir/lower.h"
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

static const CmHirItem *find_impl_method(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *stored_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        stored_name = item == NULL ? NULL
            : cm_interner_get(&hir->strings, item->name);
        if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && !cm_hir_def_id_is_none(item->parent_definition)
            && !cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            && stored_name != NULL && stored_name->len == length
            && memcmp(stored_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
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

static void test_concrete_impl_method_is_admitted(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmHirItem *method;
    const CmHirBody *body;

    fixture_init(&f,
        "fn plus_one(x: u32) -> u32 { x + 1u32 } "
        "trait Value { fn value(x: u32) -> u32; } "
        "impl Value for u32 { "
        "    fn value(x: u32) -> u32 { plus_one(x) } "
        "} "
        "pub fn main() -> u32 { 0u32 }");
    method = find_impl_method(&f.hir, "value");
    assert(method != NULL
        && cm_hir_body_function_owner_kind(&f.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD);
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    body = cm_hir_get_body(&f.hir, method->data.function_item.body);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_OK
        && result.item_result.status == CM_SEMANTIC_ITEM_OK
        && result.body_result.status == CM_SEMANTIC_BODY_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_generic_impl_method_is_rejected_atomically(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmHirItem *method;
    size_t expressions;
    size_t types;

    fixture_init(&f,
        "trait Value { fn value() -> u32; } "
        "struct Wrap<T> { value: T } "
        "impl<T> Value for Wrap<T> { fn value() -> u32 { 1u32 } } "
        "pub fn main() -> u32 { 0u32 }");
    method = find_impl_method(&f.hir, "value");
    assert(method != NULL
        && cm_hir_body_function_owner_kind(&f.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    expressions = f.hir.expressions.len;
    types = f.hir.types.len;
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE
        && result.local_bodies.status
            == CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER
        && cm_hir_def_id_equal(result.owner, method->definition)
        && result.body == method->data.function_item.body
        && admission.state == NULL
        && f.hir.expressions.len == expressions
        && f.hir.types.len == types
        && cm_hir_get_body(&f.hir,
            method->data.function_item.body)->state
                == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&f);
}

static void test_impl_method_body_failure_is_atomic(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmHirItem *method;
    size_t expressions;
    size_t types;

    fixture_init(&f,
        "trait Missing {} "
        "fn bounded<T: Missing>(x: T) -> T { x } "
        "trait Value { fn value(x: u32) -> u32; } "
        "impl Value for u32 { "
        "    fn value(x: u32) -> u32 { bounded::<u32>(x) } "
        "} "
        "pub fn main() -> u32 { 0u32 }");
    method = find_impl_method(&f.hir, "value");
    assert(method != NULL);
    expressions = f.hir.expressions.len;
    types = f.hir.types.len;
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_BODY_FAILURE
        && cm_hir_def_id_equal(result.owner, method->definition)
        && result.body == method->data.function_item.body
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_OK
        && result.item_result.status == CM_SEMANTIC_ITEM_OK
        && result.session_status == CM_TRAIT_SOLVER_PROVEN
        && result.body_result.status != CM_SEMANTIC_BODY_OK
        && admission.state == NULL
        && f.hir.expressions.len == expressions
        && f.hir.types.len == types
        && cm_hir_get_body(&f.hir,
            method->data.function_item.body)->state
                == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&f);
}

static void test_mir_admission_gates(void)
{
    Fixture f, foreign;
    CmSemanticAdmission admission, foreign_admission, missing;
    CmSemanticAdmissionResult admission_result;
    CmMirContext mir, copied, rejected;
    CmMirLowerResult lower_result;
    const CmMirBody *stored;
    CmMirBody candidate;
    CmMirBodyId copied_id;
    CmHirType type;
    CmHirTypeId type_id;
    size_t body_count;

    fixture_init(&f,
        "fn first(x: u32) -> u32 { x } "
        "fn second(x: u32) -> u32 { x }");
    fixture_init(&foreign, "fn foreign(x: u32) -> u32 { x }");
    memset(&admission, 0, sizeof(admission));
    memset(&foreign_admission, 0, sizeof(foreign_admission));
    memset(&missing, 0, sizeof(missing));
    assert(admit(&f, &admission).status == CM_SEMANTIC_ADMISSION_OK);
    assert(admit(&foreign, &foreign_admission).status
        == CM_SEMANTIC_ADMISSION_OK);
    cm_mir_context_init(&mir);
    cm_mir_context_init(&copied);
    cm_mir_context_init(&rejected);

    lower_result = cm_mir_lower_admitted_instance(&mir, &missing, 1u,
        NULL, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && lower_result.first_error.mir_status == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    lower_result = cm_mir_lower_admitted_instance(NULL, &admission, 1u,
        NULL, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ARGUMENT);

    lower_result = cm_mir_lower_admitted_instance(&mir, &admission, 1u,
        NULL, 0u);
    assert(lower_result.error_count == 0u);
    assert(lower_result.lowered_body_count == 1u);
    assert(cm_mir_body_count(&mir) == 1u);
    assert(mir.hir_owner == &f.hir && mir.admitted_crate == 1u);
    assert(mir.admitted_storage_lifetime_id == f.hir.storage.lifetime_id);
    assert(mir.admitted_semantic_generation == f.hir.semantic_generation);
    assert(mir.admitted_rewind_generation == f.hir.rewind_generation);
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        lower_result.body) == CM_MIR_OK);

    stored = cm_mir_get_body(&mir, lower_result.body);
    assert(stored != NULL);
    candidate = *stored;
    candidate.owned_storage = NULL;
    assert(cm_mir_add_admitted_monomorphized_body(&copied, &admission,
        &candidate, &copied_id) == CM_MIR_OK
        && copied_id == 1u && cm_mir_body_count(&copied) == 1u);

    candidate.basic_blocks = NULL;
    copied_id = 99u;
    assert(cm_mir_add_admitted_monomorphized_body(&rejected, &admission,
        &candidate, &copied_id) == CM_MIR_INVARIANT_VIOLATION
        && copied_id == CM_MIR_BODY_NONE
        && cm_mir_body_count(&rejected) == 0u
        && rejected.admitted_crate == CM_HIR_CRATE_NONE);
    candidate = *stored;
    candidate.owned_storage = NULL;
    candidate.owner.crate_id = 2u;
    assert(cm_mir_add_admitted_monomorphized_body(&rejected, &admission,
        &candidate, &copied_id) == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&rejected) == 0u);

    body_count = cm_mir_body_count(&mir);
    lower_result = cm_mir_lower_admitted_instance(&mir,
        &foreign_admission, 1u, NULL, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == body_count);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ f.source, 0u, 1u };
    assert(cm_hir_add_type(&f.hir, &type, &type_id) == CM_HIR_OK);
    assert(!cm_semantic_admission_is_current(&admission));
    lower_result = cm_mir_lower_admitted_instance(&mir, &admission, 2u,
        NULL, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == body_count
        && cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
            1u) == CM_MIR_INVALID_ADMISSION);

    cm_mir_context_destroy(&rejected);
    cm_mir_context_destroy(&copied);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&foreign_admission);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&foreign);
    fixture_destroy(&f);

    fixture_init(&f,
        "trait Missing {} "
        "fn bounded<T: Missing>(x: T) -> T { x } "
        "fn bad(x: u32) -> u32 { bounded::<u32>(x) }");
    memset(&admission, 0, sizeof(admission));
    admission_result = admit(&f, &admission);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_BODY_FAILURE);
    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_instance(&mir, &admission, 2u,
        NULL, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_context_destroy(&mir);
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
    test_concrete_impl_method_is_admitted();
    test_generic_impl_method_is_rejected_atomically();
    test_impl_method_body_failure_is_atomic();
    test_mir_admission_gates();
    test_invalid_api();
    puts("hir semantic admission tests passed");
    return 0;
}
