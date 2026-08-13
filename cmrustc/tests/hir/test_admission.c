#include "cm/hir/admission.h"
#include "cm/hir/lower.h"
#include "cm/hir/instance.h"
#include "cm/hir/semantic_results.h"
#include "cm/mir/lower.h"
#include "cm/source.h"
#include "cm/alloc.h"

#include "../../src/hir/admission_internal.h"
#include "../../src/hir/instance_internal.h"
#include "../../src/hir/semantic_results_internal.h"

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

static CmSemanticBarrierResult init_barrier(Fixture *f,
    CmSemanticBarrier *barrier)
{
    return cm_semantic_barrier_init_structural(barrier, &f->hir, 1u,
        &f->graph, f->graph_result.revision, &f->imports, &f->modules);
}

static void advance_barrier_to_regions(Fixture *f,
    CmSemanticBarrier *barrier)
{
    assert(cm_semantic_barrier_advance_typed(barrier, &f->graph,
            f->graph_result.revision, &f->imports, &f->modules).status
        == CM_SEMANTIC_BARRIER_OK);
    assert(cm_semantic_barrier_advance_marked(barrier).status
        == CM_SEMANTIC_BARRIER_OK);
    assert(cm_semantic_barrier_advance_regions(barrier).status
        == CM_SEMANTIC_BARRIER_OK);
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

static const CmHirItem *find_free_function(const CmHirContext *hir,
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
            && cm_hir_def_id_is_none(item->parent_definition)
            && stored_name != NULL && stored_name->len == length
            && memcmp(stored_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_trait_default(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirDefinition *parent_record;
        const CmHirItem *parent;
        const CmInternedString *stored_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        parent_record = item == NULL
                || item->kind != CM_HIR_ITEM_FUNCTION
                || cm_hir_def_id_is_none(item->parent_definition)
            ? NULL : cm_hir_lookup_definition(hir,
                item->parent_definition);
        parent = parent_record == NULL
                || parent_record->kind != CM_HIR_DEFINITION_ITEM
                || parent_record->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir,
                parent_record->entity.item_id);
        stored_name = item == NULL ? NULL
            : cm_interner_get(&hir->strings, item->name);
        if (parent != NULL && parent->kind == CM_HIR_ITEM_TRAIT
            && item->data.function_item.body != CM_HIR_BODY_NONE
            && stored_name != NULL && stored_name->len == length
            && memcmp(stored_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static void lower_function(Fixture *f, const CmHirItem *function)
{
    CmHirBodyLowerResult result;

    assert(function != NULL
        && function->data.function_item.body != CM_HIR_BODY_NONE);
    result = cm_hir_lower_body(&f->hir,
        function->data.function_item.body, &f->graph,
        f->graph_result.revision, &f->imports, &f->modules);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
}

static CmHirExprId find_body_call(const CmHirContext *hir, CmHirBodyId body)
{
    size_t index;

    for (index = 0u; index < hir->expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(hir, (CmHirExprId)(index + 1u));
        if (expression != NULL && expression->owner_body == body
            && expression->kind == CM_HIR_EXPR_CALL) {
            return (CmHirExprId)(index + 1u);
        }
    }
    return CM_HIR_EXPR_NONE;
}

static CmHirExprId find_body_qualified_call(const CmHirContext *hir,
    CmHirBodyId body)
{
    size_t index;

    for (index = 0u; index < hir->expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(hir, (CmHirExprId)(index + 1u));
        if (expression != NULL && expression->owner_body == body
            && expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
            return (CmHirExprId)(index + 1u);
        }
    }
    return CM_HIR_EXPR_NONE;
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
        && cm_semantic_admission_results(&admission) == NULL
        && f.hir.expressions.len == expressions
        && f.hir.types.len == types
        && cm_hir_get_body(&f.hir, 1u)->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&f.hir, 2u)->state == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&f);
}

static void test_closed_trait_default_is_admitted_but_not_executable(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticBodyView body_view;
    const CmSemanticResults *results;
    const CmHirItem *method;
    const CmHirBody *body;
    CmMirContext mir;
    CmMirLowerResult lower_result;

    fixture_init(&f,
        "fn first() -> u32 { 0u32 } "
        "trait Closed { fn bump(value: u32) -> u32 { value + 1u32 } } "
        "const VALUE: u32 = 2u32;");
    method = find_trait_default(&f.hir, "bump");
    assert(method != NULL
        && cm_hir_body_function_owner_kind(&f.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT);
    memset(&admission, 0, sizeof(admission));
    result = admit(&f, &admission);
    results = cm_semantic_admission_results(&admission);
    body = cm_hir_get_body(&f.hir,
        method->data.function_item.body);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && result.local_bodies.status == CM_HIR_LOCAL_BODIES_OK
        && result.body_result.status == CM_SEMANTIC_BODY_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && results != NULL
        && cm_semantic_results_body_count(results, &admission) == 2u
        && cm_semantic_results_body(results, &admission,
            method->data.function_item.body, &body_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(body_view.owner, method->definition));
    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        method->data.function_item.body);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind
            == CM_MIR_LOWER_INVALID_ADMISSION
        && lower_result.first_error.mir_status == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);
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

static void test_generic_impl_method_definition_is_admitted(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    const CmHirItem *method;
    const CmHirBody *body;

    fixture_init(&f,
        "trait Value { fn value() -> u32; } "
        "struct Wrap<T> { value: T } "
        "impl<T> Value for Wrap<T> { fn value() -> u32 { 1u32 } } "
        "pub fn main() -> u32 { 0u32 }");
    method = find_impl_method(&f.hir, "value");
    assert(method != NULL
        && cm_hir_body_function_owner_kind(&f.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD);
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
    const CmMirBody *caller_body;
    const CmHirBody *caller_hir_body;
    CmHirExpr *caller_expression;
    size_t expression_index;
    CmMirBody candidate;
    CmMirBodyId copied_id;
    CmHirDefId first_definition;
    CmHirType type;
    CmHirTypeId type_id;
    CmSemanticFunctionSignatureView signature_view;
    CmSemanticTypeView parameter_view;
    size_t body_count;

    fixture_init(&f,
        "fn first(x: u32) -> u32 { x } "
        "fn second(x: u32) -> u32 { first(x) }");
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

    lower_result = cm_mir_lower_admitted_body(&mir, &missing, 1u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && lower_result.first_error.mir_status == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    lower_result = cm_mir_lower_admitted_body(NULL, &admission, 1u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ARGUMENT);

    lower_result = cm_mir_lower_admitted_body(&mir, &admission, 1u);
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
    first_definition = stored->instance.definition;
    memset(&signature_view, 0xA5, sizeof(signature_view));
    memset(&parameter_view, 0xA5, sizeof(parameter_view));
    assert(cm_mir_admitted_signature(&mir, &admission, lower_result.body,
            &signature_view) == CM_MIR_OK
        && cm_hir_def_id_equal(signature_view.definition, first_definition)
        && signature_view.body == stored->source_body
        && signature_view.parameter_count == 1u
        && signature_view.return_type.bytes != NULL
        && signature_view.return_type.size != 0u
        && cm_mir_admitted_signature_parameter(&mir, &admission,
            lower_result.body, 0u, &parameter_view) == CM_MIR_OK
        && parameter_view.bytes != NULL && parameter_view.size != 0u);
    memset(&signature_view, 0xA5, sizeof(signature_view));
    assert(cm_mir_admitted_signature(&mir, &missing, lower_result.body,
            &signature_view) == CM_MIR_INVALID_ADMISSION
        && cm_hir_def_id_is_none(signature_view.definition)
        && signature_view.body == CM_HIR_BODY_NONE
        && signature_view.parameter_count == 0u
        && signature_view.return_type.bytes == NULL
        && signature_view.return_type.size == 0u);
    memset(&parameter_view, 0xA5, sizeof(parameter_view));
    assert(cm_mir_admitted_signature_parameter(&mir, &foreign_admission,
            lower_result.body, 0u, &parameter_view)
            == CM_MIR_INVALID_ADMISSION
        && parameter_view.bytes == NULL && parameter_view.size == 0u);
    memset(&signature_view, 0xA5, sizeof(signature_view));
    assert(cm_mir_admitted_signature(&mir, &admission, 99u,
            &signature_view) == CM_MIR_INVALID_ID
        && cm_hir_def_id_is_none(signature_view.definition)
        && signature_view.return_type.bytes == NULL
        && signature_view.return_type.size == 0u);
    memset(&parameter_view, 0xA5, sizeof(parameter_view));
    assert(cm_mir_admitted_signature_parameter(&mir, &admission,
            lower_result.body, 1u, &parameter_view)
            == CM_MIR_INVALID_ADMISSION
        && parameter_view.bytes == NULL && parameter_view.size == 0u);

    lower_result = cm_mir_lower_admitted_body(&mir, &admission, 2u);
    assert(lower_result.error_count == 0u
        && lower_result.lowered_body_count == 1u
        && cm_mir_body_count(&mir) == 2u);
    caller_body = cm_mir_get_body(&mir, lower_result.body);
    assert(caller_body != NULL && caller_body->basic_block_count >= 2u
        && caller_body->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && cm_hir_def_id_equal(
            caller_body->basic_blocks[0].terminator.data.call.callee
                .definition,
            first_definition));

    caller_hir_body = cm_hir_get_body(&f.hir, 2u);
    caller_expression = NULL;
    for (expression_index = 0u;
         caller_hir_body != NULL && expression_index < f.hir.expressions.len;
         ++expression_index) {
        CmHirExpr *candidate_expression;

        candidate_expression = (CmHirExpr *)cm_hir_get_expr(&f.hir,
            (CmHirExprId)(expression_index + 1u));
        if (candidate_expression != NULL
            && candidate_expression->owner_body == 2u
            && candidate_expression->kind == CM_HIR_EXPR_CALL) {
            caller_expression = candidate_expression;
            break;
        }
    }
    assert(caller_expression != NULL
        && caller_expression->kind == CM_HIR_EXPR_CALL);
    caller_expression->data.call.callee.index += 100u;
    body_count = cm_mir_body_count(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission, 2u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == body_count
        && cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
            2u) == CM_MIR_INVALID_ADMISSION);
    caller_expression->data.call.callee.index -= 100u;
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        2u) == CM_MIR_OK);

    stored = cm_mir_get_body(&mir, 1u);
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
    lower_result = cm_mir_lower_admitted_body(&mir,
        &foreign_admission, 1u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == body_count);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ f.source, 0u, 1u };
    assert(cm_hir_add_type(&f.hir, &type, &type_id) == CM_HIR_OK);
    assert(!cm_semantic_admission_is_current(&admission));
    memset(&signature_view, 0xA5, sizeof(signature_view));
    memset(&parameter_view, 0xA5, sizeof(parameter_view));
    assert(cm_mir_admitted_signature(&mir, &admission, 1u,
            &signature_view) == CM_MIR_INVALID_ADMISSION
        && cm_hir_def_id_is_none(signature_view.definition)
        && signature_view.return_type.bytes == NULL
        && signature_view.return_type.size == 0u
        && cm_mir_admitted_signature_parameter(&mir, &admission, 1u, 0u,
            &parameter_view) == CM_MIR_INVALID_ADMISSION
        && parameter_view.bytes == NULL && parameter_view.size == 0u);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission, 2u);
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
    lower_result = cm_mir_lower_admitted_body(&mir, &admission, 2u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_context_destroy(&mir);
    fixture_destroy(&f);
}

static void test_reachable_admission_subset_and_mir(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableBody reachable;
    CmSemanticBodyView body_view;
    CmMirContext mir;
    CmMirLowerResult lower_result;
    const CmHirItem *chosen;
    const CmHirItem *unsupported;
    const CmSemanticResults *results;
    CmHirType type;
    CmHirTypeId type_id;

    fixture_init(&f,
        "fn chosen(x: u32) -> u32 { x + 1u32 } "
        "fn unsupported(x: u32) -> u32 { x * 1u32 }");
    chosen = find_free_function(&f.hir, "chosen");
    unsupported = find_free_function(&f.hir, "unsupported");
    assert(chosen != NULL && unsupported != NULL);
    lower_function(&f, chosen);
    assert(cm_hir_get_body(&f.hir,
            unsupported->data.function_item.body)->state
        == CM_HIR_BODY_UNLOWERED);

    reachable.owner = chosen->definition;
    reachable.body = chosen->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, &reachable, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_is_current(&admission)
        && results != NULL
        && cm_semantic_results_body_count(results, &admission) == 1u
        && cm_semantic_results_body(results, &admission, reachable.body,
            &body_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_body(results, &admission,
            unsupported->data.function_item.body, &body_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_hir_get_body(&f.hir,
            unsupported->data.function_item.body)->state
            == CM_HIR_BODY_UNLOWERED);

    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        reachable.body);
    assert(lower_result.error_count == 0u
        && lower_result.body != CM_MIR_BODY_NONE
        && cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
            lower_result.body) == CM_MIR_OK);
    cm_mir_context_destroy(&mir);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ f.source, 0u, 1u };
    assert(cm_hir_add_type(&f.hir, &type, &type_id) == CM_HIR_OK
        && !cm_semantic_admission_is_current(&admission));
    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        reachable.body);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_reachable_admission_requires_closed_unique_set(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableBody reachable[2];
    CmSemanticDirectCallView call_view;
    CmSemanticFunctionSignatureView signature_view;
    CmMirContext mir;
    CmMirLowerResult lower_result;
    const CmHirItem *callee;
    const CmHirItem *caller;
    const CmSemanticResults *results;

    fixture_init(&f,
        "fn callee(x: u32) -> u32 { x + 1u32 } "
        "fn caller(x: u32) -> u32 { callee(x) }");
    callee = find_free_function(&f.hir, "callee");
    caller = find_free_function(&f.hir, "caller");
    assert(callee != NULL && caller != NULL);
    lower_function(&f, callee);
    lower_function(&f, caller);
    reachable[0].owner = caller->definition;
    reachable[0].body = caller->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_HIR_FAILURE
        && admission.state == NULL
        && cm_semantic_admission_results(&admission) == NULL);

    reachable[1].owner = callee->definition;
    reachable[1].body = callee->data.function_item.body;
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, reachable, 2u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && results != NULL
        && cm_semantic_results_body_count(results, &admission) == 2u
        && cm_semantic_results_direct_call(results, &admission,
            caller->data.function_item.body,
            find_body_call(&f.hir, caller->data.function_item.body),
            &call_view) == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(call_view.callee, callee->definition)
        && cm_semantic_results_signature(results, &admission,
            callee->data.function_item.body,
            &signature_view) == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(signature_view.definition,
            callee->definition));
    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        callee->data.function_item.body);
    assert(lower_result.error_count == 0u);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        caller->data.function_item.body);
    assert(lower_result.error_count == 0u
        && cm_mir_body_count(&mir) == 2u);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);

    reachable[1] = reachable[0];
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, reachable, 2u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    reachable[0].owner = callee->definition;
    reachable[0].body = caller->data.function_item.body;
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    fixture_destroy(&f);
}

static void test_reachable_admission_rejects_untyped_and_generic(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableBody reachable;
    const CmHirItem *generic;
    const CmHirItem *plain;

    fixture_init(&f,
        "fn plain(x: u32) -> u32 { x } "
        "fn generic<T>(x: T) -> T { x }");
    plain = find_free_function(&f.hir, "plain");
    generic = find_free_function(&f.hir, "generic");
    assert(plain != NULL && generic != NULL);
    memset(&admission, 0, sizeof(admission));
    reachable.owner = plain->definition;
    reachable.body = plain->data.function_item.body;
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);

    lower_function(&f, generic);
    reachable.owner = generic->definition;
    reachable.body = generic->data.function_item.body;
    result = cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    fixture_destroy(&f);
}

static void test_leaf_instance_admission_is_exact(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirInstanceSpec missing_spec;
    CmHirGenericArg argument;
    CmHirGenericArg missing_argument;
    CmSemanticAdmissionResult result;
    const CmSemanticResults *results;
    CmSemanticBodyView body_view;
    CmSemanticFunctionSignatureView signature;
    CmSemanticExpressionView expression;
    CmSemanticTypeView parameter;
    CmHirInstanceKey key;
    const CmHirItem *generic;
    const CmHirItem *plain;
    const CmHirBody *body;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    size_t index;
    int equal;

    fixture_init(&f,
        "fn generic<T>(x: T) -> T { x } "
        "fn plain(x: u32) -> u32 { x } "
        "fn flag(x: bool) -> bool { x }");
    generic = find_free_function(&f.hir, "generic");
    plain = find_free_function(&f.hir, "plain");
    assert(generic != NULL && plain != NULL);
    lower_function(&f, generic);
    u32_type = CM_HIR_TYPE_NONE;
    bool_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U32) {
            u32_type = (CmHirTypeId)(index + 1u);
        } else if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            bool_type = (CmHirTypeId)(index + 1u);
        }
    }
    assert(u32_type != CM_HIR_TYPE_NONE && bool_type != CM_HIR_TYPE_NONE);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u32_type;
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = generic->definition;
    spec.body_definition = generic->definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    reachable.body = generic->data.function_item.body;
    reachable.spec = &spec;
    memset(&admission, 0, sizeof(admission));
    memset(&key, 0, sizeof(key));
    result = cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u);
    results = cm_semantic_admission_results(&admission);
    body = cm_hir_get_body(&f.hir, reachable.body);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && results != NULL && body != NULL
        && cm_semantic_results_body_count(results, &admission) == 0u
        && cm_semantic_results_instance_body(results, &admission, &spec,
            &body_view) == CM_SEMANTIC_RESULTS_OK
        && body_view.body == reachable.body
        && cm_hir_def_id_equal(body_view.owner, generic->definition)
        && cm_semantic_results_instance_signature(results, &admission,
            &spec, &signature) == CM_SEMANTIC_RESULTS_OK
        && signature.parameter_count == 1u
        && cm_semantic_results_instance_signature_parameter(results,
            &admission, &spec, 0u, &parameter)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&signature.return_type, &parameter,
            &equal) == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, body->root_expression, &expression)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&expression.adjusted_type, &parameter,
            &equal) == CM_SEMANTIC_RESULTS_OK && equal
        && cm_hir_instance_key_init(&key, &admission, &spec)
            == CM_HIR_INSTANCE_OK);
    cm_hir_instance_key_destroy(&key);
    missing_argument = argument;
    missing_argument.data.type = bool_type;
    missing_spec = spec;
    missing_spec.item_arguments = &missing_argument;
    assert(cm_semantic_results_instance_body(results, &admission,
            &missing_spec, &body_view) == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_hir_instance_key_init(&key, &admission, &missing_spec)
            == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    cm_semantic_admission_destroy(&admission);

    reachable.body = plain->data.function_item.body;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    fixture_destroy(&f);
}

static void test_generic_impl_method_instances_are_exact(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableInstance reachable[2];
    CmHirInstanceSpec specs[2];
    CmHirInstanceSpec wrong_spec;
    CmHirGenericArg impl_arguments[2];
    CmHirGenericArg wrong_argument;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    const CmHirItem *method;
    const CmHirDefinition *impl_record;
    CmHirItem *mutable_method;
    CmHirItem *impl_item;
    CmHirBody *body;
    const CmSemanticResults *results;
    CmSemanticBodyView u32_body;
    CmSemanticBodyView u8_body;
    CmSemanticFunctionSignatureView u32_signature;
    CmSemanticFunctionSignatureView u8_signature;
    CmSemanticTypeView u32_parameter;
    CmSemanticTypeView u8_parameter;
    size_t index;
    int equal;

    fixture_init(&f,
        "trait Echo { fn echo(value: Self) -> Self; } "
        "impl Echo for u32 { fn echo(value: u32) -> u32 { value } } "
        "fn byte(value: u8) -> u8 { value }");
    method = find_impl_method(&f.hir, "echo");
    assert(method != NULL);
    lower_function(&f, method);
    impl_record = cm_hir_lookup_definition(&f.hir,
        method->parent_definition);
    impl_item = impl_record == NULL ? NULL : (CmHirItem *)cm_hir_get_item(
        &f.hir, impl_record->entity.item_id);
    mutable_method = (CmHirItem *)method;
    body = (CmHirBody *)cm_hir_get_body(&f.hir,
        method->data.function_item.body);
    u32_type = CM_HIR_TYPE_NONE;
    u8_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND) {
            if (type->data.integer_type.kind == CM_HIR_INT_U32) {
                u32_type = (CmHirTypeId)(index + 1u);
            } else if (type->data.integer_type.kind == CM_HIR_INT_U8) {
                u8_type = (CmHirTypeId)(index + 1u);
            }
        }
    }
    assert(impl_item != NULL && impl_item->kind == CM_HIR_ITEM_IMPL
        && mutable_method != NULL && body != NULL
        && u32_type != CM_HIR_TYPE_NONE && u8_type != CM_HIR_TYPE_NONE);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = impl_item->definition;
    parameter.name = cm_hir_intern(&f.hir, "T");
    parameter.span = impl_item->span;
    assert(cm_hir_add_generic_param(&f.hir, &parameter, &parameter_id)
            == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = impl_item->span;
    parameter_type_value.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&f.hir, &parameter_type_value,
            &parameter_type) == CM_HIR_OK);
    impl_item->generic_parameter_start = parameter_id;
    impl_item->generic_parameter_count = 1u;
    impl_item->data.impl_item.self_type = parameter_type;
    mutable_method->data.function_item.signature.parameters[0].type =
        parameter_type;
    mutable_method->data.function_item.signature.return_type = parameter_type;
    body->expected_type = parameter_type;
    body->locals[0].type = parameter_type;
    for (index = 0u; index < f.hir.expressions.len; ++index) {
        CmHirExpr *expression;

        expression = (CmHirExpr *)cm_vec_at(&f.hir.expressions, index);
        if (expression != NULL
            && expression->owner_body == method->data.function_item.body) {
            expression->type = parameter_type;
        }
    }
    memset(impl_arguments, 0, sizeof(impl_arguments));
    impl_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    impl_arguments[0].data.type = u32_type;
    impl_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    impl_arguments[1].data.type = u8_type;
    for (index = 0u; index < 2u; ++index) {
        cm_hir_instance_spec_init(&specs[index]);
        specs[index].selected_callable = method->definition;
        specs[index].body_definition = method->definition;
        specs[index].declared_trait_callable =
            method->data.function_item.trait_item_definition;
        specs[index].enclosing_impl = impl_item->definition;
        specs[index].enclosing_impl_arguments = &impl_arguments[index];
        specs[index].enclosing_impl_argument_count = 1u;
        specs[index].implemented_trait =
            impl_item->data.impl_item.trait_type.definition;
        specs[index].self_owner = impl_item->definition;
        specs[index].self_type = index == 0u ? u32_type : u8_type;
        reachable[index].body = method->data.function_item.body;
        reachable[index].spec = &specs[index];
    }
    memset(&admission, 0, sizeof(admission));
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 2u).status == CM_SEMANTIC_ADMISSION_OK);
    results = cm_semantic_admission_results(&admission);
    assert(results != NULL
        && cm_semantic_results_instance_body(results, &admission,
            &specs[0], &u32_body) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_body(results, &admission,
            &specs[1], &u8_body) == CM_SEMANTIC_RESULTS_OK
        && u32_body.body == u8_body.body
        && cm_semantic_results_instance_signature(results, &admission,
            &specs[0], &u32_signature) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_signature(results, &admission,
            &specs[1], &u8_signature) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_signature_parameter(results,
            &admission, &specs[0], 0u, &u32_parameter)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_signature_parameter(results,
            &admission, &specs[1], 0u, &u8_parameter)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&u32_signature.return_type,
            &u32_parameter, &equal) == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_type_view_equal(&u8_signature.return_type,
            &u8_parameter, &equal) == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_type_view_equal(&u32_parameter, &u8_parameter,
            &equal) == CM_SEMANTIC_RESULTS_OK && !equal);
    cm_semantic_admission_destroy(&admission);

    wrong_spec = specs[0];
    wrong_spec.enclosing_impl_arguments = NULL;
    wrong_spec.enclosing_impl_argument_count = 0u;
    reachable[0].spec = &wrong_spec;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    wrong_spec = specs[0];
    wrong_spec.self_type = u8_type;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    memset(&wrong_argument, 0, sizeof(wrong_argument));
    wrong_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    wrong_argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    wrong_spec = specs[0];
    wrong_spec.enclosing_impl_arguments = &wrong_argument;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    wrong_argument = impl_arguments[0];
    wrong_spec = specs[0];
    wrong_spec.method_arguments = &wrong_argument;
    wrong_spec.method_argument_count = 1u;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    wrong_spec = specs[0];
    wrong_spec.item_arguments = &wrong_argument;
    wrong_spec.item_argument_count = 1u;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    wrong_spec = specs[0];
    wrong_spec.implemented_trait_arguments = &wrong_argument;
    wrong_spec.implemented_trait_argument_count = 1u;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    wrong_spec = specs[0];
    wrong_spec.enclosing_impl = cm_hir_def_id_none();
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    reachable[0].spec = &specs[0];
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
            1u, reachable, 1u).status == CM_SEMANTIC_ADMISSION_OK);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_leaf_instance_recipes_are_exact(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirInstanceSpec wrong_spec;
    CmSemanticAdmissionResult result;
    const CmSemanticResults *results;
    const CmHirItem *recipe;
    const CmHirItem *other;
    const CmHirBody *body;
    const CmHirExpr *binary_expression;
    const CmHirExpr *field_expression;
    CmHirExprId binary;
    CmHirExprId field;
    CmSemanticPrimitiveBinaryView binary_view;
    CmSemanticFieldSelectionView field_view;
    CmSemanticExpressionView expression_view;
    CmSemanticExpressionView left_view;
    CmSemanticExpressionView right_view;
    CmSemanticExpressionView base_view;
    CmSemanticAdjustmentView adjustment_view;
    size_t index;
    int equal;

    fixture_init(&f,
        "struct Pair { left: u32, right: u32 } "
        "fn recipe(value: Pair) -> u32 { value.left + value.right } "
        "fn other(value: u32) -> u32 { value }");
    recipe = find_free_function(&f.hir, "recipe");
    other = find_free_function(&f.hir, "other");
    assert(recipe != NULL && other != NULL);
    lower_function(&f, recipe);
    body = cm_hir_get_body(&f.hir, recipe->data.function_item.body);
    assert(body != NULL);
    binary = CM_HIR_EXPR_NONE;
    field = CM_HIR_EXPR_NONE;
    for (index = 0u; index < f.hir.expressions.len; ++index) {
        const CmHirExpr *expression;

        expression = cm_hir_get_expr(&f.hir,
            (CmHirExprId)(index + 1u));
        if (expression == NULL || expression->owner_body
                != recipe->data.function_item.body) {
            continue;
        }
        if (expression->kind == CM_HIR_EXPR_BINARY) {
            assert(binary == CM_HIR_EXPR_NONE);
            binary = (CmHirExprId)(index + 1u);
        } else if (expression->kind == CM_HIR_EXPR_FIELD
            && field == CM_HIR_EXPR_NONE) {
            field = (CmHirExprId)(index + 1u);
        }
    }
    binary_expression = cm_hir_get_expr(&f.hir, binary);
    field_expression = cm_hir_get_expr(&f.hir, field);
    assert(binary_expression != NULL && field_expression != NULL
        && binary_expression->kind == CM_HIR_EXPR_BINARY
        && field_expression->kind == CM_HIR_EXPR_FIELD);

    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = recipe->definition;
    spec.body_definition = recipe->definition;
    cm_hir_instance_spec_init(&wrong_spec);
    wrong_spec.selected_callable = other->definition;
    wrong_spec.body_definition = other->definition;
    reachable.body = recipe->data.function_item.body;
    reachable.spec = &spec;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_primitive_binary(results,
            &admission, &spec, binary, &binary_view)
            == CM_SEMANTIC_RESULTS_OK
        && binary_view.body == recipe->data.function_item.body
        && binary_view.expression == binary
        && binary_view.operator_kind
            == binary_expression->data.binary.operator_kind
        && binary_view.left_expression
            == binary_expression->data.binary.left
        && binary_view.right_expression
            == binary_expression->data.binary.right
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, binary, &expression_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, binary_view.left_expression, &left_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, binary_view.right_expression, &right_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&binary_view.left_type,
            &left_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_type_view_equal(&binary_view.right_type,
            &right_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_type_view_equal(&binary_view.result_type,
            &expression_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_instance_expression_adjustment(results,
            &admission, &spec, binary, 0u, &adjustment_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_instance_primitive_binary(results,
            &admission, &wrong_spec, binary, &binary_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND);

    assert(cm_semantic_results_instance_field_selection(results,
            &admission, &spec, field, &field_view)
            == CM_SEMANTIC_RESULTS_OK
        && field_view.body == recipe->data.function_item.body
        && field_view.expression == field
        && field_view.base_expression == field_expression->data.field.base
        && cm_hir_def_id_equal(field_view.aggregate_definition,
            field_expression->data.field.definition)
        && field_view.field_index == field_expression->data.field.field_index
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, field, &expression_view) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_expression(results, &admission,
            &spec, field_view.base_expression, &base_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&field_view.base_type,
            &base_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_type_view_equal(&field_view.field_type,
            &expression_view.adjusted_type, &equal)
            == CM_SEMANTIC_RESULTS_OK && equal
        && cm_semantic_results_instance_expression_adjustment(results,
            &admission, &spec, field, 0u, &adjustment_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_semantic_results_instance_field_selection(results,
            &admission, &wrong_spec, field, &field_view)
            == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_leaf_instance_admitted_mir_is_exact(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirGenericArg argument;
    CmMirContext mir;
    CmMirContext copied;
    CmMirLowerResult lower_result;
    CmMirBody candidate;
    CmMirBodyId copied_id;
    const CmMirBody *stored;
    const CmHirItem *generic;
    const CmHirDefinition *definition;
    CmHirItem *mutable_generic;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    size_t index;

    fixture_init(&f,
        "fn generic<T>(x: T) -> T { x } "
        "fn number(x: u32) -> u32 { x } fn flag() -> bool { true }");
    generic = find_free_function(&f.hir, "generic");
    assert(generic != NULL);
    lower_function(&f, generic);
    u32_type = CM_HIR_TYPE_NONE;
    bool_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U32) {
            u32_type = (CmHirTypeId)(index + 1u);
        } else if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            bool_type = (CmHirTypeId)(index + 1u);
        }
    }
    assert(u32_type != CM_HIR_TYPE_NONE && bool_type != CM_HIR_TYPE_NONE);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u32_type;
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = generic->definition;
    spec.body_definition = generic->definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    reachable.body = generic->data.function_item.body;
    reachable.spec = &spec;
    memset(&admission, 0, sizeof(admission));
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u).status == CM_SEMANTIC_ADMISSION_OK);

    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_instance(&mir, &admission,
        reachable.body, &u32_type, 1u);
    assert(lower_result.error_count == 0u && lower_result.body == 1u
        && cm_mir_body_count(&mir) == 1u
        && cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
            lower_result.body) == CM_MIR_OK);
    stored = cm_mir_get_body(&mir, lower_result.body);
    assert(stored != NULL && stored->instance.substitution_count == 1u
        && stored->instance.substitutions[0] == u32_type
        && stored->local_count == 2u
        && stored->locals[0].type == u32_type
        && stored->locals[1].type == u32_type);

    definition = cm_hir_lookup_definition(&f.hir, generic->definition);
    assert(definition != NULL && definition->kind == CM_HIR_DEFINITION_ITEM);
    mutable_generic = (CmHirItem *)cm_vec_at(&f.hir.items,
        (size_t)definition->entity.item_id - 1u);
    assert(mutable_generic != NULL);
    mutable_generic->data.function_item.signature.return_type = bool_type;
    mutable_generic->data.function_item.signature.parameters[0].type =
        bool_type;
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        lower_result.body) == CM_MIR_OK);
    candidate = *stored;
    candidate.owned_storage = NULL;
    cm_mir_context_init(&copied);
    assert(cm_mir_add_admitted_monomorphized_body(&copied, &admission,
        &candidate, &copied_id) == CM_MIR_OK && copied_id == 1u
        && cm_mir_validate_admitted_monomorphized_body(&copied, &admission,
            copied_id) == CM_MIR_OK);
    cm_mir_context_destroy(&copied);

    lower_result = cm_mir_lower_admitted_instance(&mir, &admission,
        reachable.body, NULL, 1u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ARGUMENT
        && cm_mir_body_count(&mir) == 1u);
    lower_result = cm_mir_lower_admitted_instance(&mir, &admission,
        reachable.body, &u32_type, 0u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ARGUMENT
        && cm_mir_body_count(&mir) == 1u);

    lower_result = cm_mir_lower_admitted_instance(&mir, &admission,
        reachable.body, &bool_type, 1u);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 1u);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_leaf_instance_admission_rejects_calls_and_bounds(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirGenericArg argument;
    const CmHirItem *calls;
    const CmHirItem *bounded;
    CmHirTypeId u32_type;
    size_t index;

    fixture_init(&f,
        "trait Missing {} "
        "fn helper(x: u32) -> u32 { x } "
        "fn calls(x: u32) -> u32 { helper(x) } "
        "fn bounded<T: Missing>(x: T) -> T { x } "
        "fn concrete(x: u32) -> u32 { x }");
    calls = find_free_function(&f.hir, "calls");
    bounded = find_free_function(&f.hir, "bounded");
    assert(calls != NULL && bounded != NULL);
    lower_function(&f, calls);
    lower_function(&f, bounded);
    u32_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U32) {
            u32_type = (CmHirTypeId)(index + 1u);
            break;
        }
    }
    assert(u32_type != CM_HIR_TYPE_NONE);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u32_type;
    cm_hir_instance_spec_init(&spec);
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    reachable.spec = &spec;
    memset(&admission, 0, sizeof(admission));

    spec.selected_callable = calls->definition;
    spec.body_definition = calls->definition;
    spec.item_arguments = NULL;
    spec.item_argument_count = 0u;
    reachable.body = calls->data.function_item.body;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u).status == CM_SEMANTIC_ADMISSION_HIR_FAILURE
        && admission.state == NULL);

    spec.selected_callable = bounded->definition;
    spec.body_definition = bounded->definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    reachable.body = bounded->data.function_item.body;
    assert(cm_semantic_admit_typed_leaf_instances(&admission, &f.hir,
        1u, &reachable, 1u).status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    fixture_destroy(&f);
}

static void test_exact_instance_closure_authenticates_generic_calls(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable[2];
    CmSemanticReachableInstanceCall edge;
    CmSemanticReachableInstanceCall duplicate_edges[2];
    CmHirInstanceSpec caller_spec;
    CmHirInstanceSpec callee_spec;
    CmHirInstanceSpec wrong_callee_spec;
    CmHirGenericArg caller_argument;
    CmHirGenericArg callee_argument;
    CmHirGenericArg wrong_argument;
    const CmHirItem *caller;
    const CmHirItem *phantom;
    const CmSemanticResults *results;
    CmSemanticDirectCallView call;
    CmSemanticTypeView parameter;
    CmHirCanonicalInstance caller_identity;
    CmHirCanonicalInstance callee_identity;
    CmHirCanonicalInstance retained_identity;
    CmHirCanonicalInstance rejected_identity;
    CmHirExprId call_expression;
    CmHirTypeId u32_type;
    CmHirTypeId bool_type;
    size_t index;

    fixture_init(&f,
        "fn phantom<T>(x: T) -> T { x } "
        "fn caller(x: u32) -> u32 { phantom::<u32>(x) } "
        "fn bool_value(x: bool) -> bool { x }");
    caller = find_free_function(&f.hir, "caller");
    phantom = find_free_function(&f.hir, "phantom");
    assert(caller != NULL && phantom != NULL);
    lower_function(&f, phantom);
    lower_function(&f, caller);
    call_expression = find_body_call(&f.hir,
        caller->data.function_item.body);
    assert(call_expression != CM_HIR_EXPR_NONE);
    u32_type = CM_HIR_TYPE_NONE;
    bool_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
            && type->data.integer_type.kind == CM_HIR_INT_U32) {
            u32_type = (CmHirTypeId)(index + 1u);
        } else if (type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND) {
            bool_type = (CmHirTypeId)(index + 1u);
        }
    }
    assert(u32_type != CM_HIR_TYPE_NONE && bool_type != CM_HIR_TYPE_NONE);
    memset(&caller_argument, 0, sizeof(caller_argument));
    memset(&callee_argument, 0, sizeof(callee_argument));
    memset(&wrong_argument, 0, sizeof(wrong_argument));
    caller_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    caller_argument.data.type = u32_type;
    callee_argument = caller_argument;
    wrong_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    wrong_argument.data.type = bool_type;
    cm_hir_instance_spec_init(&caller_spec);
    caller_spec.selected_callable = caller->definition;
    caller_spec.body_definition = caller->definition;
    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = phantom->definition;
    callee_spec.body_definition = phantom->definition;
    callee_spec.item_arguments = &callee_argument;
    callee_spec.item_argument_count = 1u;
    wrong_callee_spec = callee_spec;
    wrong_callee_spec.item_arguments = &wrong_argument;
    reachable[0].body = caller->data.function_item.body;
    reachable[0].spec = &caller_spec;
    reachable[1].body = phantom->data.function_item.body;
    reachable[1].spec = &callee_spec;
    edge.caller = &caller_spec;
    edge.expression = call_expression;
    edge.callee = &callee_spec;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, &edge, 1u);
    results = cm_semantic_admission_results(&admission);
    cm_hir_canonical_instance_init(&caller_identity);
    cm_hir_canonical_instance_init(&callee_identity);
    cm_hir_canonical_instance_init(&retained_identity);
    cm_hir_canonical_instance_init(&rejected_identity);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_direct_call(results, &admission,
            &caller_spec, call_expression, &callee_spec, &call)
            == CM_SEMANTIC_RESULTS_OK
        && call.expression == call_expression
        && call.parameter_count == 1u
        && cm_hir_def_id_equal(call.callee, phantom->definition)
        && cm_semantic_results_instance_direct_call_parameter(results,
            &admission, &caller_spec, call_expression, &callee_spec, 0u,
            &parameter) == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_direct_call(results, &admission,
            &caller_spec, call_expression, &wrong_callee_spec, &call)
            == CM_SEMANTIC_RESULTS_NOT_FOUND
        && cm_hir_canonical_instance_encode(&f.hir, 1u, &caller_spec,
            &caller_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode(&f.hir, 1u, &callee_spec,
            &callee_identity) == CM_HIR_INSTANCE_OK
        && cm_semantic_results_canonical_instance_callee_identity(results,
            &admission, &caller_identity, call_expression,
            &retained_identity) == CM_SEMANTIC_RESULTS_OK);
    {
        int equal;

        equal = 0;
        assert(cm_hir_canonical_instance_equal(&retained_identity,
            &callee_identity, &equal) == CM_HIR_INSTANCE_OK && equal);
    }
    caller_identity.bytes[caller_identity.size - 1u] ^= 1u;
    assert(cm_semantic_results_canonical_instance_callee_identity(results,
        &admission, &caller_identity, call_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
    caller_identity.bytes[caller_identity.size - 1u] ^= 1u;
    caller_identity.body = callee_identity.body;
    assert(cm_semantic_results_canonical_instance_callee_identity(results,
        &admission, &caller_identity, call_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
    caller_identity.body = reachable[0].body;
    caller_identity.definition = callee_identity.definition;
    assert(cm_semantic_results_canonical_instance_callee_identity(results,
        &admission, &caller_identity, call_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
    caller_identity.definition = caller_spec.selected_callable;
    caller_identity.size -= 1u;
    assert(cm_semantic_results_canonical_instance_callee_identity(results,
        &admission, &caller_identity, call_expression, &rejected_identity)
            == CM_SEMANTIC_RESULTS_INVALID_ARGUMENT);
    caller_identity.size += 1u;
    assert(cm_semantic_results_canonical_instance_callee_identity(results,
        &admission, &caller_identity, CM_HIR_EXPR_NONE, &rejected_identity)
            == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_hir_canonical_instance_destroy(&rejected_identity);
    cm_hir_canonical_instance_destroy(&retained_identity);
    cm_hir_canonical_instance_destroy(&callee_identity);
    cm_hir_canonical_instance_destroy(&caller_identity);
    cm_semantic_admission_destroy(&admission);

    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, NULL, 0u).status
            == CM_SEMANTIC_ADMISSION_HIR_FAILURE
        && admission.state == NULL);
    duplicate_edges[0] = edge;
    duplicate_edges[1] = edge;
    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, duplicate_edges, 2u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 1u, &edge, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);

    reachable[1].spec = &wrong_callee_spec;
    edge.callee = &wrong_callee_spec;
    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, &edge, 1u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    fixture_destroy(&f);
}

typedef struct CanonicalAdmissionSnapshot {
    size_t expression_count;
    size_t type_count;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
} CanonicalAdmissionSnapshot;

static CanonicalAdmissionSnapshot canonical_admission_snapshot(
    const CmHirContext *hir)
{
    CanonicalAdmissionSnapshot snapshot;

    snapshot.expression_count = hir->expressions.len;
    snapshot.type_count = hir->types.len;
    snapshot.semantic_generation = hir->semantic_generation;
    snapshot.rewind_generation = hir->rewind_generation;
    return snapshot;
}

static void assert_canonical_admission_rejected_unchanged(
    CmSemanticAdmission *admission, CmHirContext *hir,
    const CmSemanticCanonicalReachableInstance *instances,
    size_t instance_count,
    const CmSemanticCanonicalReachableInstanceCall *calls,
    size_t call_count, CanonicalAdmissionSnapshot snapshot,
    CmSemanticAdmissionStatus expected_status)
{
    CmSemanticAdmissionResult result;

    result = cm_semantic_admit_typed_canonical_instance_closure(admission,
        hir, 1u, instances, instance_count, calls, call_count);
    assert(result.status == expected_status
        && admission->state == NULL
        && cm_semantic_admission_results(admission) == NULL
        && hir->expressions.len == snapshot.expression_count
        && hir->types.len == snapshot.type_count
        && hir->semantic_generation == snapshot.semantic_generation
        && hir->rewind_generation == snapshot.rewind_generation);
}

static void test_canonical_instance_closure_is_exact_and_atomic(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance flat_instances[2];
    CmSemanticReachableInstanceCall flat_call;
    CmSemanticCanonicalReachableInstance canonical_instances[2];
    CmSemanticCanonicalReachableInstance malformed_instances[2];
    CmSemanticCanonicalReachableInstance duplicate_instances[2];
    CmSemanticCanonicalReachableInstanceCall canonical_call;
    CmSemanticCanonicalReachableInstanceCall duplicate_calls[2];
    CmHirInstanceSpec caller_spec;
    CmHirInstanceSpec callee_spec;
    CmHirInstanceSpec wrong_callee_spec;
    CmHirGenericArg callee_argument;
    CmHirGenericArg wrong_argument;
    CmHirCanonicalInstance caller_identity;
    CmHirCanonicalInstance callee_identity;
    CmHirCanonicalInstance wrong_callee_identity;
    CmHirCanonicalInstance flat_retained_identity;
    CmHirCanonicalInstance canonical_retained_identity;
    CmHirCanonicalInstance truncated_identity;
    CmHirCanonicalInstance trailing_identity;
    const CmHirItem *caller;
    const CmHirItem *phantom;
    const CmSemanticResults *results;
    CanonicalAdmissionSnapshot snapshot;
    CmHirExprId call_expression;
    CmHirTypeId u32_type;
    CmHirTypeId i32_type;
    unsigned char *trailing_bytes;
    size_t index;
    int equal;

    fixture_init(&f,
        "fn phantom<T>(x: T) -> T { x } "
        "fn caller(x: u32) -> u32 { phantom::<u32>(x) } "
        "fn i32_value(x: i32) -> i32 { x }");
    caller = find_free_function(&f.hir, "caller");
    phantom = find_free_function(&f.hir, "phantom");
    assert(caller != NULL && phantom != NULL);
    lower_function(&f, phantom);
    lower_function(&f, caller);
    call_expression = find_body_call(&f.hir,
        caller->data.function_item.body);
    assert(call_expression != CM_HIR_EXPR_NONE);
    u32_type = CM_HIR_TYPE_NONE;
    i32_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < f.hir.types.len; ++index) {
        const CmHirType *type;

        type = cm_hir_get_type(&f.hir, (CmHirTypeId)(index + 1u));
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND) {
            if (type->data.integer_type.kind == CM_HIR_INT_U32) {
                u32_type = (CmHirTypeId)(index + 1u);
            } else if (type->data.integer_type.kind == CM_HIR_INT_I32) {
                i32_type = (CmHirTypeId)(index + 1u);
            }
        }
    }
    assert(u32_type != CM_HIR_TYPE_NONE && i32_type != CM_HIR_TYPE_NONE);
    memset(&callee_argument, 0, sizeof(callee_argument));
    memset(&wrong_argument, 0, sizeof(wrong_argument));
    callee_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    callee_argument.data.type = u32_type;
    wrong_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    wrong_argument.data.type = i32_type;
    cm_hir_instance_spec_init(&caller_spec);
    caller_spec.selected_callable = caller->definition;
    caller_spec.body_definition = caller->definition;
    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = phantom->definition;
    callee_spec.body_definition = phantom->definition;
    callee_spec.item_arguments = &callee_argument;
    callee_spec.item_argument_count = 1u;
    wrong_callee_spec = callee_spec;
    wrong_callee_spec.item_arguments = &wrong_argument;
    flat_instances[0].body = caller->data.function_item.body;
    flat_instances[0].spec = &caller_spec;
    flat_instances[1].body = phantom->data.function_item.body;
    flat_instances[1].spec = &callee_spec;
    flat_call.caller = &caller_spec;
    flat_call.expression = call_expression;
    flat_call.callee = &callee_spec;
    memset(&admission, 0, sizeof(admission));
    cm_hir_canonical_instance_init(&caller_identity);
    cm_hir_canonical_instance_init(&callee_identity);
    cm_hir_canonical_instance_init(&wrong_callee_identity);
    cm_hir_canonical_instance_init(&flat_retained_identity);
    cm_hir_canonical_instance_init(&canonical_retained_identity);
    assert(cm_hir_canonical_instance_encode(&f.hir, 1u, &caller_spec,
            &caller_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode(&f.hir, 1u, &callee_spec,
            &callee_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode(&f.hir, 1u,
            &wrong_callee_spec, &wrong_callee_identity)
                == CM_HIR_INSTANCE_OK);

    result = cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, flat_instances, 2u, &flat_call, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_canonical_instance_callee_identity(results,
            &admission, &caller_identity, call_expression,
            &flat_retained_identity) == CM_SEMANTIC_RESULTS_OK);
    cm_semantic_admission_destroy(&admission);

    canonical_instances[0].identity = &caller_identity;
    canonical_instances[1].identity = &callee_identity;
    canonical_call.caller = &caller_identity;
    canonical_call.expression = call_expression;
    canonical_call.callee = &callee_identity;
    result = cm_semantic_admit_typed_canonical_instance_closure(&admission,
        &f.hir, 1u, canonical_instances, 2u, &canonical_call, 1u);
    results = cm_semantic_admission_results(&admission);
    equal = 0;
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_canonical_instance_callee_identity(results,
            &admission, &caller_identity, call_expression,
            &canonical_retained_identity) == CM_SEMANTIC_RESULTS_OK
        && cm_hir_canonical_instance_equal(&flat_retained_identity,
            &canonical_retained_identity, &equal) == CM_HIR_INSTANCE_OK
        && equal);
    cm_semantic_admission_destroy(&admission);
    snapshot = canonical_admission_snapshot(&f.hir);

    truncated_identity = callee_identity;
    truncated_identity.size -= 1u;
    malformed_instances[0] = canonical_instances[0];
    malformed_instances[1].identity = &truncated_identity;
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        malformed_instances, 2u, &canonical_call, 1u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);

    trailing_bytes = (unsigned char *)cm_alloc(callee_identity.size + 1u);
    memcpy(trailing_bytes, callee_identity.bytes, callee_identity.size);
    trailing_bytes[callee_identity.size] = 0u;
    trailing_identity = callee_identity;
    trailing_identity.bytes = trailing_bytes;
    trailing_identity.size += 1u;
    malformed_instances[1].identity = &trailing_identity;
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        malformed_instances, 2u, &canonical_call, 1u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    cm_free(trailing_bytes);

    duplicate_instances[0] = canonical_instances[0];
    duplicate_instances[1] = canonical_instances[0];
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        duplicate_instances, 2u, NULL, 0u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);

    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        canonical_instances, 2u, NULL, 0u, snapshot,
        CM_SEMANTIC_ADMISSION_HIR_FAILURE);
    duplicate_calls[0] = canonical_call;
    duplicate_calls[1] = canonical_call;
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        canonical_instances, 2u, duplicate_calls, 2u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        canonical_instances, 1u, &canonical_call, 1u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);

    malformed_instances[0] = canonical_instances[0];
    malformed_instances[1].identity = &wrong_callee_identity;
    canonical_call.callee = &wrong_callee_identity;
    assert_canonical_admission_rejected_unchanged(&admission, &f.hir,
        malformed_instances, 2u, &canonical_call, 1u, snapshot,
        CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT);

    cm_hir_canonical_instance_destroy(&canonical_retained_identity);
    cm_hir_canonical_instance_destroy(&flat_retained_identity);
    cm_hir_canonical_instance_destroy(&wrong_callee_identity);
    cm_hir_canonical_instance_destroy(&callee_identity);
    cm_hir_canonical_instance_destroy(&caller_identity);
    fixture_destroy(&f);
}

static void test_exact_instance_closure_authenticates_qualified_call(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmission foreign;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable[2];
    CmSemanticReachableInstanceCall edge;
    CmSemanticReachableInstanceCall duplicates[2];
    CmHirInstanceSpec caller_spec;
    CmHirInstanceSpec callee_spec;
    const CmHirItem *caller;
    const CmHirItem *callee;
    const CmHirItem *impl_item;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmSemanticTypeView parameter;
    CmSemanticFunctionSignatureView signature;
    CmSemanticTypeView signature_parameter;
    CmSemanticExpressionView argument_view;
    CmMirContext mir;
    CmMirPublication publication;
    CmMirLowerResult lower_result;
    const CmMirBody *callee_body;
    const CmMirBody *caller_body;
    CmMirBody *mutable_callee_body;
    CmMirBody *mutable_caller_body;
    CmHirCanonicalInstance caller_identity;
    CmHirCanonicalInstance callee_identity;
    CmMirInstance caller_key;
    CmMirInstance callee_key;
    CmMirBodyId callee_body_id;
    CmMirBodyId caller_body_id;
    unsigned char *saved_identity_bytes;
    size_t saved_identity_size;
    CmHirBodyId saved_identity_body;
    CmHirExprId call_expression;
    CmHirExprId argument;
    int equal;
    int matches;

    fixture_init(&f,
        "trait Convert { fn convert(value: u32) -> u32; } "
        "impl Convert for u32 { fn convert(value: u32) -> u32 { value } } "
        "fn caller(value: u32) -> u32 { <u32 as Convert>::convert(value) }");
    caller = find_free_function(&f.hir, "caller");
    callee = find_impl_method(&f.hir, "convert");
    assert(caller != NULL && callee != NULL);
    lower_function(&f, callee);
    lower_function(&f, caller);
    call_expression = find_body_qualified_call(&f.hir,
        caller->data.function_item.body);
    impl_item = cm_hir_get_item(&f.hir,
        cm_hir_lookup_definition(&f.hir, callee->parent_definition)
            ->entity.item_id);
    assert(call_expression != CM_HIR_EXPR_NONE && impl_item != NULL
        && impl_item->kind == CM_HIR_ITEM_IMPL);
    cm_hir_instance_spec_init(&caller_spec);
    caller_spec.selected_callable = caller->definition;
    caller_spec.body_definition = caller->definition;
    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = callee->definition;
    callee_spec.body_definition = callee->definition;
    callee_spec.declared_trait_callable =
        callee->data.function_item.trait_item_definition;
    callee_spec.enclosing_impl = impl_item->definition;
    callee_spec.implemented_trait =
        impl_item->data.impl_item.trait_type.definition;
    callee_spec.self_owner = impl_item->definition;
    callee_spec.self_type = impl_item->data.impl_item.self_type;
    reachable[0].body = caller->data.function_item.body;
    reachable[0].spec = &caller_spec;
    reachable[1].body = callee->data.function_item.body;
    reachable[1].spec = &callee_spec;
    edge.caller = &caller_spec;
    edge.expression = call_expression;
    edge.callee = &callee_spec;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, &edge, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_callable_selection(results,
            &admission, &caller_spec, call_expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(selection.selected_callable,
            callee->definition)
        && selection.argument_count == 1u
        && cm_semantic_results_instance_callable_argument(results,
            &admission, &caller_spec, call_expression, 0u, &argument)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_expression(results, &admission,
            &caller_spec, argument, &argument_view)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_results_instance_callable_parameter(results,
            &admission, &caller_spec, call_expression, 0u, &parameter)
            == CM_SEMANTIC_RESULTS_OK
        && cm_semantic_type_view_equal(&parameter,
            &argument_view.adjusted_type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal
        && cm_semantic_results_instance_callable_argument(results,
            &admission, &caller_spec, call_expression, 1u, &argument)
            == CM_SEMANTIC_RESULTS_NOT_FOUND);
    cm_mir_context_init(&mir);
    cm_mir_publication_init(&publication);
    cm_hir_canonical_instance_init(&caller_identity);
    cm_hir_canonical_instance_init(&callee_identity);
    memset(&caller_key, 0, sizeof(caller_key));
    memset(&callee_key, 0, sizeof(callee_key));
    assert(cm_hir_canonical_instance_encode(&f.hir, 1u, &caller_spec,
            &caller_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode(&f.hir, 1u, &callee_spec,
            &callee_identity) == CM_HIR_INSTANCE_OK);
    caller_key.definition = caller_identity.definition;
    caller_key.body_definition = caller_identity.body_definition;
    caller_key.body = caller_identity.body;
    caller_key.identity_bytes = caller_identity.bytes;
    caller_key.identity_size = caller_identity.size;
    callee_key.definition = callee_identity.definition;
    callee_key.body_definition = callee_identity.body_definition;
    callee_key.body = callee_identity.body;
    callee_key.identity_bytes = callee_identity.bytes;
    callee_key.identity_size = callee_identity.size;
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
            == CM_MIR_OK
        && cm_mir_publication_reserve_canonical(&publication, &callee_key,
            callee->data.function_item.body, &callee_body_id)
            == CM_MIR_OK
        && cm_mir_publication_reserve_canonical(&publication, &caller_key,
            caller->data.function_item.body, &caller_body_id)
            == CM_MIR_OK);
    cm_hir_canonical_instance_destroy(&callee_identity);
    cm_hir_canonical_instance_destroy(&caller_identity);
    lower_result = cm_mir_lower_admitted_publication_canonical(&mir,
        &publication, &admission, callee_body_id);
    assert(lower_result.error_count == 0u
        && lower_result.body == callee_body_id);
    lower_result = cm_mir_lower_admitted_publication_canonical(&mir,
        &publication, &admission, caller_body_id);
    assert(lower_result.error_count == 0u
        && lower_result.body == caller_body_id
        && cm_mir_publication_validate(&publication) == CM_MIR_OK
        && cm_mir_publication_commit(&publication) == CM_MIR_OK);
    cm_mir_publication_destroy(&publication);
    callee_body = cm_mir_get_body(&mir, callee_body_id);
    caller_body = cm_mir_get_body(&mir, caller_body_id);
    memset(&signature, 0, sizeof(signature));
    memset(&signature_parameter, 0, sizeof(signature_parameter));
    matches = 0;
    assert(callee_body != NULL && caller_body != NULL
        && caller_body->basic_block_count != 0u
        && caller_body->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && caller_body->basic_blocks[0].terminator.data.call.callee_instance
            == callee_body_id
        && caller_body->basic_blocks[0].terminator.data.call.callee.body
            == callee_body->instance.body
        && caller_body->basic_blocks[0].terminator.data.call.callee
            .identity_size == callee_body->instance.identity_size
        && memcmp(caller_body->basic_blocks[0].terminator.data.call.callee
                .identity_bytes, callee_body->instance.identity_bytes,
            callee_body->instance.identity_size) == 0
        && callee_body->local_count == 2u
        && cm_mir_admitted_signature(&mir, &admission, callee_body_id,
            &signature) == CM_MIR_OK
        && cm_hir_def_id_equal(signature.definition, callee->definition)
        && signature.body == callee->data.function_item.body
        && signature.parameter_count == 1u
        && cm_semantic_type_view_matches_monomorphic_hir(results,
            &admission, &signature.return_type, callee_body->locals[0].type,
            &matches) == CM_SEMANTIC_RESULTS_OK && matches
        && cm_mir_admitted_signature_parameter(&mir, &admission,
            callee_body_id, 0u, &signature_parameter) == CM_MIR_OK);
    matches = 0;
    assert(cm_semantic_type_view_matches_monomorphic_hir(results,
            &admission, &signature_parameter, callee_body->locals[1].type,
            &matches) == CM_SEMANTIC_RESULTS_OK && matches);
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        caller_body_id) == CM_MIR_OK);

    /* Exact replay never flattens away one canonical half of a call. */
    mutable_callee_body = (CmMirBody *)callee_body;
    saved_identity_bytes = mutable_callee_body->instance.identity_bytes;
    saved_identity_size = mutable_callee_body->instance.identity_size;
    saved_identity_body = mutable_callee_body->instance.body;
    mutable_callee_body->instance.identity_bytes = NULL;
    mutable_callee_body->instance.identity_size = 0u;
    mutable_callee_body->instance.body = CM_HIR_BODY_NONE;
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        caller_body_id) == CM_MIR_INVALID_ADMISSION);
    mutable_callee_body->instance.identity_bytes = saved_identity_bytes;
    mutable_callee_body->instance.identity_size = saved_identity_size;
    mutable_callee_body->instance.body = saved_identity_body;

    mutable_caller_body = (CmMirBody *)caller_body;
    saved_identity_bytes = mutable_caller_body->instance.identity_bytes;
    saved_identity_size = mutable_caller_body->instance.identity_size;
    saved_identity_body = mutable_caller_body->instance.body;
    mutable_caller_body->instance.identity_bytes = NULL;
    mutable_caller_body->instance.identity_size = 0u;
    mutable_caller_body->instance.body = CM_HIR_BODY_NONE;
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        caller_body_id) == CM_MIR_INVALID_ADMISSION);
    mutable_caller_body->instance.identity_bytes = saved_identity_bytes;
    mutable_caller_body->instance.identity_size = saved_identity_size;
    mutable_caller_body->instance.body = saved_identity_body;
    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        caller_body_id) == CM_MIR_OK);
    cm_mir_context_destroy(&mir);
    memset(&foreign, 0, sizeof(foreign));
    assert(cm_semantic_results_instance_callable_selection(results,
        &foreign, &caller_spec, call_expression, &selection)
        == CM_SEMANTIC_RESULTS_STALE);
    cm_semantic_admission_destroy(&admission);

    memset(&admission, 0, sizeof(admission));
    duplicates[0] = edge;
    duplicates[1] = edge;
    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, duplicates, 2u).status
            == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && admission.state == NULL);
    assert(cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, NULL, 0u).status
            == CM_SEMANTIC_ADMISSION_HIR_FAILURE
        && admission.state == NULL);
    fixture_destroy(&f);
}

static void test_exact_instance_closure_executes_inherited_trait_default(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable[2];
    CmSemanticReachableInstanceCall edge;
    CmHirInstanceSpec caller_spec;
    CmHirInstanceSpec callee_spec;
    const CmHirItem *caller;
    const CmHirItem *trait_default;
    const CmHirItem *impl_item;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmHirInstanceKey key;
    CmHirExprId call_expression;
    size_t index;

    fixture_init(&f,
        "trait Convert { fn convert(value: u32) -> u32 { value + 1u32 } } "
        "impl Convert for u32 {} "
        "fn caller(value: u32) -> u32 { <u32 as Convert>::convert(value) }");
    caller = find_free_function(&f.hir, "caller");
    trait_default = find_trait_default(&f.hir, "convert");
    impl_item = NULL;
    for (index = 0u; index < f.hir.items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(&f.hir.items,
            index);
        if (candidate != NULL && candidate->kind == CM_HIR_ITEM_IMPL
            && candidate->data.impl_item.has_trait
            && !candidate->data.impl_item.is_negative
            && trait_default != NULL
            && cm_hir_def_id_equal(candidate->data.impl_item.trait_type
                    .definition, trait_default->parent_definition)) {
            assert(impl_item == NULL);
            impl_item = candidate;
        }
    }
    assert(caller != NULL && trait_default != NULL && impl_item != NULL);
    lower_function(&f, trait_default);
    lower_function(&f, caller);
    call_expression = find_body_qualified_call(&f.hir,
        caller->data.function_item.body);
    assert(call_expression != CM_HIR_EXPR_NONE);
    cm_hir_instance_spec_init(&caller_spec);
    caller_spec.selected_callable = caller->definition;
    caller_spec.body_definition = caller->definition;
    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = trait_default->definition;
    callee_spec.body_definition = trait_default->definition;
    callee_spec.declared_trait_callable = trait_default->definition;
    callee_spec.enclosing_impl = impl_item->definition;
    callee_spec.implemented_trait = trait_default->parent_definition;
    callee_spec.self_owner = impl_item->definition;
    callee_spec.self_type = impl_item->data.impl_item.self_type;
    reachable[0].body = caller->data.function_item.body;
    reachable[0].spec = &caller_spec;
    reachable[1].body = trait_default->data.function_item.body;
    reachable[1].spec = &callee_spec;
    edge.caller = &caller_spec;
    edge.expression = call_expression;
    edge.callee = &callee_spec;
    memset(&admission, 0, sizeof(admission));
    memset(&key, 0, sizeof(key));
    result = cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, &edge, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_callable_selection(results,
            &admission, &caller_spec, call_expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(selection.selected_impl,
            impl_item->definition)
        && cm_hir_def_id_equal(selection.selected_callable,
            trait_default->definition)
        && cm_hir_def_id_equal(selection.body_definition,
            trait_default->definition)
        && cm_hir_instance_key_init(&key, &admission, &callee_spec)
            == CM_HIR_INSTANCE_OK);
    cm_hir_instance_key_destroy(&key);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_exact_instance_closure_prefers_trait_override(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable[2];
    CmSemanticReachableInstanceCall edge;
    CmHirInstanceSpec caller_spec;
    CmHirInstanceSpec callee_spec;
    const CmHirItem *caller;
    const CmHirItem *trait_default;
    const CmHirItem *impl_item;
    const CmHirItem *override;
    const CmSemanticResults *results;
    CmSemanticCallableSelectionView selection;
    CmHirExprId call_expression;
    size_t index;

    fixture_init(&f,
        "trait Convert { fn convert(value: u32) -> u32 { value + 1u32 } } "
        "impl Convert for u32 { "
        "fn convert(value: u32) -> u32 { value + 2u32 } } "
        "fn caller(value: u32) -> u32 { <u32 as Convert>::convert(value) }");
    caller = find_free_function(&f.hir, "caller");
    trait_default = find_trait_default(&f.hir, "convert");
    impl_item = NULL;
    override = NULL;
    for (index = 0u; index < f.hir.items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(&f.hir.items,
            index);
        if (candidate != NULL && candidate->kind == CM_HIR_ITEM_IMPL
            && candidate->data.impl_item.has_trait
            && trait_default != NULL
            && cm_hir_def_id_equal(candidate->data.impl_item.trait_type
                    .definition, trait_default->parent_definition)) {
            impl_item = candidate;
        } else if (candidate != NULL
            && candidate->kind == CM_HIR_ITEM_FUNCTION
            && trait_default != NULL
            && cm_hir_def_id_equal(candidate->data.function_item
                    .trait_item_definition, trait_default->definition)) {
            override = candidate;
        }
    }
    assert(caller != NULL && trait_default != NULL && impl_item != NULL
        && override != NULL && override->data.function_item.body
            != CM_HIR_BODY_NONE);
    lower_function(&f, trait_default);
    lower_function(&f, override);
    lower_function(&f, caller);
    call_expression = find_body_qualified_call(&f.hir,
        caller->data.function_item.body);
    assert(call_expression != CM_HIR_EXPR_NONE);
    cm_hir_instance_spec_init(&caller_spec);
    caller_spec.selected_callable = caller->definition;
    caller_spec.body_definition = caller->definition;
    cm_hir_instance_spec_init(&callee_spec);
    callee_spec.selected_callable = override->definition;
    callee_spec.body_definition = override->definition;
    callee_spec.declared_trait_callable = trait_default->definition;
    callee_spec.enclosing_impl = impl_item->definition;
    callee_spec.implemented_trait = trait_default->parent_definition;
    callee_spec.self_owner = impl_item->definition;
    callee_spec.self_type = impl_item->data.impl_item.self_type;
    reachable[0].body = caller->data.function_item.body;
    reachable[0].spec = &caller_spec;
    reachable[1].body = override->data.function_item.body;
    reachable[1].spec = &callee_spec;
    edge.caller = &caller_spec;
    edge.expression = call_expression;
    edge.callee = &callee_spec;
    memset(&admission, 0, sizeof(admission));
    result = cm_semantic_admit_typed_instance_closure(&admission, &f.hir,
        1u, reachable, 2u, &edge, 1u);
    results = cm_semantic_admission_results(&admission);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK && results != NULL
        && cm_semantic_results_instance_callable_selection(results,
            &admission, &caller_spec, call_expression, &selection)
            == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(selection.selected_impl,
            impl_item->definition)
        && cm_hir_def_id_equal(selection.selected_callable,
            override->definition)
        && cm_hir_def_id_equal(selection.body_definition,
            override->definition));
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_reachable_admission_scope_is_enforced(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableBody reachable;
    CmHirInstanceKey key;
    CmHirInstanceSpec spec;
    CmMirContext admitted_mir;
    CmMirContext legacy_mir;
    CmMirContext rejected_mir;
    CmMirLowerResult lower_result;
    CmMirBody candidate;
    CmMirBodyId copied_id;
    const CmHirItem *selected;
    const CmHirItem *unselected;
    const CmMirBody *stored;

    fixture_init(&f,
        "fn selected(x: u32) -> u32 { x + 1u32 } "
        "fn unselected(x: u32) -> u32 { x }");
    selected = find_free_function(&f.hir, "selected");
    unselected = find_free_function(&f.hir, "unselected");
    assert(selected != NULL && unselected != NULL);
    lower_function(&f, selected);
    lower_function(&f, unselected);
    reachable.owner = selected->definition;
    reachable.body = selected->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    assert(cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, &reachable, 1u).status == CM_SEMANTIC_ADMISSION_OK);

    cm_mir_context_init(&admitted_mir);
    lower_result = cm_mir_lower_admitted_body(&admitted_mir, &admission,
        unselected->data.function_item.body);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_MIR_LOWER_INVALID_ADMISSION
        && cm_mir_body_count(&admitted_mir) == 0u);

    cm_mir_context_init(&legacy_mir);
    lower_result = cm_mir_lower_instance(&legacy_mir, &f.hir,
        unselected->data.function_item.body, NULL, 0u);
    assert(lower_result.error_count == 0u);
    stored = cm_mir_get_body(&legacy_mir, lower_result.body);
    assert(stored != NULL);
    candidate = *stored;
    candidate.owned_storage = NULL;
    cm_mir_context_init(&rejected_mir);
    copied_id = 99u;
    assert(cm_mir_add_admitted_monomorphized_body(&rejected_mir,
        &admission, &candidate, &copied_id) == CM_MIR_INVALID_ADMISSION
        && copied_id == CM_MIR_BODY_NONE
        && cm_mir_body_count(&rejected_mir) == 0u);

    memset(&key, 0, sizeof(key));
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = unselected->definition;
    spec.body_definition = unselected->definition;
    assert(cm_hir_instance_key_init(&key, &admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);

    cm_mir_context_destroy(&rejected_mir);
    cm_mir_context_destroy(&legacy_mir);
    cm_mir_context_destroy(&admitted_mir);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&f);
}

static void test_admitted_mir_header_uses_semantic_signature(void)
{
    Fixture f;
    CmSemanticAdmission admission;
    CmSemanticReachableBody reachable;
    CmMirContext mir;
    CmMirContext copied;
    CmMirLowerResult lower_result;
    CmMirBody candidate;
    CmMirBodyId copied_id;
    const CmHirItem *selected;
    const CmHirItem *bool_function;
    const CmHirDefinition *definition;
    CmHirItem *mutable_selected;
    const CmMirBody *stored;

    fixture_init(&f,
        "fn selected(x: u32) -> u32 { x } "
        "fn bool_type(x: bool) -> bool { x }");
    selected = find_free_function(&f.hir, "selected");
    bool_function = find_free_function(&f.hir, "bool_type");
    assert(selected != NULL && bool_function != NULL);
    lower_function(&f, selected);
    reachable.owner = selected->definition;
    reachable.body = selected->data.function_item.body;
    memset(&admission, 0, sizeof(admission));
    assert(cm_semantic_admit_typed_reachable_bodies(&admission, &f.hir,
        1u, &reachable, 1u).status == CM_SEMANTIC_ADMISSION_OK);
    cm_mir_context_init(&mir);
    lower_result = cm_mir_lower_admitted_body(&mir, &admission,
        reachable.body);
    assert(lower_result.error_count == 0u);
    stored = cm_mir_get_body(&mir, lower_result.body);
    assert(stored != NULL);
    candidate = *stored;
    candidate.owned_storage = NULL;

    definition = cm_hir_lookup_definition(&f.hir, selected->definition);
    assert(definition != NULL && definition->kind == CM_HIR_DEFINITION_ITEM);
    mutable_selected = (CmHirItem *)cm_vec_at(&f.hir.items,
        (size_t)definition->entity.item_id - 1u);
    assert(mutable_selected != NULL
        && mutable_selected->data.function_item.signature.parameter_count
            == 1u);
    mutable_selected->data.function_item.signature.return_type =
        bool_function->data.function_item.signature.return_type;
    mutable_selected->data.function_item.signature.parameters[0].type =
        bool_function->data.function_item.signature.parameters[0].type;

    assert(cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
        lower_result.body) == CM_MIR_OK);
    cm_mir_context_init(&copied);
    assert(cm_mir_add_admitted_monomorphized_body(&copied, &admission,
        &candidate, &copied_id) == CM_MIR_OK && copied_id == 1u);
    cm_mir_context_destroy(&copied);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);
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
    for (status = 0u;
         status <= (unsigned int)CM_SEMANTIC_ADMISSION_INVALID_BARRIER;
         ++status)
        assert(strcmp(cm_semantic_admission_status_name(
            (CmSemanticAdmissionStatus)status), "unknown") != 0);
}

static void test_regions_admission_authority_gates(void)
{
    Fixture f;
    Fixture foreign;
    Fixture replacement;
    CmSemanticBarrier barrier;
    CmSemanticBarrier foreign_barrier;
    CmSemanticAdmission admission;
    CmSemanticAdmission foreign_admission;
    CmSemanticAdmission marked_admission;
    CmSemanticAdmission exact_admission;
    CmSemanticAdmission rejected_admission;
    CmSemanticAdmissionResult result;
    CmSemanticCanonicalReachableInstance instance;
    CmHirCanonicalInstance identity;
    CmHirInstanceSpec spec;
    const CmHirItem *function;
    CmHirItem *second;
    CmHirBodyId saved_body;
    CmMirContext mir;
    CmMirPublication publication;
    uint64_t marked_capability;
    uint64_t regions_capability;
    uint64_t admission_capability;

    fixture_init(&f,
        "fn first(value: u32) -> u32 { value } "
        "fn second(value: u32) -> u32 { first(value) }");
    fixture_init(&foreign, "fn foreign(value: u32) -> u32 { value }");
    fixture_init(&replacement, "fn replacement() -> u32 { 2u32 }");
    memset(&barrier, 0, sizeof(barrier));
    memset(&foreign_barrier, 0, sizeof(foreign_barrier));
    memset(&admission, 0, sizeof(admission));
    memset(&foreign_admission, 0, sizeof(foreign_admission));
    memset(&marked_admission, 0, sizeof(marked_admission));
    memset(&exact_admission, 0, sizeof(exact_admission));
    memset(&rejected_admission, 0, sizeof(rejected_admission));

    assert(init_barrier(&f, &barrier).status == CM_SEMANTIC_BARRIER_OK);
    result = cm_semantic_admit_regions_local_crate(&admission, &barrier);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_BARRIER
        && admission.state == NULL);
    assert(cm_semantic_barrier_advance_typed(&barrier, &f.graph,
            f.graph_result.revision, &f.imports, &f.modules).status
        == CM_SEMANTIC_BARRIER_OK);
    result = cm_semantic_admit_regions_local_crate(&admission, &barrier);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_BARRIER
        && admission.state == NULL);
    assert(cm_semantic_barrier_advance_marked(&barrier).status
        == CM_SEMANTIC_BARRIER_OK);
    marked_capability = cm_semantic_barrier_capability_id(&barrier);
    assert(marked_capability != UINT64_C(0)
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_MARKED);
    result = cm_semantic_admit_regions_local_crate(&admission, &barrier);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_BARRIER
        && admission.state == NULL);

    /* A same-generation compatibility admission is not REGIONS authority. */
    assert(admit(&f, &marked_admission).status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_barrier_capability_id(&marked_admission)
            == UINT64_C(0));
    assert(cm_semantic_barrier_advance_regions_admitted(&barrier,
            &marked_admission).status == CM_SEMANTIC_BARRIER_OK);
    regions_capability = cm_semantic_barrier_capability_id(&barrier);
    assert(regions_capability != UINT64_C(0)
        && regions_capability != marked_capability
        && cm_semantic_barrier_phase(&barrier)
            == CM_SEMANTIC_BARRIER_REGIONS);
    result = cm_semantic_admit_regions_local_crate(&admission, &barrier);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_is_current(&admission)
        && cm_semantic_results_seal_kind(
            cm_semantic_admission_results(&admission))
                == CM_SEMANTIC_RESULTS_SEAL_WHOLE_LOCAL
        && cm_semantic_admission_barrier_capability_id(&admission)
            == regions_capability);

    /* A live barrier still fails closed if an item no longer matches its
     * captured body/owner manifest. */
    second = (CmHirItem *)cm_hir_get_item(&f.hir, 2u);
    assert(second != NULL && second->kind == CM_HIR_ITEM_FUNCTION);
    saved_body = second->data.function_item.body;
    second->data.function_item.body = 1u;
    assert(cm_semantic_barrier_is_current(&barrier));
    result = cm_semantic_admit_regions_local_crate(&foreign_admission,
        &barrier);
    assert(result.status != CM_SEMANTIC_ADMISSION_OK
        && foreign_admission.state == NULL);
    second->data.function_item.body = saved_body;

    function = find_free_function(&f.hir, "first");
    assert(function != NULL);
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = function->definition;
    spec.body_definition = function->definition;
    cm_hir_canonical_instance_init(&identity);
    assert(cm_hir_canonical_instance_encode(&f.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_OK);
    instance.identity = &identity;

    /* Exact admissions require a parent admitted by this precise barrier. */
    result = cm_semantic_admit_regions_canonical_instance_closure(
        &exact_admission, &barrier, &marked_admission, &instance, 1u,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && exact_admission.state == NULL);
    result = cm_semantic_admit_regions_canonical_instance_closure(
        &exact_admission, &barrier, &admission, &instance, 1u, NULL, 0u);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_is_current(&exact_admission)
        && cm_semantic_results_seal_kind(
            cm_semantic_admission_results(&exact_admission))
                == CM_SEMANTIC_RESULTS_SEAL_INSTANCE_CLOSURE
        && cm_semantic_admission_barrier_capability_id(&exact_admission)
            == regions_capability);
    cm_mir_context_init(&mir);
    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin_regions(&publication, &mir,
            &exact_admission) == CM_MIR_OK);

    assert(init_barrier(&foreign, &foreign_barrier).status
        == CM_SEMANTIC_BARRIER_OK);
    advance_barrier_to_regions(&foreign, &foreign_barrier);
    assert(cm_semantic_admit_regions_local_crate(&foreign_admission,
            &foreign_barrier).status == CM_SEMANTIC_ADMISSION_OK);
    result = cm_semantic_admit_regions_canonical_instance_closure(
        &rejected_admission, &barrier, &foreign_admission, &instance, 1u,
        NULL, 0u);
    assert(result.status == CM_SEMANTIC_ADMISSION_INVALID_ARGUMENT
        && rejected_admission.state == NULL);

    /* Replacing a parent with a fresh capability does not revive a child
     * that retained the previous exact parent capability. */
    admission_capability = cm_semantic_admission_capability_id(&admission);
    assert(admission_capability != UINT64_C(0));
    cm_semantic_admission_destroy(&admission);
    assert(!cm_semantic_admission_is_current(&exact_admission)
        && cm_mir_publication_validate(&publication)
            == CM_MIR_INVALID_ADMISSION
        && cm_mir_publication_commit(&publication)
            == CM_MIR_INVALID_ADMISSION);
    cm_mir_publication_destroy(&publication);
    cm_mir_context_destroy(&mir);
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_capability_id(&admission)
            != admission_capability
        && !cm_semantic_admission_is_current(&exact_admission));

    cm_hir_canonical_instance_destroy(&identity);
    cm_semantic_admission_destroy(&rejected_admission);
    cm_semantic_admission_destroy(&exact_admission);
    cm_semantic_admission_destroy(&foreign_admission);
    cm_semantic_admission_destroy(&marked_admission);
    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&foreign_barrier);

    /* Destroying and reinitializing the same wrapper cannot revive an old
     * admission, even when the replacement barrier is itself current. */
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK);
    cm_semantic_barrier_destroy(&barrier);
    assert(!cm_semantic_admission_is_current(&admission));
    assert(init_barrier(&replacement, &barrier).status
            == CM_SEMANTIC_BARRIER_OK
        && !cm_semantic_admission_is_current(&admission));
    advance_barrier_to_regions(&replacement, &barrier);
    assert(!cm_semantic_admission_is_current(&admission)
        && cm_semantic_admission_barrier_capability_id(&admission)
            == UINT64_C(0));

    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&replacement);
    fixture_destroy(&foreign);
    fixture_destroy(&f);
}

static void test_regions_admission_snapshot_invalidation(void)
{
    Fixture f;
    CmSemanticBarrier barrier;
    CmSemanticAdmission admission;
    CmHirType type;
    CmHirTypeId type_id;

    fixture_init(&f, "fn value() -> u32 { 1u32 }");
    memset(&barrier, 0, sizeof(barrier));
    memset(&admission, 0, sizeof(admission));
    assert(init_barrier(&f, &barrier).status == CM_SEMANTIC_BARRIER_OK);
    advance_barrier_to_regions(&f, &barrier);
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = (CmSpan){ f.source, 0u, 1u };
    assert(cm_hir_add_type(&f.hir, &type, &type_id) == CM_HIR_OK
        && !cm_semantic_barrier_is_current(&barrier)
        && !cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&f);

    fixture_init(&f, "fn value() -> u32 { 1u32 }");
    memset(&barrier, 0, sizeof(barrier));
    assert(init_barrier(&f, &barrier).status == CM_SEMANTIC_BARRIER_OK);
    advance_barrier_to_regions(&f, &barrier);
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK);
    cm_module_graph_destroy(&f.graph);
    cm_module_graph_init(&f.graph);
    assert(!cm_semantic_barrier_is_current(&barrier)
        && !cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&f);

    fixture_init(&f, "fn value() -> u32 { 1u32 }");
    memset(&barrier, 0, sizeof(barrier));
    assert(init_barrier(&f, &barrier).status == CM_SEMANTIC_BARRIER_OK);
    advance_barrier_to_regions(&f, &barrier);
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK);
    cm_import_resolver_destroy(&f.imports);
    cm_import_resolver_init(&f.imports);
    assert(!cm_semantic_barrier_is_current(&barrier)
        && !cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&f);

    fixture_init(&f, "fn value() -> u32 { 1u32 }");
    memset(&barrier, 0, sizeof(barrier));
    assert(init_barrier(&f, &barrier).status == CM_SEMANTIC_BARRIER_OK);
    advance_barrier_to_regions(&f, &barrier);
    assert(cm_semantic_admit_regions_local_crate(&admission, &barrier).status
        == CM_SEMANTIC_ADMISSION_OK);
    cm_hir_module_map_destroy(&f.modules);
    cm_hir_module_map_init(&f.modules);
    assert(!cm_semantic_barrier_is_current(&barrier)
        && !cm_semantic_admission_is_current(&admission));
    cm_semantic_admission_destroy(&admission);
    cm_semantic_barrier_destroy(&barrier);
    fixture_destroy(&f);
}

int main(void)
{
    test_success_and_stale();
    test_body_failure_rolls_back();
    test_semantic_failure_rolls_back();
    test_closed_trait_default_is_admitted_but_not_executable();
    test_concrete_impl_method_is_admitted();
    test_generic_impl_method_definition_is_admitted();
    test_impl_method_body_failure_is_atomic();
    test_mir_admission_gates();
    test_reachable_admission_subset_and_mir();
    test_reachable_admission_requires_closed_unique_set();
    test_reachable_admission_rejects_untyped_and_generic();
    test_leaf_instance_admission_is_exact();
    test_generic_impl_method_instances_are_exact();
    test_leaf_instance_recipes_are_exact();
    test_leaf_instance_admitted_mir_is_exact();
    test_leaf_instance_admission_rejects_calls_and_bounds();
    test_exact_instance_closure_authenticates_generic_calls();
    test_canonical_instance_closure_is_exact_and_atomic();
    test_exact_instance_closure_authenticates_qualified_call();
    test_exact_instance_closure_executes_inherited_trait_default();
    test_exact_instance_closure_prefers_trait_override();
    test_reachable_admission_scope_is_enforced();
    test_admitted_mir_header_uses_semantic_signature();
    test_regions_admission_authority_gates();
    test_regions_admission_snapshot_invalidation();
    test_invalid_api();
    puts("hir semantic admission tests passed");
    return 0;
}
