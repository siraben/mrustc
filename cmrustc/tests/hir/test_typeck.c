#include "cm/hir/typeck.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmTypeckContext typeck;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId definitions[6];
    CmHirGenericParamId parameters[2];
    CmHirTypeId hir_u32;
    CmHirTypeId hir_i32;
    CmHirTypeId hir_usize;
    CmHirTypeId hir_f64;
    CmHirTypeId hir_bool;
    CmTypeckTypeId u32_type;
    CmTypeckTypeId i32_type;
    CmTypeckTypeId usize_type;
    CmTypeckTypeId f64_type;
    CmTypeckTypeId bool_type;
} TestFixture;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirTypeId add_hir_scalar(CmHirContext *hir, CmHirTypeKind kind,
    unsigned int subtype)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(1u, 2u);
    if (kind == CM_HIR_TYPE_INTEGER_KIND) {
        type.data.integer_type.kind = (CmHirIntType)subtype;
    } else if (kind == CM_HIR_TYPE_FLOAT_KIND) {
        type.data.float_type.kind = (CmHirFloatType)subtype;
    }
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static void fixture_init(TestFixture *fixture)
{
    CmHirGenericParam parameter;
    CmHirItem trait_item;
    CmHirItemId trait_item_id;
    uint32_t index;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "typeck_test"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &fixture->crate_id,
        &fixture->root_module) == CM_HIR_OK);
    for (index = 0u; index < 5u; ++index) {
        assert(cm_hir_reserve_item_definition(&fixture->hir,
            fixture->crate_id, test_span(index + 1u, index + 2u),
            &fixture->definitions[index]) == CM_HIR_OK);
    }
    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(6u, 7u),
        &fixture->definitions[5]) == CM_HIR_OK);
    memset(&trait_item, 0, sizeof(trait_item));
    trait_item.kind = CM_HIR_ITEM_TRAIT;
    trait_item.definition = fixture->definitions[5];
    trait_item.owner_module = fixture->root_module;
    trait_item.parent_definition = cm_hir_def_id_none();
    trait_item.name = cm_hir_intern(&fixture->hir, "FixtureTrait");
    trait_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    trait_item.visibility.restriction = cm_hir_def_id_none();
    trait_item.span = test_span(6u, 7u);
    trait_item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &trait_item, &trait_item_id)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = fixture->definitions[0];
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(10u, 11u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->parameters[0]) == CM_HIR_OK);
    parameter.owner = fixture->definitions[1];
    parameter.name = cm_hir_intern(&fixture->hir, "U");
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &fixture->parameters[1]) == CM_HIR_OK);
    fixture->hir_u32 = add_hir_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, (unsigned int)CM_HIR_INT_U32);
    fixture->hir_i32 = add_hir_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, (unsigned int)CM_HIR_INT_I32);
    fixture->hir_usize = add_hir_scalar(&fixture->hir,
        CM_HIR_TYPE_INTEGER_KIND, (unsigned int)CM_HIR_INT_USIZE);
    fixture->hir_f64 = add_hir_scalar(&fixture->hir,
        CM_HIR_TYPE_FLOAT_KIND, (unsigned int)CM_HIR_FLOAT_F64);
    fixture->hir_bool = add_hir_scalar(&fixture->hir,
        CM_HIR_TYPE_BOOL_KIND, 0u);
    cm_typeck_context_init(&fixture->typeck, &fixture->hir);
    assert(cm_typeck_import_hir_type(&fixture->typeck, fixture->hir_u32,
        &fixture->u32_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&fixture->typeck, fixture->hir_i32,
        &fixture->i32_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&fixture->typeck, fixture->hir_usize,
        &fixture->usize_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&fixture->typeck, fixture->hir_f64,
        &fixture->f64_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&fixture->typeck, fixture->hir_bool,
        &fixture->bool_type) == CM_TYPECK_OK);
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_typeck_context_destroy(&fixture->typeck);
    cm_hir_context_destroy(&fixture->hir);
}

static CmTypeckTypeId new_variable(TestFixture *fixture,
    CmHirInferenceKind class_kind)
{
    CmTypeckTypeId id;

    assert(cm_typeck_new_variable(&fixture->typeck, class_kind,
        test_span(20u, 21u), &id) == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_tuple(TestFixture *fixture,
    CmTypeckTypeId *elements, uint32_t count)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_TUPLE;
    type.span = test_span(20u, 30u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = count;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_reference(TestFixture *fixture,
    CmTypeckTypeId pointee, CmHirMutability mutability)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_REFERENCE;
    type.span = test_span(20u, 30u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = mutability;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_reference_region(TestFixture *fixture,
    CmTypeckTypeId pointee, CmHirRegion region)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_REFERENCE;
    type.span = test_span(20u, 30u);
    type.data.reference_type.region = region;
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_array(TestFixture *fixture,
    CmTypeckTypeId element, uint64_t length)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_ARRAY;
    type.span = test_span(20u, 30u);
    type.data.array_type.element = element;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = fixture->usize_type;
    type.data.array_type.length.data.value.low_bits = length;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_named(TestFixture *fixture, CmHirDefId definition,
    CmTypeckGenericArg *arguments, uint32_t count)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_ADT;
    type.span = test_span(20u, 30u);
    type.data.named_type.definition = definition;
    type.data.named_type.arguments = arguments;
    type.data.named_type.argument_count = count;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_fn_pointer(TestFixture *fixture,
    CmTypeckTypeId *parameters, uint32_t parameter_count,
    CmTypeckTypeId return_type, const char *abi, CmHirSafety safety)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_FN_POINTER;
    type.span = test_span(20u, 30u);
    type.data.fn_pointer_type.parameters = parameters;
    type.data.fn_pointer_type.parameter_count = parameter_count;
    type.data.fn_pointer_type.return_type = return_type;
    type.data.fn_pointer_type.abi = cm_hir_intern(&fixture->hir, abi);
    type.data.fn_pointer_type.safety = safety;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_parameter(TestFixture *fixture,
    CmHirGenericParamId parameter)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_PARAMETER;
    type.span = test_span(20u, 30u);
    type.data.parameter_type.parameter = parameter;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static CmTypeckTypeId add_projection(TestFixture *fixture,
    CmTypeckTypeId self_type, CmHirDefId trait_definition,
    CmHirDefId associated_definition, CmTypeckGenericArg *trait_arguments,
    uint32_t trait_argument_count)
{
    CmTypeckType type;
    CmTypeckTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_TYPECK_TYPE_PROJECTION;
    type.span = test_span(20u, 30u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count =
        trait_argument_count;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_typeck_add_type(&fixture->typeck, &type, &id)
        == CM_TYPECK_OK);
    return id;
}

static void assert_unresolved(TestFixture *fixture, CmTypeckTypeId variable)
{
    CmTypeckTypeId resolved;

    assert(cm_typeck_resolve(&fixture->typeck, variable, &resolved)
        == CM_TYPECK_OK);
    assert(resolved == variable);
    assert(cm_typeck_get_type(&fixture->typeck, resolved)->kind
        == CM_TYPECK_TYPE_VARIABLE);
}

static void test_variable_classes_and_aliases(void)
{
    TestFixture fixture;
    CmTypeckTypeId general;
    CmTypeckTypeId integer;
    CmTypeckTypeId integer_other;
    CmTypeckTypeId floating;
    CmTypeckTypeId first;
    CmTypeckTypeId second;
    CmTypeckTypeId third;
    CmTypeckTypeId resolved;
    CmTypeckSnapshot snapshot;

    fixture_init(&fixture);
    general = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    assert(cm_typeck_unify(&fixture.typeck, general, fixture.u32_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_resolve(&fixture.typeck, general, &resolved)
        == CM_TYPECK_OK && resolved == fixture.u32_type);
    integer = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    assert(cm_typeck_unify(&fixture.typeck, integer, fixture.f64_type)
        == CM_TYPECK_KIND_CONFLICT);
    assert_unresolved(&fixture, integer);
    assert(cm_typeck_unify(&fixture.typeck, integer, fixture.u32_type)
        == CM_TYPECK_OK);
    integer_other = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    floating = new_variable(&fixture, CM_HIR_INFER_FLOAT);
    assert(cm_typeck_unify(&fixture.typeck, floating, integer_other)
        == CM_TYPECK_KIND_CONFLICT);
    assert_unresolved(&fixture, integer_other);
    assert_unresolved(&fixture, floating);

    first = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    second = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    third = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    assert(cm_typeck_unify(&fixture.typeck, third, second) == CM_TYPECK_OK);
    assert(cm_typeck_unify(&fixture.typeck, second, first) == CM_TYPECK_OK);
    assert(cm_typeck_resolve(&fixture.typeck, third, &resolved)
        == CM_TYPECK_OK && resolved == first);

    general = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    integer = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    assert(cm_typeck_snapshot(&fixture.typeck, &snapshot) == CM_TYPECK_OK);
    assert(cm_typeck_unify(&fixture.typeck, general, integer)
        == CM_TYPECK_OK);
    assert(cm_typeck_resolve(&fixture.typeck, general, &resolved)
        == CM_TYPECK_OK && resolved == general);
    assert(cm_typeck_get_type(&fixture.typeck, resolved)->data.variable
        .class_kind == CM_HIR_INFER_INTEGER);
    assert(cm_typeck_rollback(&fixture.typeck, &snapshot) == CM_TYPECK_OK);
    assert(cm_typeck_get_type(&fixture.typeck, general)->data.variable
            .class_kind == CM_HIR_INFER_GENERAL
        && cm_typeck_get_type(&fixture.typeck, integer)->data.variable
            .class_kind == CM_HIR_INFER_INTEGER);
    fixture_destroy(&fixture);
}

static void test_balanced_deterministic_variable_roots(void)
{
    TestFixture fixture;
    CmTypeckTypeId variables[1024];
    CmTypeckTypeId resolved;
    uint32_t index;

    fixture_init(&fixture);
    for (index = 0u; index < 1024u; ++index) {
        variables[index] = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    }
    for (index = 1023u; index != 0u; --index) {
        assert(cm_typeck_unify(&fixture.typeck, variables[index],
            variables[index - 1u]) == CM_TYPECK_OK);
    }
    for (index = 0u; index < 1024u; ++index) {
        assert(cm_typeck_resolve(&fixture.typeck, variables[index],
            &resolved) == CM_TYPECK_OK && resolved == variables[0]);
    }
    fixture_destroy(&fixture);
}

static void test_transactional_numeric_defaulting(void)
{
    TestFixture fixture;
    CmTypeckTypeId first;
    CmTypeckTypeId second;
    CmTypeckTypeId constrained;
    CmTypeckTypeId general;
    CmTypeckTypeId resolved;
    size_t count;

    fixture_init(&fixture);
    first = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    second = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    constrained = new_variable(&fixture, CM_HIR_INFER_INTEGER);
    general = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    assert(cm_typeck_unify(&fixture.typeck, second, first)
        == CM_TYPECK_OK);
    assert(cm_typeck_unify(&fixture.typeck, constrained, fixture.u32_type)
        == CM_TYPECK_OK);
    count = 99u;
    assert(cm_typeck_default_unresolved(&fixture.typeck,
        CM_HIR_INFER_INTEGER, fixture.hir_i32, &count) == CM_TYPECK_OK
        && count == 1u);
    assert(cm_typeck_resolve(&fixture.typeck, first, &resolved)
            == CM_TYPECK_OK
        && resolved == fixture.i32_type);
    assert(cm_typeck_resolve(&fixture.typeck, second, &resolved)
            == CM_TYPECK_OK
        && resolved == fixture.i32_type);
    assert(cm_typeck_resolve(&fixture.typeck, constrained, &resolved)
            == CM_TYPECK_OK
        && resolved == fixture.u32_type);
    assert_unresolved(&fixture, general);

    count = 99u;
    assert(cm_typeck_default_unresolved(&fixture.typeck,
        CM_HIR_INFER_INTEGER, fixture.hir_f64, &count)
        == CM_TYPECK_KIND_CONFLICT && count == 0u);
    count = 99u;
    assert(cm_typeck_default_unresolved(&fixture.typeck,
        CM_HIR_INFER_INTEGER, CM_HIR_TYPE_NONE, &count)
        == CM_TYPECK_INVALID_ARGUMENT && count == 0u);
    count = 99u;
    assert(cm_typeck_default_unresolved(&fixture.typeck,
        CM_HIR_INFER_INTEGER, fixture.hir_i32, &count) == CM_TYPECK_OK
        && count == 0u);
    fixture_destroy(&fixture);
}

static void test_region_erased_unification(void)
{
    TestFixture fixture;
    CmHirGenericParam lifetime;
    CmHirGenericParamId first_lifetime;
    CmHirGenericParamId second_lifetime;
    CmHirRegion first_region;
    CmHirRegion second_region;
    CmTypeckTypeId first_reference;
    CmTypeckTypeId second_reference;
    CmTypeckGenericArg first_argument;
    CmTypeckGenericArg second_argument;
    CmTypeckTypeId first_named;
    CmTypeckTypeId second_named;

    fixture_init(&fixture);
    memset(&lifetime, 0, sizeof(lifetime));
    lifetime.kind = CM_HIR_GENERIC_LIFETIME;
    lifetime.owner = fixture.definitions[2];
    lifetime.name = cm_hir_intern(&fixture.hir, "a");
    lifetime.span = test_span(21u, 22u);
    assert(cm_hir_add_generic_param(&fixture.hir, &lifetime,
        &first_lifetime) == CM_HIR_OK);
    lifetime.owner = fixture.definitions[3];
    lifetime.name = cm_hir_intern(&fixture.hir, "b");
    assert(cm_hir_add_generic_param(&fixture.hir, &lifetime,
        &second_lifetime) == CM_HIR_OK);
    memset(&first_region, 0, sizeof(first_region));
    first_region.kind = CM_HIR_REGION_EARLY_BOUND;
    first_region.data.parameter = first_lifetime;
    memset(&second_region, 0, sizeof(second_region));
    second_region.kind = CM_HIR_REGION_EARLY_BOUND;
    second_region.data.parameter = second_lifetime;
    first_reference = add_reference_region(&fixture, fixture.u32_type,
        first_region);
    second_reference = add_reference_region(&fixture, fixture.u32_type,
        second_region);
    assert(cm_typeck_unify(&fixture.typeck, first_reference,
        second_reference) == CM_TYPECK_OK);
    memset(&first_argument, 0, sizeof(first_argument));
    memset(&second_argument, 0, sizeof(second_argument));
    first_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    first_argument.data.lifetime = first_region;
    second_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    second_argument.data.lifetime = second_region;
    first_named = add_named(&fixture, fixture.definitions[4],
        &first_argument, 1u);
    second_named = add_named(&fixture, fixture.definitions[4],
        &second_argument, 1u);
    assert(cm_typeck_unify(&fixture.typeck, first_named, second_named)
        == CM_TYPECK_OK);
    fixture_destroy(&fixture);
}

static void test_structural_unification_and_rollback(void)
{
    TestFixture fixture;
    CmTypeckTypeId left_elements[2];
    CmTypeckTypeId right_elements[2];
    CmTypeckTypeId left;
    CmTypeckTypeId right;
    CmTypeckTypeId variable;
    CmTypeckGenericArg left_args[2];
    CmTypeckGenericArg right_args[2];
    CmTypeckTypeId named_left;
    CmTypeckTypeId named_right;
    CmTypeckTypeId array_left;
    CmTypeckTypeId array_right;

    fixture_init(&fixture);
    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    left_elements[0] = add_reference(&fixture, variable, CM_HIR_IMMUTABLE);
    left_elements[1] = fixture.bool_type;
    right_elements[0] = add_reference(&fixture, fixture.u32_type,
        CM_HIR_IMMUTABLE);
    right_elements[1] = fixture.bool_type;
    left = add_tuple(&fixture, left_elements, 2u);
    right = add_tuple(&fixture, right_elements, 2u);
    assert(cm_typeck_unify(&fixture.typeck, left, right) == CM_TYPECK_OK);
    assert(cm_typeck_resolve(&fixture.typeck, variable, &left)
        == CM_TYPECK_OK && left == fixture.u32_type);

    memset(left_args, 0, sizeof(left_args));
    memset(right_args, 0, sizeof(right_args));
    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    left_args[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    left_args[0].data.type = variable;
    left_args[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    left_args[1].data.type = fixture.bool_type;
    right_args[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    right_args[0].data.type = fixture.u32_type;
    right_args[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    right_args[1].data.type = fixture.f64_type;
    named_left = add_named(&fixture, fixture.definitions[2], left_args, 2u);
    named_right = add_named(&fixture, fixture.definitions[2], right_args, 2u);
    assert(cm_typeck_unify(&fixture.typeck, named_left, named_right)
        == CM_TYPECK_TYPE_MISMATCH);
    assert_unresolved(&fixture, variable);
    named_right = add_named(&fixture, fixture.definitions[3], left_args, 2u);
    assert(cm_typeck_unify(&fixture.typeck, named_left, named_right)
        == CM_TYPECK_TYPE_MISMATCH);

    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    array_left = add_array(&fixture, variable, UINT64_C(3));
    array_right = add_array(&fixture, fixture.u32_type, UINT64_C(4));
    assert(cm_typeck_unify(&fixture.typeck, array_left, array_right)
        == CM_TYPECK_TYPE_MISMATCH);
    assert_unresolved(&fixture, variable);
    fixture_destroy(&fixture);
}

static void test_functions_parameters_and_projections(void)
{
    TestFixture fixture;
    CmTypeckTypeId parameters[1];
    CmTypeckTypeId function_rust;
    CmTypeckTypeId function_c;
    CmTypeckTypeId function_unsafe;
    CmTypeckTypeId parameter_a;
    CmTypeckTypeId parameter_b;
    CmTypeckGenericArg args_a[1];
    CmTypeckGenericArg args_b[1];
    CmTypeckTypeId projection_a;
    CmTypeckTypeId projection_b;

    fixture_init(&fixture);
    parameters[0] = fixture.u32_type;
    function_rust = add_fn_pointer(&fixture, parameters, 1u,
        fixture.bool_type, "Rust", CM_HIR_SAFE);
    function_c = add_fn_pointer(&fixture, parameters, 1u,
        fixture.bool_type, "C", CM_HIR_SAFE);
    function_unsafe = add_fn_pointer(&fixture, parameters, 1u,
        fixture.bool_type, "Rust", CM_HIR_UNSAFE);
    assert(cm_typeck_unify(&fixture.typeck, function_rust, function_c)
        == CM_TYPECK_TYPE_MISMATCH);
    assert(cm_typeck_unify(&fixture.typeck, function_rust, function_unsafe)
        == CM_TYPECK_TYPE_MISMATCH);

    parameter_a = add_parameter(&fixture, fixture.parameters[0]);
    parameter_b = add_parameter(&fixture, fixture.parameters[1]);
    assert(cm_typeck_unify(&fixture.typeck, parameter_a, parameter_a)
        == CM_TYPECK_OK);
    assert(cm_typeck_unify(&fixture.typeck, parameter_a, parameter_b)
        == CM_TYPECK_TYPE_MISMATCH);

    memset(args_a, 0, sizeof(args_a));
    memset(args_b, 0, sizeof(args_b));
    args_a[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    args_a[0].data.type = fixture.u32_type;
    args_b[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    args_b[0].data.type = fixture.bool_type;
    projection_a = add_projection(&fixture, parameter_a,
        fixture.definitions[2], fixture.definitions[3], args_a, 1u);
    projection_b = add_projection(&fixture, parameter_a,
        fixture.definitions[2], fixture.definitions[3], args_b, 1u);
    assert(cm_typeck_unify(&fixture.typeck, projection_a, projection_b)
        == CM_TYPECK_TYPE_MISMATCH);
    projection_b = add_projection(&fixture, parameter_a,
        fixture.definitions[2], fixture.definitions[4], args_a, 1u);
    assert(cm_typeck_unify(&fixture.typeck, projection_a, projection_b)
        == CM_TYPECK_TYPE_MISMATCH);
    fixture_destroy(&fixture);
}

static void test_occurs_and_snapshots(void)
{
    TestFixture fixture;
    CmTypeckTypeId variable;
    CmTypeckTypeId elements[1];
    CmTypeckTypeId tuple;
    CmTypeckTypeId added_variable;
    CmTypeckSnapshot outer;
    CmTypeckSnapshot inner;
    size_t count;

    fixture_init(&fixture);
    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    elements[0] = variable;
    tuple = add_tuple(&fixture, elements, 1u);
    assert(cm_typeck_unify(&fixture.typeck, variable, tuple)
        == CM_TYPECK_OCCURS_CHECK);
    assert_unresolved(&fixture, variable);
    assert(cm_typeck_unify(&fixture.typeck, variable,
        add_reference(&fixture, variable, CM_HIR_IMMUTABLE))
        == CM_TYPECK_OCCURS_CHECK);

    count = cm_typeck_type_count(&fixture.typeck);
    assert(cm_typeck_snapshot(&fixture.typeck, &outer) == CM_TYPECK_OK);
    added_variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    assert(cm_typeck_unify(&fixture.typeck, variable, fixture.u32_type)
        == CM_TYPECK_OK);
    assert(cm_typeck_snapshot(&fixture.typeck, &inner) == CM_TYPECK_OK);
    (void)add_reference(&fixture, added_variable, CM_HIR_MUTABLE);
    assert(cm_typeck_commit(&fixture.typeck, &outer)
        == CM_TYPECK_INVALID_SNAPSHOT);
    assert(cm_typeck_rollback(&fixture.typeck, &inner) == CM_TYPECK_OK);
    assert(cm_typeck_rollback(&fixture.typeck, &outer) == CM_TYPECK_OK);
    assert(cm_typeck_type_count(&fixture.typeck) == count);
    assert(cm_typeck_get_type(&fixture.typeck, added_variable) == NULL);
    assert_unresolved(&fixture, variable);
    assert(cm_typeck_rollback(&fixture.typeck, &outer)
        == CM_TYPECK_INVALID_SNAPSHOT);
    fixture_destroy(&fixture);
}

static void test_deep_finite_occurs_check(void)
{
    TestFixture fixture;
    CmTypeckTypeId variable;
    CmTypeckTypeId current;
    CmTypeckTypeId other;
    CmTypeckTypeId duplicated[2];
    CmTypeckTypeId other_duplicated[2];
    CmTypeckGenericArg argument;
    uint32_t index;

    fixture_init(&fixture);
    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    current = fixture.u32_type;
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    for (index = 0u; index < 16u; ++index) {
        argument.data.type = current;
        current = add_named(&fixture, fixture.definitions[2], &argument, 1u);
    }
    assert(cm_typeck_unify(&fixture.typeck, variable, current)
        == CM_TYPECK_OK);

    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    current = fixture.u32_type;
    for (index = 0u; index < 16u; ++index) {
        argument.data.type = current;
        current = add_projection(&fixture, fixture.bool_type,
            fixture.definitions[2], fixture.definitions[3], &argument, 1u);
    }
    assert(cm_typeck_unify(&fixture.typeck, variable, current)
        == CM_TYPECK_OK);

    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    current = fixture.u32_type;
    for (index = 0u; index < 40u; ++index) {
        duplicated[0] = current;
        duplicated[1] = current;
        current = add_tuple(&fixture, duplicated, 2u);
    }
    assert(cm_typeck_unify(&fixture.typeck, variable, current)
        == CM_TYPECK_OK);

    current = fixture.u32_type;
    other = fixture.u32_type;
    for (index = 0u; index < 40u; ++index) {
        duplicated[0] = current;
        duplicated[1] = current;
        current = add_tuple(&fixture, duplicated, 2u);
        other_duplicated[0] = other;
        other_duplicated[1] = other;
        other = add_tuple(&fixture, other_duplicated, 2u);
    }
    assert(cm_typeck_unify(&fixture.typeck, current, other)
        == CM_TYPECK_OK);
    fixture_destroy(&fixture);
}

static void test_hir_rewind_invalidates_session(void)
{
    TestFixture fixture;
    CmHirContextMark rewind_mark;
    CmHirContextMark caller_mark;
    CmHirType type;
    CmHirTypeId original;
    CmHirTypeId reused;
    CmTypeckTypeId imported;
    CmTypeckFreezeResult result;

    fixture_init(&fixture);
    assert(cm_hir_context_mark(&fixture.hir, &rewind_mark) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_UNIT_KIND;
    type.span = test_span(31u, 32u);
    assert(cm_hir_add_type(&fixture.hir, &type, &original) == CM_HIR_OK);
    assert(cm_typeck_import_hir_type(&fixture.typeck, original, &imported)
        == CM_TYPECK_OK);
    assert(cm_hir_context_rewind(&fixture.hir, &rewind_mark) == CM_HIR_OK);

    type.kind = CM_HIR_TYPE_BOOL_KIND;
    assert(cm_hir_add_type(&fixture.hir, &type, &reused) == CM_HIR_OK);
    assert(reused == original);
    assert(cm_hir_context_mark(&fixture.hir, &caller_mark) == CM_HIR_OK);
    result = cm_typeck_freeze_hir_type(&fixture.typeck, imported,
        &fixture.hir, &caller_mark);
    assert(result.status == CM_TYPECK_INVALID_ARGUMENT
        && result.type == CM_HIR_TYPE_NONE
        && cm_hir_get_type(&fixture.hir, reused)->kind
            == CM_HIR_TYPE_BOOL_KIND);
    assert(cm_hir_context_rewind(&fixture.hir, &caller_mark) == CM_HIR_OK);
    fixture_destroy(&fixture);
}

static void test_hir_reinit_invalidates_session(void)
{
    CmHirContext hir;
    CmTypeckContext typeck;
    CmHirContextMark mark;
    CmHirType type;
    CmHirTypeId hir_type;
    CmTypeckTypeId imported;
    CmTypeckFreezeResult result;

    cm_hir_context_init(&hir);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_UNIT_KIND;
    type.span = test_span(33u, 34u);
    assert(cm_hir_add_type(&hir, &type, &hir_type) == CM_HIR_OK);
    cm_typeck_context_init(&typeck, &hir);
    assert(cm_typeck_import_hir_type(&typeck, hir_type, &imported)
        == CM_TYPECK_OK);
    cm_hir_context_destroy(&hir);
    cm_hir_context_init(&hir);
    assert(cm_hir_context_mark(&hir, &mark) == CM_HIR_OK);
    result = cm_typeck_freeze_hir_type(&typeck, imported, &hir, &mark);
    assert(result.status == CM_TYPECK_INVALID_ARGUMENT
        && result.type == CM_HIR_TYPE_NONE);
    assert(cm_hir_context_rewind(&hir, &mark) == CM_HIR_OK);
    cm_typeck_context_destroy(&typeck);
    cm_hir_context_destroy(&hir);
}

static void test_recursion_limits_fail_closed(void)
{
    TestFixture fixture;
    CmHirType hir_type_value;
    CmHirTypeId deep_hir;
    CmTypeckTypeId imported;
    CmTypeckTypeId left;
    CmTypeckTypeId right;
    CmHirContextMark mark;
    CmTypeckFreezeResult result;
    size_t hir_count;
    size_t typeck_count;
    uint32_t index;

    fixture_init(&fixture);
    deep_hir = fixture.hir_u32;
    memset(&hir_type_value, 0, sizeof(hir_type_value));
    hir_type_value.kind = CM_HIR_TYPE_REFERENCE_KIND;
    hir_type_value.span = test_span(35u, 36u);
    hir_type_value.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    hir_type_value.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    for (index = 0u; index < 300u; ++index) {
        hir_type_value.data.reference_type.pointee = deep_hir;
        assert(cm_hir_add_type(&fixture.hir, &hir_type_value, &deep_hir)
            == CM_HIR_OK);
    }
    typeck_count = cm_typeck_type_count(&fixture.typeck);
    assert(cm_typeck_import_hir_type(&fixture.typeck, deep_hir, &imported)
        == CM_TYPECK_OVERFLOW);
    assert(imported == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == typeck_count);

    left = fixture.u32_type;
    right = fixture.u32_type;
    for (index = 0u; index < 300u; ++index) {
        left = add_reference(&fixture, left, CM_HIR_IMMUTABLE);
        right = add_reference(&fixture, right, CM_HIR_IMMUTABLE);
    }
    assert(cm_typeck_unify(&fixture.typeck, left, right)
        == CM_TYPECK_OVERFLOW);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    hir_count = fixture.hir.types.len;
    result = cm_typeck_freeze_hir_type(&fixture.typeck, left,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_OVERFLOW
        && result.type == CM_HIR_TYPE_NONE
        && fixture.hir.types.len == hir_count && mark.active);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    fixture_destroy(&fixture);
}

static void test_freeze_transactions(void)
{
    TestFixture fixture;
    CmTypeckTypeId variable;
    CmTypeckTypeId elements[2];
    CmTypeckTypeId tuple;
    CmTypeckTypeId bad_named;
    CmTypeckGenericArg bad_argument;
    CmTypeckFreezeResult result;
    CmHirContextMark mark;
    CmHirContext other;
    CmHirContextMark other_mark;
    CmHirDefId bad_definition;
    size_t count;

    fixture_init(&fixture);
    variable = new_variable(&fixture, CM_HIR_INFER_GENERAL);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    count = fixture.hir.types.len;
    result = cm_typeck_freeze_hir_type(&fixture.typeck, variable,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_UNRESOLVED
        && result.type == CM_HIR_TYPE_NONE
        && fixture.hir.types.len == count && mark.active);

    elements[0] = add_reference(&fixture, variable, CM_HIR_IMMUTABLE);
    elements[1] = fixture.bool_type;
    tuple = add_tuple(&fixture, elements, 2u);
    assert(cm_typeck_unify(&fixture.typeck, variable, fixture.u32_type)
        == CM_TYPECK_OK);
    result = cm_typeck_freeze_hir_type(&fixture.typeck, tuple,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_OK && result.added_type_count == 2u);
    assert(cm_hir_get_type(&fixture.hir, result.type)->kind
        == CM_HIR_TYPE_TUPLE_KIND);
    assert(cm_hir_context_commit(&fixture.hir, &mark) == CM_HIR_OK);

    result = cm_typeck_freeze_hir_type(&fixture.typeck, fixture.u32_type,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_INVALID_ARGUMENT);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    result = cm_typeck_freeze_hir_type(&fixture.typeck, fixture.u32_type,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_OK && result.type == fixture.hir_u32
        && result.added_type_count == 0u);
    assert(cm_hir_context_commit(&fixture.hir, &mark) == CM_HIR_OK);

    cm_hir_context_init(&other);
    assert(cm_hir_context_mark(&other, &other_mark) == CM_HIR_OK);
    result = cm_typeck_freeze_hir_type(&fixture.typeck, fixture.u32_type,
        &other, &other_mark);
    assert(result.status == CM_TYPECK_INVALID_ARGUMENT);
    assert(cm_hir_context_rewind(&other, &other_mark) == CM_HIR_OK);
    cm_hir_context_destroy(&other);

    bad_definition.crate_id = fixture.crate_id;
    bad_definition.index = UINT32_MAX;
    memset(&bad_argument, 0, sizeof(bad_argument));
    bad_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    bad_argument.data.type = add_reference(&fixture, fixture.u32_type,
        CM_HIR_IMMUTABLE);
    bad_named = add_named(&fixture, bad_definition, &bad_argument, 1u);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    count = fixture.hir.types.len;
    result = cm_typeck_freeze_hir_type(&fixture.typeck, bad_named,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_HIR_FAILURE
        && fixture.hir.types.len == count && mark.active);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    fixture_destroy(&fixture);
}

static CmHirTypeId add_unsupported_hir_type(TestFixture *fixture,
    CmHirTypeKind kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(40u, 41u);
    if (kind == CM_HIR_TYPE_ERROR_KIND) {
        type.data.error_type.reason = cm_hir_intern(&fixture->hir, "error");
    } else if (kind == CM_HIR_TYPE_INFER_KIND) {
        type.data.infer_type.kind = CM_HIR_INFER_GENERAL;
        type.data.infer_type.variable = 1u;
    } else if (kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
        type.data.named_type.definition = fixture->definitions[2];
    } else if (kind == CM_HIR_TYPE_DYN_TRAIT_KIND) {
        type.data.dyn_trait_type.has_principal = 1;
        type.data.dyn_trait_type.principal_trait.definition =
            fixture->definitions[5];
        type.data.dyn_trait_type.region.kind = CM_HIR_REGION_STATIC;
    }
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static void test_import_rejection_and_complex_reuse(void)
{
    TestFixture fixture;
    CmHirTypeKind unsupported[4];
    CmHirType type;
    CmHirTypeId hir_type;
    CmHirTypeId tuple_elements[7];
    CmHirTypeId fn_parameters[1];
    CmHirGenericArg named_arguments[1];
    CmTypeckTypeId imported;
    CmTypeckFreezeResult result;
    CmHirContextMark mark;
    size_t count;
    uint32_t index;

    fixture_init(&fixture);
    unsupported[0] = CM_HIR_TYPE_ERROR_KIND;
    unsupported[1] = CM_HIR_TYPE_INFER_KIND;
    unsupported[2] = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    unsupported[3] = CM_HIR_TYPE_DYN_TRAIT_KIND;
    for (index = 0u; index < 4u; ++index) {
        hir_type = add_unsupported_hir_type(&fixture, unsupported[index]);
        count = cm_typeck_type_count(&fixture.typeck);
        assert(cm_typeck_import_hir_type(&fixture.typeck, hir_type, &imported)
            == CM_TYPECK_UNSUPPORTED_HIR_TYPE);
        assert(imported == CM_TYPECK_TYPE_NONE
            && cm_typeck_type_count(&fixture.typeck) == count);
    }

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(40u, 41u);
    type.data.array_type.element = fixture.hir_u32;
    type.data.array_type.length.kind = CM_HIR_CONST_INFER;
    type.data.array_type.length.type = fixture.hir_usize;
    type.data.array_type.length.data.inference_variable = 1u;
    assert(cm_hir_add_type(&fixture.hir, &type, &hir_type) == CM_HIR_OK);
    count = cm_typeck_type_count(&fixture.typeck);
    assert(cm_typeck_import_hir_type(&fixture.typeck, hir_type, &imported)
        == CM_TYPECK_UNSUPPORTED_CONSTANT);
    assert(cm_typeck_type_count(&fixture.typeck) == count);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(40u, 41u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = fixture.hir_u32;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[0])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = test_span(40u, 41u);
    type.data.raw_pointer_type.pointee = fixture.hir_u32;
    type.data.raw_pointer_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[1])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SLICE_KIND;
    type.span = test_span(40u, 41u);
    type.data.slice_type.element = fixture.hir_bool;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[2])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(40u, 41u);
    type.data.array_type.element = fixture.hir_bool;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = fixture.hir_usize;
    type.data.array_type.length.data.value.low_bits = UINT64_C(3);
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[3])
        == CM_HIR_OK);
    fn_parameters[0] = fixture.hir_u32;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_FN_POINTER_KIND;
    type.span = test_span(40u, 41u);
    type.data.fn_pointer_type.parameters = fn_parameters;
    type.data.fn_pointer_type.parameter_count = 1u;
    type.data.fn_pointer_type.return_type = fixture.hir_bool;
    type.data.fn_pointer_type.abi = cm_hir_intern(&fixture.hir, "Rust");
    type.data.fn_pointer_type.safety = CM_HIR_SAFE;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[4])
        == CM_HIR_OK);
    memset(named_arguments, 0, sizeof(named_arguments));
    named_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    named_arguments[0].data.type = fixture.hir_u32;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(40u, 41u);
    type.data.named_type.definition = fixture.definitions[0];
    type.data.named_type.arguments = named_arguments;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[5])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(40u, 41u);
    type.data.parameter_type.parameter = fixture.parameters[0];
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_elements[6])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(40u, 41u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 7u;
    assert(cm_hir_add_type(&fixture.hir, &type, &hir_type) == CM_HIR_OK);
    assert(cm_typeck_import_hir_type(&fixture.typeck, hir_type, &imported)
        == CM_TYPECK_OK);
    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    result = cm_typeck_freeze_hir_type(&fixture.typeck, imported,
        &fixture.hir, &mark);
    assert(result.status == CM_TYPECK_OK && result.type == hir_type
        && result.added_type_count == 0u);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    fixture_destroy(&fixture);
}

static void test_transactional_hir_instantiation(void)
{
    TestFixture fixture;
    CmHirGenericParam generic;
    CmHirGenericParamId lifetime_parameter;
    CmHirGenericParamId const_parameter;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirTypeId foreign_parameter_type;
    CmHirTypeId self_type;
    CmHirTypeId reference_type;
    CmHirTypeId array_type;
    CmHirTypeId named_type;
    CmHirTypeId tuple_type;
    CmHirTypeId elements[6];
    CmHirGenericArg named_arguments[3];
    CmTypeckGenericArg arguments[3];
    CmTypeckInstantiation instantiation;
    CmTypeckNamedType instantiated_named;
    CmTypeckTypeId instantiated;
    const CmTypeckType *scratch;

    fixture_init(&fixture);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_LIFETIME;
    generic.owner = fixture.definitions[0];
    generic.index = 1u;
    generic.name = cm_hir_intern(&fixture.hir, "a");
    generic.span = test_span(50u, 51u);
    assert(cm_hir_add_generic_param(&fixture.hir, &generic,
        &lifetime_parameter) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_CONST;
    generic.owner = fixture.definitions[0];
    generic.index = 2u;
    generic.name = cm_hir_intern(&fixture.hir, "N");
    generic.span = test_span(51u, 52u);
    generic.declared_type = fixture.hir_usize;
    assert(cm_hir_add_generic_param(&fixture.hir, &generic,
        &const_parameter) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(52u, 53u);
    type.data.parameter_type.parameter = fixture.parameters[0];
    assert(cm_hir_add_type(&fixture.hir, &type, &parameter_type)
        == CM_HIR_OK);
    type.data.parameter_type.parameter = fixture.parameters[1];
    assert(cm_hir_add_type(&fixture.hir, &type, &foreign_parameter_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(52u, 53u);
    type.data.self_type.owner = fixture.definitions[5];
    assert(cm_hir_add_type(&fixture.hir, &type, &self_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(52u, 53u);
    type.data.reference_type.region.kind = CM_HIR_REGION_EARLY_BOUND;
    type.data.reference_type.region.data.parameter = lifetime_parameter;
    type.data.reference_type.pointee = parameter_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&fixture.hir, &type, &reference_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(52u, 53u);
    type.data.array_type.element = parameter_type;
    type.data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
    type.data.array_type.length.type = fixture.hir_usize;
    type.data.array_type.length.data.parameter = const_parameter;
    assert(cm_hir_add_type(&fixture.hir, &type, &array_type) == CM_HIR_OK);

    memset(named_arguments, 0, sizeof(named_arguments));
    named_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    named_arguments[0].data.lifetime.kind = CM_HIR_REGION_EARLY_BOUND;
    named_arguments[0].data.lifetime.data.parameter = lifetime_parameter;
    named_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    named_arguments[1].data.type = parameter_type;
    named_arguments[2].kind = CM_HIR_GENERIC_ARG_CONST;
    named_arguments[2].data.constant.kind = CM_HIR_CONST_PARAMETER;
    named_arguments[2].data.constant.type = fixture.hir_usize;
    named_arguments[2].data.constant.data.parameter = const_parameter;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(52u, 53u);
    type.data.named_type.definition = fixture.definitions[2];
    type.data.named_type.arguments = named_arguments;
    type.data.named_type.argument_count = 3u;
    assert(cm_hir_add_type(&fixture.hir, &type, &named_type) == CM_HIR_OK);

    elements[0] = parameter_type;
    elements[1] = foreign_parameter_type;
    elements[2] = self_type;
    elements[3] = reference_type;
    elements[4] = array_type;
    elements[5] = named_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(52u, 53u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 6u;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_type) == CM_HIR_OK);

    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = fixture.u32_type;
    arguments[1].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    arguments[1].data.lifetime.kind = CM_HIR_REGION_STATIC;
    arguments[2].kind = CM_HIR_GENERIC_ARG_CONST;
    arguments[2].data.constant.kind = CM_HIR_CONST_VALUE;
    arguments[2].data.constant.type = fixture.usize_type;
    arguments[2].data.constant.data.value.low_bits = UINT64_C(9);
    cm_typeck_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.parameter_owner = fixture.definitions[0];
    instantiation.arguments = arguments;
    instantiation.argument_count = 3u;
    instantiation.self_owner = fixture.definitions[5];
    instantiation.self_type = fixture.bool_type;

    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, tuple_type,
        &instantiation, &instantiated) == CM_TYPECK_OK);
    scratch = cm_typeck_get_type(&fixture.typeck, instantiated);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_TUPLE
        && scratch->data.tuple_type.element_count == 6u);
    assert(scratch->data.tuple_type.elements[0] == fixture.u32_type
        && scratch->data.tuple_type.elements[2] == fixture.bool_type);
    scratch = cm_typeck_get_type(&fixture.typeck,
        cm_typeck_get_type(&fixture.typeck, instantiated)
            ->data.tuple_type.elements[1]);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_PARAMETER
        && scratch->data.parameter_type.parameter == fixture.parameters[1]);
    scratch = cm_typeck_get_type(&fixture.typeck,
        cm_typeck_get_type(&fixture.typeck, instantiated)
            ->data.tuple_type.elements[3]);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_REFERENCE
        && scratch->data.reference_type.region.kind == CM_HIR_REGION_STATIC
        && scratch->data.reference_type.pointee == fixture.u32_type);
    scratch = cm_typeck_get_type(&fixture.typeck,
        cm_typeck_get_type(&fixture.typeck, instantiated)
            ->data.tuple_type.elements[4]);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_ARRAY
        && scratch->data.array_type.element == fixture.u32_type
        && scratch->data.array_type.length.kind == CM_HIR_CONST_VALUE
        && scratch->data.array_type.length.data.value.low_bits
            == UINT64_C(9));
    scratch = cm_typeck_get_type(&fixture.typeck,
        cm_typeck_get_type(&fixture.typeck, instantiated)
            ->data.tuple_type.elements[5]);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_ADT
        && scratch->data.named_type.argument_count == 3u
        && scratch->data.named_type.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_STATIC
        && scratch->data.named_type.arguments[1].data.type
            == fixture.u32_type
        && scratch->data.named_type.arguments[2].data.constant.kind
            == CM_HIR_CONST_VALUE);

    memset(&instantiated_named, 0, sizeof(instantiated_named));
    assert(cm_typeck_instantiate_hir_named(&fixture.typeck,
        &cm_hir_get_type(&fixture.hir, named_type)->data.named_type,
        &instantiation, &instantiated_named) == CM_TYPECK_OK);
    assert(instantiated_named.definition.crate_id
            == fixture.definitions[2].crate_id
        && instantiated_named.definition.index
            == fixture.definitions[2].index
        && instantiated_named.argument_count == 3u
        && instantiated_named.arguments[0].data.lifetime.kind
            == CM_HIR_REGION_STATIC
        && instantiated_named.arguments[1].data.type == fixture.u32_type
        && instantiated_named.arguments[2].data.constant.data.value.low_bits
            == UINT64_C(9));
    fixture_destroy(&fixture);
}

static void test_scoped_multi_owner_instantiation(void)
{
    TestFixture fixture;
    CmTypeckContext foreign_typeck;
    CmTypeckSnapshot snapshot;
    CmTypeckType scratch_type;
    CmHirItem method_item;
    CmHirItemId method_item_id;
    CmHirGenericParam const_parameter;
    CmHirGenericParamId const_parameter_id;
    CmHirType type;
    CmHirTypeId owner_types[2];
    CmHirTypeId self_type;
    CmHirTypeId tuple_type;
    CmHirTypeId elements[3];
    CmTypeckGenericArg arguments[3];
    CmTypeckInstantiationFrame frames[2];
    CmTypeckScopedInstantiation instantiation;
    CmTypeckTypeId instantiated;
    CmTypeckTypeId stale_type;
    CmTypeckTypeId reused_type;
    const CmTypeckType *scratch;
    size_t count;

    fixture_init(&fixture);
    memset(&method_item, 0, sizeof(method_item));
    method_item.kind = CM_HIR_ITEM_STRUCT;
    method_item.definition = fixture.definitions[2];
    method_item.owner_module = fixture.root_module;
    method_item.parent_definition = cm_hir_def_id_none();
    method_item.name = cm_hir_intern(&fixture.hir, "MethodOwner");
    method_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    method_item.visibility.restriction = cm_hir_def_id_none();
    method_item.span = test_span(3u, 4u);
    method_item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    method_item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&fixture.hir, &method_item, &method_item_id)
        == CM_HIR_OK);
    (void)method_item_id;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(55u, 56u);
    type.data.parameter_type.parameter = fixture.parameters[0];
    assert(cm_hir_add_type(&fixture.hir, &type, &owner_types[0])
        == CM_HIR_OK);
    type.data.parameter_type.parameter = fixture.parameters[1];
    assert(cm_hir_add_type(&fixture.hir, &type, &owner_types[1])
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(55u, 56u);
    type.data.self_type.owner = fixture.definitions[5];
    assert(cm_hir_add_type(&fixture.hir, &type, &self_type) == CM_HIR_OK);
    elements[0] = owner_types[0];
    elements[1] = owner_types[1];
    elements[2] = self_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(55u, 56u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 3u;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_type) == CM_HIR_OK);

    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = fixture.u32_type;
    arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[1].data.type = fixture.bool_type;
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = fixture.definitions[0];
    frames[0].arguments = &arguments[0];
    frames[0].argument_count = 1u;
    frames[1].parameter_owner = fixture.definitions[1];
    frames[1].arguments = &arguments[1];
    frames[1].argument_count = 1u;
    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.frames = frames;
    instantiation.frame_count = 2u;
    instantiation.self_owner = fixture.definitions[5];
    instantiation.self_type = fixture.i32_type;
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            tuple_type, &instantiation, &instantiated) == CM_TYPECK_OK);
    scratch = cm_typeck_get_type(&fixture.typeck, instantiated);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_TUPLE
        && scratch->data.tuple_type.element_count == 3u
        && scratch->data.tuple_type.elements[0] == fixture.u32_type
        && scratch->data.tuple_type.elements[1] == fixture.bool_type
        && scratch->data.tuple_type.elements[2] == fixture.i32_type);

    frames[0].parameter_owner = fixture.definitions[1];
    frames[0].arguments = &arguments[1];
    frames[1].parameter_owner = fixture.definitions[0];
    frames[1].arguments = &arguments[0];
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            tuple_type, &instantiation, &instantiated) == CM_TYPECK_OK);
    scratch = cm_typeck_get_type(&fixture.typeck, instantiated);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_TUPLE
        && scratch->data.tuple_type.elements[0] == fixture.u32_type
        && scratch->data.tuple_type.elements[1] == fixture.bool_type
        && scratch->data.tuple_type.elements[2] == fixture.i32_type);

    count = cm_typeck_type_count(&fixture.typeck);
    frames[1] = frames[0];
    assert(!cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            tuple_type, &instantiation, &instantiated)
            == CM_TYPECK_INVALID_ARGUMENT
        && instantiated == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);

    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            owner_types[0], &instantiation, &instantiated) == CM_TYPECK_OK);
    scratch = cm_typeck_get_type(&fixture.typeck, instantiated);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_PARAMETER
        && scratch->data.parameter_type.parameter == fixture.parameters[0]);
    assert(cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
        self_type, &instantiation, &instantiated)
        == CM_TYPECK_UNSUPPORTED_HIR_TYPE);

    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = fixture.definitions[2];
    instantiation.frames = frames;
    instantiation.frame_count = 1u;
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
        &instantiation));
    frames[0].parameter_owner = fixture.definitions[1];
    assert(!cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
        &instantiation));
    frames[0].arguments = &arguments[0];
    arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    arguments[0].data.lifetime.kind = CM_HIR_REGION_STATIC;
    frames[0].argument_count = 1u;
    assert(!cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
        &instantiation));

    elements[0] = owner_types[0];
    elements[1] = self_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(55u, 56u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&fixture.hir, &type, &tuple_type) == CM_HIR_OK);
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = fixture.u32_type;
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = fixture.definitions[2];
    frames[1].parameter_owner = fixture.definitions[0];
    frames[1].arguments = &arguments[0];
    frames[1].argument_count = 1u;
    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.frames = frames;
    instantiation.frame_count = 2u;
    instantiation.self_owner = fixture.definitions[5];
    instantiation.self_type = fixture.i32_type;
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            tuple_type, &instantiation, &instantiated) == CM_TYPECK_OK);
    scratch = cm_typeck_get_type(&fixture.typeck, instantiated);
    assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_TUPLE
        && scratch->data.tuple_type.element_count == 2u
        && scratch->data.tuple_type.elements[0] == fixture.u32_type
        && scratch->data.tuple_type.elements[1] == fixture.i32_type);

    frames[0].arguments = &arguments[0];
    frames[0].argument_count = 1u;
    assert(!cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
        &instantiation));

    memset(&const_parameter, 0, sizeof(const_parameter));
    const_parameter.kind = CM_HIR_GENERIC_CONST;
    const_parameter.owner = fixture.definitions[0];
    const_parameter.index = 1u;
    const_parameter.name = cm_hir_intern(&fixture.hir, "N");
    const_parameter.span = test_span(56u, 57u);
    const_parameter.declared_type = owner_types[1];
    assert(cm_hir_add_generic_param(&fixture.hir, &const_parameter,
        &const_parameter_id) == CM_HIR_OK);
    (void)const_parameter_id;
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = fixture.u32_type;
    arguments[1].kind = CM_HIR_GENERIC_ARG_CONST;
    arguments[1].data.constant.kind = CM_HIR_CONST_VALUE;
    arguments[1].data.constant.type = fixture.bool_type;
    arguments[1].data.constant.data.value.low_bits = UINT64_C(3);
    arguments[2].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[2].data.type = fixture.bool_type;
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = fixture.definitions[0];
    frames[0].arguments = &arguments[0];
    frames[0].argument_count = 2u;
    frames[1].parameter_owner = fixture.definitions[1];
    frames[1].arguments = &arguments[2];
    frames[1].argument_count = 1u;
    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.frames = frames;
    instantiation.frame_count = 2u;
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            fixture.hir_u32, &instantiation, &instantiated)
            == CM_TYPECK_OK);
    count = cm_typeck_type_count(&fixture.typeck);
    arguments[1].data.constant.type = fixture.u32_type;
    assert(cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            fixture.hir_u32, &instantiation, &instantiated)
            == CM_TYPECK_TYPE_MISMATCH
        && instantiated == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);

    assert(cm_typeck_snapshot(&fixture.typeck, &snapshot) == CM_TYPECK_OK);
    memset(&scratch_type, 0, sizeof(scratch_type));
    scratch_type.kind = CM_TYPECK_TYPE_TUPLE;
    scratch_type.span = test_span(58u, 59u);
    scratch_type.data.tuple_type.elements = &fixture.u32_type;
    scratch_type.data.tuple_type.element_count = 1u;
    assert(cm_typeck_add_type(&fixture.typeck, &scratch_type, &stale_type)
        == CM_TYPECK_OK);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = stale_type;
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = fixture.definitions[1];
    frames[0].arguments = &arguments[0];
    frames[0].argument_count = 1u;
    cm_typeck_scoped_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.frames = frames;
    instantiation.frame_count = 1u;
    assert(cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
        &instantiation));
    assert(cm_typeck_rollback(&fixture.typeck, &snapshot) == CM_TYPECK_OK);
    scratch_type.data.tuple_type.elements = &fixture.bool_type;
    assert(cm_typeck_add_type(&fixture.typeck, &scratch_type, &reused_type)
        == CM_TYPECK_OK && reused_type == stale_type);
    assert(!cm_typeck_scoped_instantiation_is_valid(&fixture.typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&fixture.typeck,
            owner_types[1], &instantiation, &instantiated)
            == CM_TYPECK_INVALID_ARGUMENT
        && instantiated == CM_TYPECK_TYPE_NONE);

    cm_typeck_context_init(&foreign_typeck, &fixture.hir);
    assert(!cm_typeck_scoped_instantiation_is_valid(&foreign_typeck,
            &instantiation)
        && cm_typeck_instantiate_hir_type_scoped(&foreign_typeck,
            fixture.hir_u32, &instantiation, &instantiated)
            == CM_TYPECK_INVALID_ARGUMENT);
    cm_typeck_context_destroy(&foreign_typeck);
    fixture_destroy(&fixture);
}

static void test_instantiation_rejection_is_atomic(void)
{
    TestFixture fixture;
    CmHirGenericParam const_generic;
    CmHirGenericParamId const_parameter;
    CmTypeckGenericArg argument;
    CmTypeckInstantiation instantiation;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirTypeId self_type;
    CmHirTypeId deep_type;
    CmHirGenericArg named_argument;
    CmHirNamedType named;
    CmTypeckNamedType out_named;
    CmTypeckTypeId out;
    CmHirContextMark mark;
    size_t count;
    uint32_t index;

    fixture_init(&fixture);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(60u, 61u);
    type.data.parameter_type.parameter = fixture.parameters[0];
    assert(cm_hir_add_type(&fixture.hir, &type, &parameter_type)
        == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture.u32_type;
    cm_typeck_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.parameter_owner = fixture.definitions[0];
    instantiation.arguments = &argument;
    instantiation.argument_count = 1u;
    count = cm_typeck_type_count(&fixture.typeck);

    out = fixture.bool_type;
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        NULL, &out) == CM_TYPECK_INVALID_ARGUMENT
        && out == CM_TYPECK_TYPE_NONE);
    memset(&named, 0, sizeof(named));
    named.definition = fixture.definitions[2];
    memset(&out_named, 0xff, sizeof(out_named));
    assert(cm_typeck_instantiate_hir_named(&fixture.typeck, &named,
        NULL, &out_named) == CM_TYPECK_INVALID_ARGUMENT
        && cm_hir_def_id_is_none(out_named.definition)
        && out_named.arguments == NULL && out_named.argument_count == 0u);

    instantiation.argument_count = 0u;
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        &instantiation, &out) == CM_TYPECK_INVALID_ARGUMENT);
    instantiation.argument_count = 1u;
    argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        &instantiation, &out) == CM_TYPECK_INVALID_ARGUMENT);
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = CM_TYPECK_TYPE_NONE;
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        &instantiation, &out) == CM_TYPECK_INVALID_ARGUMENT);
    argument.data.type = fixture.u32_type;
    instantiation.parameter_owner = fixture.definitions[3];
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        &instantiation, &out) == CM_TYPECK_INVALID_ARGUMENT);
    assert(cm_typeck_type_count(&fixture.typeck) == count);

    memset(&const_generic, 0, sizeof(const_generic));
    const_generic.kind = CM_HIR_GENERIC_CONST;
    const_generic.owner = fixture.definitions[2];
    const_generic.name = cm_hir_intern(&fixture.hir, "M");
    const_generic.span = test_span(60u, 61u);
    const_generic.declared_type = fixture.hir_usize;
    assert(cm_hir_add_generic_param(&fixture.hir, &const_generic,
        &const_parameter) == CM_HIR_OK);
    (void)const_parameter;
    argument.kind = CM_HIR_GENERIC_ARG_CONST;
    argument.data.constant.kind = CM_HIR_CONST_VALUE;
    argument.data.constant.type = fixture.u32_type;
    argument.data.constant.data.value.low_bits = UINT64_C(3);
    instantiation.parameter_owner = fixture.definitions[2];
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck,
        fixture.hir_u32, &instantiation, &out)
        == CM_TYPECK_TYPE_MISMATCH);
    assert(out == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);

    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture.u32_type;
    instantiation.parameter_owner = fixture.definitions[0];
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(60u, 61u);
    type.data.self_type.owner = fixture.definitions[5];
    assert(cm_hir_add_type(&fixture.hir, &type, &self_type) == CM_HIR_OK);
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, self_type,
        &instantiation, &out) == CM_TYPECK_UNSUPPORTED_HIR_TYPE);
    assert(out == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);
    instantiation.self_owner = fixture.definitions[4];
    instantiation.self_type = fixture.bool_type;
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, self_type,
        &instantiation, &out) == CM_TYPECK_UNSUPPORTED_HIR_TYPE);
    assert(out == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);

    memset(&named_argument, 0, sizeof(named_argument));
    named_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    named_argument.data.type = self_type;
    memset(&named, 0, sizeof(named));
    named.definition = fixture.definitions[2];
    named.arguments = &named_argument;
    named.argument_count = 1u;
    memset(&out_named, 0xff, sizeof(out_named));
    assert(cm_typeck_instantiate_hir_named(&fixture.typeck, &named,
        &instantiation, &out_named) == CM_TYPECK_UNSUPPORTED_HIR_TYPE);
    assert(cm_hir_def_id_is_none(out_named.definition)
        && out_named.arguments == NULL && out_named.argument_count == 0u
        && cm_typeck_type_count(&fixture.typeck) == count);

    instantiation.self_owner = cm_hir_def_id_none();
    instantiation.self_type = CM_TYPECK_TYPE_NONE;
    deep_type = parameter_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(60u, 61u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    for (index = 0u; index < 300u; ++index) {
        type.data.reference_type.pointee = deep_type;
        assert(cm_hir_add_type(&fixture.hir, &type, &deep_type) == CM_HIR_OK);
    }
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, deep_type,
        &instantiation, &out) == CM_TYPECK_OVERFLOW);
    assert(out == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&fixture.typeck) == count);

    assert(cm_hir_context_mark(&fixture.hir, &mark) == CM_HIR_OK);
    assert(cm_hir_context_rewind(&fixture.hir, &mark) == CM_HIR_OK);
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, parameter_type,
        &instantiation, &out) == CM_TYPECK_INVALID_ARGUMENT);
    fixture_destroy(&fixture);
}

static void test_instantiation_memoizes_shared_hir_dag(void)
{
    TestFixture fixture;
    CmHirType type;
    CmHirTypeId current;
    CmHirTypeId elements[2];
    CmTypeckGenericArg argument;
    CmTypeckInstantiation instantiation;
    CmTypeckTypeId out;
    size_t count;
    uint32_t index;

    fixture_init(&fixture);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(65u, 66u);
    type.data.parameter_type.parameter = fixture.parameters[0];
    assert(cm_hir_add_type(&fixture.hir, &type, &current) == CM_HIR_OK);
    for (index = 0u; index < 40u; ++index) {
        elements[0] = current;
        elements[1] = current;
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_TUPLE_KIND;
        type.span = test_span(65u, 66u);
        type.data.tuple_type.elements = elements;
        type.data.tuple_type.element_count = 2u;
        assert(cm_hir_add_type(&fixture.hir, &type, &current) == CM_HIR_OK);
    }
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture.u32_type;
    cm_typeck_instantiation_init(&fixture.typeck, &instantiation);
    instantiation.parameter_owner = fixture.definitions[0];
    instantiation.arguments = &argument;
    instantiation.argument_count = 1u;
    count = cm_typeck_type_count(&fixture.typeck);
    assert(cm_typeck_instantiate_hir_type(&fixture.typeck, current,
        &instantiation, &out) == CM_TYPECK_OK);
    assert(cm_typeck_type_count(&fixture.typeck) == count + 40u);
    for (index = 0u; index < 40u; ++index) {
        const CmTypeckType *scratch;

        scratch = cm_typeck_get_type(&fixture.typeck, out);
        assert(scratch != NULL && scratch->kind == CM_TYPECK_TYPE_TUPLE
            && scratch->data.tuple_type.element_count == 2u
            && scratch->data.tuple_type.elements[0]
                == scratch->data.tuple_type.elements[1]);
        out = scratch->data.tuple_type.elements[0];
    }
    assert(out == fixture.u32_type);
    fixture_destroy(&fixture);
}

static void test_status_names_and_invalid_inputs(void)
{
    TestFixture fixture;
    CmTypeckContext empty;
    CmHirGenericParam lifetime;
    CmHirGenericParamId lifetime_id;
    CmTypeckType invalid_parameter;
    CmTypeckTypeId out;
    unsigned int status;

    memset(&empty, 0, sizeof(empty));
    cm_typeck_context_init(&empty, NULL);
    assert(cm_typeck_new_variable(&empty, CM_HIR_INFER_GENERAL,
        test_span(1u, 2u), &out) == CM_TYPECK_INVALID_ARGUMENT);
    cm_typeck_context_destroy(&empty);
    fixture_init(&fixture);
    assert(cm_typeck_unify(&fixture.typeck, CM_TYPECK_TYPE_NONE,
        fixture.u32_type) == CM_TYPECK_INVALID_ID);
    memset(&lifetime, 0, sizeof(lifetime));
    lifetime.kind = CM_HIR_GENERIC_LIFETIME;
    lifetime.owner = fixture.definitions[2];
    lifetime.name = cm_hir_intern(&fixture.hir, "lt");
    lifetime.span = test_span(1u, 2u);
    assert(cm_hir_add_generic_param(&fixture.hir, &lifetime, &lifetime_id)
        == CM_HIR_OK);
    memset(&invalid_parameter, 0, sizeof(invalid_parameter));
    invalid_parameter.kind = CM_TYPECK_TYPE_PARAMETER;
    invalid_parameter.span = test_span(1u, 2u);
    invalid_parameter.data.parameter_type.parameter = lifetime_id;
    assert(cm_typeck_add_type(&fixture.typeck, &invalid_parameter, &out)
        == CM_TYPECK_INVALID_ARGUMENT);
    assert(cm_typeck_status_name((CmTypeckStatus)99u) != NULL);
    for (status = (unsigned int)CM_TYPECK_OK;
         status <= (unsigned int)CM_TYPECK_HIR_FAILURE; ++status) {
        assert(strcmp(cm_typeck_status_name((CmTypeckStatus)status),
            "unknown") != 0);
    }
    fixture_destroy(&fixture);
}

int main(void)
{
    test_variable_classes_and_aliases();
    test_balanced_deterministic_variable_roots();
    test_transactional_numeric_defaulting();
    test_region_erased_unification();
    test_structural_unification_and_rollback();
    test_functions_parameters_and_projections();
    test_occurs_and_snapshots();
    test_deep_finite_occurs_check();
    test_hir_rewind_invalidates_session();
    test_hir_reinit_invalidates_session();
    test_recursion_limits_fail_closed();
    test_freeze_transactions();
    test_import_rejection_and_complex_reuse();
    test_transactional_hir_instantiation();
    test_scoped_multi_owner_instantiation();
    test_instantiation_rejection_is_atomic();
    test_instantiation_memoizes_shared_hir_dag();
    test_status_names_and_invalid_inputs();
    puts("hir typeck tests passed");
    return 0;
}
