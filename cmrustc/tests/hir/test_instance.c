#include "cm/hir/instance.h"
#include "cm/hir/lower.h"
#include "cm/source.h"

#include "cm/alloc.h"

#include "../../src/hir/instance_internal.h"

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
    CmHirDefId default_trait_definition;
    CmHirDefId default_method_definition;
    CmHirDefId default_u32_impl_definition;
    CmHirDefId default_usize_impl_definition;
    CmHirDefId holder_definition;
    CmHirTypeId u32_type;
    CmHirTypeId usize_type;
    CmHirTypeId holder_u32_type;
    CmHirTypeId static_reference;
    CmHirTypeId duplicate_static_reference;
    CmHirTypeId erased_reference;
    CmHirTypeId bound_reference;
    CmHirTypeId bound_function_pointer;
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
        "type BoundFn = for<'a> fn(&'a u8); "
        "struct Holder<T> { value: T } "
        "trait Value { fn value(value: u32) -> u32; } "
        "impl Value for u32 { fn value(value: u32) -> u32 { value } } "
        "trait DefaultValue { "
        "fn default_value(value: u32) -> u32 { value } } "
        "impl DefaultValue for u32 {} "
        "impl DefaultValue for usize {}";
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmSemanticAdmissionResult admission_result;
    CmHirGenericArg holder_argument;
    CmHirType type_value;
    CmHirTypeId u32_type;
    CmHirTypeId usize_type;
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
    usize_type = CM_HIR_TYPE_NONE;
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
        } else if (item != NULL && item->kind == CM_HIR_ITEM_TRAIT
            && intern_is(&fixture->hir, item->name, "DefaultValue")) {
            fixture->default_trait_definition = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_IMPL
            && cm_hir_def_id_is_none(fixture->impl_definition)) {
            fixture->impl_definition = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_STRUCT
            && intern_is(&fixture->hir, item->name, "Holder")) {
            fixture->holder_definition = item->definition;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && intern_is(&fixture->hir, item->name, "BoundFn")) {
            fixture->bound_function_pointer =
                item->data.type_alias_item.target;
        } else if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && intern_is(&fixture->hir, item->name, "value")) {
            if (item->data.function_item.body == CM_HIR_BODY_NONE) {
                fixture->declared_method = item->definition;
            } else {
                fixture->selected_method = item->definition;
            }
        } else if (item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
            && intern_is(&fixture->hir, item->name, "default_value")
            && item->data.function_item.body != CM_HIR_BODY_NONE) {
            fixture->default_method_definition = item->definition;
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
            } else if (type->data.integer_type.kind == CM_HIR_INT_USIZE) {
                usize_type = type_id;
            } else if (type->data.integer_type.kind == CM_HIR_INT_U128) {
                u128_type = type_id;
            }
        }
    }
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item;
        const CmHirType *self_type;

        item = (const CmHirItem *)cm_vec_at_const(&fixture->hir.items,
            index);
        self_type = item == NULL || item->kind != CM_HIR_ITEM_IMPL
                || !item->data.impl_item.has_trait
                || !cm_hir_def_id_equal(item->data.impl_item.trait_type
                    .definition, fixture->default_trait_definition)
            ? NULL : cm_hir_get_type(&fixture->hir,
                item->data.impl_item.self_type);
        if (self_type == NULL
            || self_type->kind != CM_HIR_TYPE_INTEGER_KIND) continue;
        if (self_type->data.integer_type.kind == CM_HIR_INT_U32) {
            fixture->default_u32_impl_definition = item->definition;
        } else if (self_type->data.integer_type.kind == CM_HIR_INT_USIZE) {
            fixture->default_usize_impl_definition = item->definition;
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
    memset(&holder_argument, 0, sizeof(holder_argument));
    holder_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    holder_argument.data.type = u32_type;
    memset(&type_value, 0, sizeof(type_value));
    type_value.kind = CM_HIR_TYPE_ADT_KIND;
    type_value.span.source = fixture->source;
    type_value.data.named_type.definition = fixture->holder_definition;
    type_value.data.named_type.arguments = &holder_argument;
    type_value.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&fixture->hir, &type_value,
        &fixture->holder_u32_type) == CM_HIR_OK);
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
        && !cm_hir_def_id_is_none(fixture->default_trait_definition)
        && !cm_hir_def_id_is_none(fixture->default_method_definition)
        && !cm_hir_def_id_is_none(fixture->default_u32_impl_definition)
        && !cm_hir_def_id_is_none(fixture->default_usize_impl_definition)
        && !cm_hir_def_id_is_none(fixture->holder_definition)
        && u32_type != CM_HIR_TYPE_NONE
        && usize_type != CM_HIR_TYPE_NONE
        && fixture->holder_u32_type != CM_HIR_TYPE_NONE
        && fixture->static_reference != CM_HIR_TYPE_NONE
        && fixture->bound_reference != CM_HIR_TYPE_NONE
        && fixture->array_128 != CM_HIR_TYPE_NONE);
    fixture->u32_type = u32_type;
    fixture->usize_type = usize_type;
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
    spec.body_definition = fixture->callable;
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
    assert(strstr(dump, "hir-instance-v2 crate=1") != NULL
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
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult result;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;
    const CmHirItem *selected;
    size_t index;

    fixture_init(&fixture);
    memset(&admission, 0, sizeof(admission));
    memset(&key, 0, sizeof(key));
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture.selected_method;
    spec.body_definition = fixture.selected_method;
    spec.declared_trait_callable = fixture.declared_method;
    spec.enclosing_impl = fixture.impl_definition;
    spec.implemented_trait = fixture.trait_definition;
    spec.self_owner = fixture.impl_definition;
    spec.self_type = fixture.u32_type;
    selected = NULL;
    for (index = 0u; index < fixture.hir.items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(&fixture.hir.items,
            index);
        if (candidate != NULL && cm_hir_def_id_equal(candidate->definition,
                fixture.selected_method)) {
            selected = candidate;
            break;
        }
    }
    assert(selected != NULL && selected->kind == CM_HIR_ITEM_FUNCTION
        && selected->data.function_item.body != CM_HIR_BODY_NONE);
    reachable.body = selected->data.function_item.body;
    reachable.spec = &spec;
    result = cm_semantic_admit_typed_leaf_instances(&admission,
        &fixture.hir, 1u, &reachable, 1u);
    assert(result.status == CM_SEMANTIC_ADMISSION_OK);
    assert(cm_hir_instance_key_init(&key, &admission, &spec)
        == CM_HIR_INSTANCE_OK);
    cm_hir_instance_key_destroy(&key);
    spec.body_definition = fixture.callable;
    assert(cm_hir_instance_key_init(&key, &admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    spec.body_definition = fixture.selected_method;
    spec.declared_trait_callable = fixture.callable;
    assert(cm_hir_instance_key_init(&key, &admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    spec.declared_trait_callable = fixture.declared_method;
    spec.self_owner = fixture.trait_definition;
    assert(cm_hir_instance_key_init(&key, &admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && key.state == NULL);
    cm_semantic_admission_destroy(&admission);
    fixture_destroy(&fixture);
}

static int canonical_bytes_are_borrowed(
    const CmHirCanonicalInstance *identity,
    const unsigned char *bytes, size_t size)
{
    size_t offset;

    if (identity == NULL || identity->bytes == NULL || bytes == NULL) return 0;
    for (offset = 0u; offset <= identity->size; ++offset) {
        if (bytes == identity->bytes + offset) {
            return size <= identity->size - offset;
        }
    }
    return 0;
}

static int canonical_argument_is_borrowed(
    const CmHirCanonicalInstance *identity,
    const CmHirCanonicalArgumentPart *argument)
{
    return argument != NULL && canonical_bytes_are_borrowed(identity,
        argument->bytes, argument->size);
}

static void test_canonical_instance_decode(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirCanonicalInstance identity;
    CmHirCanonicalInstance reencoded;
    CmHirDecodedCanonicalInstance decoded;
    int equal;

    fixture_init(&fixture);
    spec = make_spec(&fixture, arguments);
    cm_hir_canonical_instance_init(&identity);
    cm_hir_canonical_instance_init(&reencoded);
    cm_hir_decoded_canonical_instance_init(&decoded);
    spec.body_definition = cm_hir_def_id_none();
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_INVALID_RELATION
        && cm_hir_def_id_is_none(identity.definition)
        && cm_hir_def_id_is_none(identity.body_definition)
        && identity.body == CM_HIR_BODY_NONE && identity.bytes == NULL
        && identity.size == 0u);
    spec.body_definition = fixture.callable;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_decode(&fixture.hir, 1u, &identity,
            &decoded) == CM_HIR_INSTANCE_OK
        && cm_hir_def_id_equal(decoded.parts.selected_callable,
            fixture.callable)
        && cm_hir_def_id_equal(decoded.parts.body_definition,
            fixture.callable)
        && cm_hir_def_id_equal(identity.body_definition, fixture.callable)
        && decoded.parts.item_argument_count == 2u
        && decoded.parts.item_arguments == decoded.owned_item_arguments
        && decoded.parts.method_arguments == NULL
        && decoded.parts.enclosing_impl_arguments == NULL
        && decoded.parts.implemented_trait_arguments == NULL
        && decoded.parts.self_type == NULL
        && canonical_argument_is_borrowed(&identity,
            &decoded.parts.item_arguments[0])
        && canonical_argument_is_borrowed(&identity,
            &decoded.parts.item_arguments[1])
        && cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &decoded.parts, &reencoded) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&identity, &reencoded, &equal)
            == CM_HIR_INSTANCE_OK && equal);
    decoded.parts.body_definition = fixture.selected_method;
    assert(cm_hir_canonical_instance_encode_parts(&fixture.hir, 1u,
            &decoded.parts, &(CmHirCanonicalInstance){0})
        == CM_HIR_INSTANCE_INVALID_RELATION);
    decoded.parts.body_definition = fixture.callable;
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    assert(decoded.owned_item_arguments == NULL
        && decoded.parts.item_arguments == NULL
        && cm_hir_def_id_is_none(decoded.parts.selected_callable));
    cm_hir_canonical_instance_destroy(&reencoded);
    cm_hir_canonical_instance_destroy(&identity);

    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture.selected_method;
    spec.body_definition = fixture.selected_method;
    spec.declared_trait_callable = fixture.declared_method;
    spec.enclosing_impl = fixture.impl_definition;
    spec.implemented_trait = fixture.trait_definition;
    spec.self_owner = fixture.impl_definition;
    spec.self_type = fixture.u32_type;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_decode(&fixture.hir, 1u, &identity,
            &decoded) == CM_HIR_INSTANCE_OK
        && cm_hir_def_id_equal(decoded.parts.selected_callable,
            fixture.selected_method)
        && cm_hir_def_id_equal(decoded.parts.body_definition,
            fixture.selected_method)
        && cm_hir_def_id_equal(decoded.parts.declared_trait_callable,
            fixture.declared_method)
        && cm_hir_def_id_equal(decoded.parts.enclosing_impl,
            fixture.impl_definition)
        && cm_hir_def_id_equal(decoded.parts.implemented_trait,
            fixture.trait_definition)
        && cm_hir_def_id_equal(decoded.parts.self_owner,
            fixture.impl_definition)
        && decoded.parts.item_argument_count == 0u
        && decoded.parts.method_argument_count == 0u
        && decoded.parts.enclosing_impl_argument_count == 0u
        && decoded.parts.implemented_trait_argument_count == 0u
        && canonical_bytes_are_borrowed(&identity,
            decoded.parts.self_type, decoded.parts.self_type_size));
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    cm_hir_canonical_instance_destroy(&identity);
    fixture_destroy(&fixture);
}

static void test_canonical_instance_decode_rejects_malformed(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirCanonicalInstance identity;
    CmHirCanonicalInstance malformed;
    CmHirDecodedCanonicalInstance decoded;
    unsigned char *trailing_bytes;
    unsigned char saved;

    fixture_init(&fixture);
    spec = make_spec(&fixture, arguments);
    cm_hir_canonical_instance_init(&identity);
    cm_hir_decoded_canonical_instance_init(&decoded);
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
        &identity) == CM_HIR_INSTANCE_OK && identity.size > 1u);

    malformed = identity;
    malformed.size -= 1u;
    assert(cm_hir_canonical_instance_decode(&fixture.hir, 1u, &malformed,
            &decoded) != CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_validate(&fixture.hir, 1u, &malformed)
            != CM_HIR_INSTANCE_OK
        && cm_hir_def_id_is_none(decoded.parts.selected_callable));

    trailing_bytes = (unsigned char *)cm_alloc(identity.size + 1u);
    memcpy(trailing_bytes, identity.bytes, identity.size);
    trailing_bytes[identity.size] = 0u;
    malformed.bytes = trailing_bytes;
    malformed.size = identity.size + 1u;
    assert(cm_hir_canonical_instance_decode(&fixture.hir, 1u, &malformed,
            &decoded) == CM_HIR_INSTANCE_INVALID_RELATION
        && cm_hir_canonical_instance_validate(&fixture.hir, 1u, &malformed)
            == CM_HIR_INSTANCE_INVALID_RELATION
        && cm_hir_def_id_is_none(decoded.parts.selected_callable));
    cm_free(trailing_bytes);

    saved = identity.bytes[0];
    identity.bytes[0] ^= 1u;
    assert(cm_hir_canonical_instance_decode(&fixture.hir, 1u, &identity,
            &decoded) == CM_HIR_INSTANCE_INVALID_RELATION
        && cm_hir_canonical_instance_validate(&fixture.hir, 1u, &identity)
            == CM_HIR_INSTANCE_INVALID_RELATION
        && cm_hir_def_id_is_none(decoded.parts.selected_callable));
    identity.bytes[0] = saved;

    cm_hir_decoded_canonical_instance_destroy(&decoded);
    cm_hir_canonical_instance_destroy(&identity);
    fixture_destroy(&fixture);
}

static void test_inherited_default_instances_authenticate_dispatch(void)
{
    Fixture fixture;
    CmHirInstanceSpec spec;
    CmHirCanonicalInstance u32_identity;
    CmHirCanonicalInstance usize_identity;
    CmHirCanonicalInstance rejected;
    CmHirCanonicalInstance forged;
    CmHirDecodedCanonicalInstance decoded;
    CmHirInstanceKey key;
    int equal;

    fixture_init(&fixture);
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture.default_method_definition;
    spec.body_definition = fixture.default_method_definition;
    spec.declared_trait_callable = fixture.default_method_definition;
    spec.enclosing_impl = fixture.default_u32_impl_definition;
    spec.implemented_trait = fixture.default_trait_definition;
    spec.self_owner = fixture.default_u32_impl_definition;
    spec.self_type = fixture.u32_type;
    cm_hir_canonical_instance_init(&u32_identity);
    cm_hir_canonical_instance_init(&usize_identity);
    cm_hir_canonical_instance_init(&rejected);
    cm_hir_decoded_canonical_instance_init(&decoded);
    memset(&key, 0, sizeof(key));
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
            == CM_HIR_INSTANCE_INVALID_RELATION
        && key.state == NULL);
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &u32_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_decode(&fixture.hir, 1u,
            &u32_identity, &decoded) == CM_HIR_INSTANCE_OK
        && cm_hir_def_id_equal(decoded.parts.selected_callable,
            fixture.default_method_definition)
        && cm_hir_def_id_equal(decoded.parts.body_definition,
            fixture.default_method_definition)
        && cm_hir_def_id_equal(decoded.parts.enclosing_impl,
            fixture.default_u32_impl_definition)
        && cm_hir_def_id_equal(decoded.parts.self_owner,
            fixture.default_u32_impl_definition));
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    forged = u32_identity;
    forged.definition = fixture.selected_method;
    assert(cm_hir_canonical_instance_validate(&fixture.hir, 1u, &forged)
        == CM_HIR_INSTANCE_INVALID_RELATION);
    forged = u32_identity;
    forged.body_definition = fixture.callable;
    assert(cm_hir_canonical_instance_validate(&fixture.hir, 1u, &forged)
        == CM_HIR_INSTANCE_INVALID_RELATION);
    forged = u32_identity;
    forged.body = fixture.hir.bodies.len == 0u
        ? CM_HIR_BODY_NONE : (CmHirBodyId)fixture.hir.bodies.len;
    if (forged.body == u32_identity.body) forged.body = CM_HIR_BODY_NONE;
    assert(cm_hir_canonical_instance_validate(&fixture.hir, 1u, &forged)
        == CM_HIR_INSTANCE_INVALID_RELATION);

    spec.enclosing_impl = fixture.default_usize_impl_definition;
    spec.self_owner = fixture.default_usize_impl_definition;
    spec.self_type = fixture.usize_type;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &usize_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_decode(&fixture.hir, 1u,
            &usize_identity, &decoded) == CM_HIR_INSTANCE_OK
        && cm_hir_def_id_equal(decoded.parts.enclosing_impl,
            fixture.default_usize_impl_definition)
        && cm_hir_def_id_equal(decoded.parts.self_owner,
            fixture.default_usize_impl_definition)
        && u32_identity.body == usize_identity.body
        && cm_hir_canonical_instance_equal(&u32_identity, &usize_identity,
            &equal) == CM_HIR_INSTANCE_OK && !equal);
    cm_hir_decoded_canonical_instance_destroy(&decoded);

    spec.self_owner = fixture.default_u32_impl_definition;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);
    spec.self_owner = fixture.default_usize_impl_definition;
    spec.self_type = fixture.u32_type;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);
    spec.self_type = fixture.usize_type;
    spec.implemented_trait = fixture.trait_definition;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);
    spec.implemented_trait = fixture.default_trait_definition;
    spec.declared_trait_callable = fixture.declared_method;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);
    spec.declared_trait_callable = fixture.default_method_definition;
    spec.selected_callable = fixture.selected_method;
    spec.body_definition = fixture.selected_method;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);
    spec.selected_callable = fixture.default_method_definition;
    spec.body_definition = fixture.callable;
    assert(cm_hir_canonical_instance_encode(&fixture.hir, 1u, &spec,
            &rejected) == CM_HIR_INSTANCE_INVALID_RELATION);

    cm_hir_canonical_instance_destroy(&rejected);
    cm_hir_canonical_instance_destroy(&usize_identity);
    cm_hir_canonical_instance_destroy(&u32_identity);
    fixture_destroy(&fixture);
}

static void test_direct_call_parts_parity(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec caller_spec;
    CmHirCanonicalInstance caller_identity;
    CmHirCanonicalInstance from_spec;
    CmHirCanonicalInstance from_parts;
    CmHirDecodedCanonicalInstance decoded;
    const CmHirDefinition *definition;
    const CmHirItem *caller;
    CmHirExpr call;
    CmHirTypeId substitutions[2];
    int equal;

    fixture_init(&fixture);
    definition = cm_hir_lookup_definition(&fixture.hir,
        fixture.callable);
    caller = definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        ? NULL : cm_hir_get_item(&fixture.hir, definition->entity.item_id);
    caller_spec = make_spec(&fixture, arguments);
    memset(&call, 0, sizeof(call));
    if (caller != NULL && caller->kind == CM_HIR_ITEM_FUNCTION
        && caller->data.function_item.signature.parameter_count == 2u) {
        substitutions[0] = caller->data.function_item.signature.parameters[0]
            .type;
        substitutions[1] = caller->data.function_item.signature.parameters[1]
            .type;
        call.kind = CM_HIR_EXPR_CALL;
        call.owner_body = caller->data.function_item.body;
        call.data.call.callee = fixture.callable;
        call.data.call.type_substitutions = substitutions;
        call.data.call.type_substitution_count = 2u;
    }
    cm_hir_canonical_instance_init(&caller_identity);
    cm_hir_canonical_instance_init(&from_spec);
    cm_hir_canonical_instance_init(&from_parts);
    cm_hir_decoded_canonical_instance_init(&decoded);
    assert(caller != NULL && call.kind == CM_HIR_EXPR_CALL
        && call.data.call.type_substitution_count == 2u
        && cm_hir_def_id_equal(call.data.call.callee, fixture.callable)
        && cm_hir_canonical_instance_encode(&fixture.hir, 1u, &caller_spec,
            &caller_identity) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_decode(&fixture.hir, 1u,
            &caller_identity, &decoded) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode_direct_call(&fixture.hir, 1u,
            &caller_spec, &call, &from_spec) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_encode_direct_call_parts(&fixture.hir,
            1u, &decoded.parts, &call, &from_parts) == CM_HIR_INSTANCE_OK
        && cm_hir_canonical_instance_equal(&from_spec, &from_parts, &equal)
            == CM_HIR_INSTANCE_OK && equal
        && cm_hir_def_id_equal(from_parts.definition, fixture.callable)
        && cm_hir_def_id_equal(from_parts.body_definition,
            fixture.callable));
    decoded.parts.selected_callable = fixture.selected_method;
    assert(cm_hir_canonical_instance_encode_direct_call_parts(&fixture.hir,
            1u, &decoded.parts, &call, &(CmHirCanonicalInstance){0})
            == CM_HIR_INSTANCE_INVALID_RELATION);
    decoded.parts.selected_callable = fixture.callable;
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    cm_hir_canonical_instance_destroy(&from_parts);
    cm_hir_canonical_instance_destroy(&from_spec);
    cm_hir_canonical_instance_destroy(&caller_identity);
    fixture_destroy(&fixture);
}

static void test_adt_argument_kind_is_authenticated(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;
    CmHirInstanceKey rejected;
    CmHirType *holder_type;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    memset(&rejected, 0, sizeof(rejected));
    spec = make_spec(&fixture, arguments);
    arguments[0].data.type = fixture.holder_u32_type;
    assert(cm_hir_instance_key_init(&key, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_OK);

    holder_type = (CmHirType *)cm_hir_get_type(&fixture.hir,
        fixture.holder_u32_type);
    assert(holder_type != NULL && holder_type->kind == CM_HIR_TYPE_ADT_KIND
        && holder_type->data.named_type.argument_count == 1u
        && holder_type->data.named_type.arguments != NULL
        && holder_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    holder_type->data.named_type.arguments[0].kind =
        CM_HIR_GENERIC_ARG_LIFETIME;
    holder_type->data.named_type.arguments[0].data.lifetime.kind =
        CM_HIR_REGION_STATIC;
    assert(cm_hir_instance_key_init(&rejected, &fixture.admission, &spec)
        == CM_HIR_INSTANCE_INVALID_RELATION && rejected.state == NULL);

    cm_hir_instance_key_destroy(&key);
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

static void test_bound_function_pointer_fails_closed(void)
{
    Fixture fixture;
    CmHirGenericArg arguments[2];
    CmHirInstanceSpec spec;
    CmHirInstanceKey key;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    spec = make_spec(&fixture, arguments);
    arguments[0].data.type = fixture.bound_function_pointer;
    assert(fixture.bound_function_pointer != CM_HIR_TYPE_NONE
        && cm_hir_instance_key_init(&key, &fixture.admission, &spec)
            == CM_HIR_INSTANCE_UNSUPPORTED_REGION
        && key.state == NULL);
    cm_hir_instance_key_destroy(&key);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_structural_key_clone_compare_dump();
    test_authenticated_method_identity();
    test_canonical_instance_decode();
    test_canonical_instance_decode_rejects_malformed();
    test_inherited_default_instances_authenticate_dispatch();
    test_direct_call_parts_parity();
    test_adt_argument_kind_is_authenticated();
    test_fail_closed_and_stale();
    test_same_hir_foreign_admission();
    test_clone_oom_is_transactional();
    test_bound_function_pointer_fails_closed();
    puts("hir instance key tests passed");
    return 0;
}
