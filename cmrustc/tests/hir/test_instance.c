#include "cm/hir/instance.h"
#include "cm/hir/lower.h"
#include "cm/source.h"

#include "cm/alloc.h"

#include <assert.h>
#include <setjmp.h>
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
    CmSemanticAdmission admission;
    CmHirDefId callable;
    CmHirDefId trait_definition;
    CmHirDefId declared_method;
    CmHirDefId impl_definition;
    CmHirDefId selected_method;
    CmHirTypeId u32_type;
    CmHirTypeId static_reference;
    CmHirTypeId duplicate_static_reference;
    CmHirTypeId erased_reference;
    CmHirTypeId bound_reference;
    CmHirTypeId array_128;
} Fixture;

static jmp_buf oom_jump;

static void jump_on_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    longjmp(oom_jump, 1);
}

static int intern_is(const CmHirContext *hir, CmInternId id,
    const char *expected)
{
    const CmInternedString *value;
    size_t length;

    value = cm_interner_get(&hir->strings, id);
    length = strlen(expected);
    return value != NULL && value->len == length
        && memcmp(value->bytes, expected, length) == 0;
}

static void fixture_init(Fixture *fixture)
{
    static const char source[] =
        "fn choose<A, B>(left: A, _right: B) -> A { left } "
        "trait Value { fn value(value: u32) -> u32; } "
        "impl Value for u32 { fn value(value: u32) -> u32 { value } }";
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmSemanticAdmissionResult admission_result;
    CmHirType type_value;
    CmHirTypeId u32_type;
    CmHirTypeId u128_type;
    size_t index;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    assert(cm_source_add_memory(&fixture->sources, "instance/lib.rs",
        (const unsigned char *)source, strlen(source), &fixture->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2024;
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
    lower_options.crate_name = "instance_test";
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->modules, &lower_options);
    assert(lower_result.error_count == 0u);
    u32_type = CM_HIR_TYPE_NONE;
    u128_type = CM_HIR_TYPE_NONE;
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&fixture->hir.items,
            index);
        if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && intern_is(&fixture->hir, item->name, "choose")) {
            fixture->callable = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_TRAIT
            && intern_is(&fixture->hir, item->name, "Value")) {
            fixture->trait_definition = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_IMPL) {
            fixture->impl_definition = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && intern_is(&fixture->hir, item->name, "value")) {
            if (item->data.function_item.body == CM_HIR_BODY_NONE) {
                fixture->declared_method = item->definition;
            } else {
                fixture->selected_method = item->definition;
            }
        }
    }
    for (index = 0u; index < fixture->hir.types.len; ++index) {
        const CmHirType *type;
        CmHirTypeId type_id;

        type_id = (CmHirTypeId)(index + 1u);
        type = cm_hir_get_type(&fixture->hir, type_id);
        if (type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND) {
            if (type->data.integer_type.kind == CM_HIR_INT_U32) {
                u32_type = type_id;
            } else if (type->data.integer_type.kind == CM_HIR_INT_U128) {
                u128_type = type_id;
            }
        }
    }
    if (u32_type == CM_HIR_TYPE_NONE) {
        memset(&type_value, 0, sizeof(type_value));
        type_value.kind = CM_HIR_TYPE_INTEGER_KIND;
        type_value.span.source = fixture->source;
        type_value.data.integer_type.kind = CM_HIR_INT_U32;
        assert(cm_hir_add_type(&fixture->hir, &type_value, &u32_type)
            == CM_HIR_OK);
    }
    if (u128_type == CM_HIR_TYPE_NONE) {
        memset(&type_value, 0, sizeof(type_value));
        type_value.kind = CM_HIR_TYPE_INTEGER_KIND;
        type_value.span.source = fixture->source;
        type_value.data.integer_type.kind = CM_HIR_INT_U128;
        assert(cm_hir_add_type(&fixture->hir, &type_value, &u128_type)
            == CM_HIR_OK);
    }
    memset(&type_value, 0, sizeof(type_value));
    type_value.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type_value.span.source = fixture->source;
    type_value.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type_value.data.reference_type.pointee = u32_type;
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->static_reference) == CM_HIR_OK);
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->duplicate_static_reference) == CM_HIR_OK);
    type_value.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->erased_reference) == CM_HIR_OK);
    type_value.data.reference_type.region.kind = CM_HIR_REGION_INFER;
    type_value.data.reference_type.region.data.inference_variable = 9u;
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->bound_reference) == CM_HIR_OK);
    memset(&type_value, 0, sizeof(type_value));
    type_value.kind = CM_HIR_TYPE_ARRAY_KIND;
    type_value.span.source = fixture->source;
    type_value.data.array_type.element = u128_type;
    type_value.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type_value.data.array_type.length.type = u128_type;
    type_value.data.array_type.length.data.value.low_bits = UINT64_MAX;
    type_value.data.array_type.length.data.value.high_bits = UINT64_MAX;
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->array_128) == CM_HIR_OK);
    memset(&fixture->admission, 0, sizeof(fixture->admission));
    admission_result = cm_semantic_admit_local_crate(&fixture->admission,
        &fixture->hir, 1u, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports,
        &fixture->modules);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);
    assert(!cm_hir_def_id_is_none(fixture->callable)
        && !cm_hir_def_id_is_none(fixture->trait_definition)
        && !cm_hir_def_id_is_none(fixture->declared_method)
        && !cm_hir_def_id_is_none(fixture->impl_definition)
        && !cm_hir_def_id_is_none(fixture->selected_method)
        && u32_type != CM_HIR_TYPE_NONE
        && fixture->static_reference != CM_HIR_TYPE_NONE
        && fixture->bound_reference != CM_HIR_TYPE_NONE
        && fixture->array_128 != CM_HIR_TYPE_NONE);
    fixture->u32_type = u32_type;
}

static void fixture_destroy(Fixture *fixture)
{
    cm_semantic_admission_destroy(&fixture->admission);
    cm_hir_module_map_destroy(&fixture->modules);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmHirInstanceSpec make_spec(const Fixture *fixture,
    CmHirGenericArg arguments[2])
{
    CmHirInstanceSpec spec;

    memset(arguments, 0, 2u * sizeof(*arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = fixture->static_reference;
    arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[1].data.type = fixture->array_128;
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture->callable;
    spec.item_arguments = arguments;
    spec.item_argument_count = 2u;
    return spec;
}

static void test_structural_key_clone_compare_dump(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirGenericArg reversed[2];
    CmHirInstanceSpec spec;
    CmHirInstanceSpec reversed_spec;
    CmHirInstanceKey key;
    CmHirInstanceKey clone;
    CmHirInstanceKey other;
    CmHirInstanceKey duplicate;
    FILE *stream;
    char dump[2048];
    size_t count;
    int equal;
    int order;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    memset(&clone, 0, sizeof(clone));
    memset(&other, 0, sizeof(other));
    memset(&duplicate, 0, sizeof(duplicate));
    spec = make_spec(&fixture, arguments);
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);
    assert(cm_hir_instance_key_validate(&key, &fixture.admission)
        == CM_HIR_INSTANCE_OK);
    assert(cm_hir_instance_key_clone(&clone, &fixture.admission, &key)
        == CM_HIR_INSTANCE_OK);
    assert(cm_hir_instance_key_equal(&fixture.admission, &key, &clone,
        &equal) == CM_HIR_INSTANCE_OK && equal);
    reversed[0] = arguments[1];
    reversed[1] = arguments[0];
    reversed_spec = spec;
    reversed_spec.item_arguments = reversed;
    assert(cm_hir_instance_key_init(&other, &fixture.admission,
        &reversed_spec) == CM_HIR_INSTANCE_OK);
    assert(cm_hir_instance_key_equal(&fixture.admission, &key, &other,
        &equal) == CM_HIR_INSTANCE_OK && !equal);
    assert(cm_hir_instance_key_compare(&fixture.admission, &key, &other,
        &order) == CM_HIR_INSTANCE_OK && order != 0);
    reversed_spec = spec;
    reversed[0] = arguments[0];
    reversed[0].data.type = fixture.duplicate_static_reference;
    reversed[1] = arguments[1];
    reversed_spec.item_arguments = reversed;
    assert(cm_hir_instance_key_init(&duplicate, &fixture.admission,
        &reversed_spec) == CM_HIR_INSTANCE_OK);
    assert(cm_hir_instance_key_equal(&fixture.admission, &key, &duplicate,
        &equal) == CM_HIR_INSTANCE_OK && equal);
    stream = tmpfile();
    assert(stream != NULL
        && cm_hir_instance_key_dump(stream, &fixture.admission, &key)
            == CM_HIR_INSTANCE_OK
        && fflush(stream) == 0 && fseek(stream, 0L, SEEK_SET) == 0);
    count = fread(dump, 1u, sizeof(dump) - 1u, stream);
    dump[count] = '\0';
    assert(strstr(dump, "hir-instance-v1 crate=1") != NULL
        && strstr(dump, "ffffffffffffffffffffffffffffffff") != NULL);
    fclose(stream);
    cm_hir_instance_key_destroy(&duplicate);
    cm_hir_instance_key_destroy(&other);
    cm_hir_instance_key_destroy(&clone);
    cm_hir_instance_key_destroy(&key);
    fixture_destroy(&fixture);
}

static void test_authenticated_method_identity(void)
{
    Fixture fixture;
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture.selected_method;
    spec.declared_trait_callable = fixture.declared_method;
    spec.enclosing_impl = fixture.impl_definition;
    spec.implemented_trait = fixture.trait_definition;
    spec.self_owner = fixture.impl_definition;
    spec.self_type = fixture.u32_type;
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);
    cm_hir_instance_key_destroy(&key);
    spec.declared_trait_callable = fixture.callable;
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    spec.declared_trait_callable = fixture.declared_method;
    spec.self_owner = fixture.trait_definition;
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    fixture_destroy(&fixture);
}

static void test_fail_closed_and_stale(void)
{
    Fixture fixture;
    Fixture foreign;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;
    CmHirInstanceKey rejected;
    CmHirType type;
    CmHirTypeId type_id;

    fixture_init(&fixture);
    fixture_init(&foreign);
    memset(&key, 0, sizeof(key));
    memset(&rejected, 0, sizeof(rejected));
    spec = make_spec(&fixture, arguments);
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);
    arguments[0].data.type = fixture.bound_reference;
    assert(cm_hir_instance_key_init(&rejected, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_UNSUPPORTED_REGION
        && rejected.state == NULL);
    arguments[0].data.type = fixture.static_reference;
    assert(cm_hir_instance_key_validate(&key, &foreign.admission)
        == CM_HIR_INSTANCE_FOREIGN_ADMISSION);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INFER_KIND;
    type.span.source = fixture.source;
    type.data.infer_type.kind = CM_HIR_INFER_GENERAL;
    assert(cm_hir_add_type(&fixture.hir, &type, &type_id) == CM_HIR_OK);
    assert(cm_hir_instance_key_validate(&key, &fixture.admission)
        == CM_HIR_INSTANCE_STALE_ADMISSION);
    assert(cm_hir_instance_key_init(&rejected, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_STALE_ADMISSION);
    cm_hir_instance_key_destroy(&key);
    fixture_destroy(&foreign);
    fixture_destroy(&fixture);
}

static void test_same_hir_foreign_admission(void)
{
    Fixture fixture;
    CmSemanticAdmission foreign_admission;
    CmSemanticAdmissionResult admission_result;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;
    CmHirInstanceKey clone;
    FILE *stream;
    int equal;
    int order;

    fixture_init(&fixture);
    memset(&foreign_admission, 0, sizeof(foreign_admission));
    memset(&key, 0, sizeof(key));
    memset(&clone, 0, sizeof(clone));
    spec = make_spec(&fixture, arguments);
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);
    admission_result = cm_semantic_admit_local_crate(&foreign_admission,
        &fixture.hir, 1u, &fixture.graph, fixture.graph_result.revision,
        &fixture.imports, &fixture.modules);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_is_current(&fixture.admission)
        && cm_semantic_admission_is_current(&foreign_admission)
        && cm_semantic_admission_generation(&fixture.admission)
            == cm_semantic_admission_generation(&foreign_admission)
        && cm_semantic_admission_capability_id(&fixture.admission)
            != cm_semantic_admission_capability_id(&foreign_admission));

    equal = 1;
    order = 1;
    stream = tmpfile();
    assert(stream != NULL
        && cm_hir_instance_key_validate(&key, &foreign_admission)
            == CM_HIR_INSTANCE_FOREIGN_ADMISSION
        && cm_hir_instance_key_clone(&clone, &foreign_admission, &key)
            == CM_HIR_INSTANCE_FOREIGN_ADMISSION
        && clone.state == NULL
        && cm_hir_instance_key_equal(&foreign_admission, &key, &key,
            &equal) == CM_HIR_INSTANCE_FOREIGN_ADMISSION
        && equal == 0
        && cm_hir_instance_key_compare(&foreign_admission, &key, &key,
            &order) == CM_HIR_INSTANCE_FOREIGN_ADMISSION
        && order == 0
        && cm_hir_instance_key_dump(stream, &foreign_admission, &key)
            == CM_HIR_INSTANCE_FOREIGN_ADMISSION);
    fclose(stream);
    assert(cm_hir_instance_key_validate(&key, &fixture.admission)
        == CM_HIR_INSTANCE_OK);

    cm_hir_instance_key_destroy(&key);
    cm_semantic_admission_destroy(&foreign_admission);
    fixture_destroy(&fixture);
}

static void test_clone_oom_is_transactional(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;
    CmHirInstanceKey clone;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    memset(&clone, 0, sizeof(clone));
    spec = make_spec(&fixture, arguments);
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_hir_instance_key_clone(&clone, &fixture.admission, &key);
        assert(0);
    }
    cm_alloc_fail_never();
    assert(clone.state == NULL
        && cm_hir_instance_key_validate(&key, &fixture.admission)
            == CM_HIR_INSTANCE_OK);
    cm_alloc_set_oom_handler(NULL, NULL);
    cm_hir_instance_key_destroy(&key);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_structural_key_clone_compare_dump();
    test_authenticated_method_identity();
    test_fail_closed_and_stale();
    test_same_hir_foreign_admission();
    test_clone_oom_is_transactional();
    puts("hir instance key tests passed");
    return 0;
}
