#include "cm/hir/projection_normalizer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root;
    CmHirDefId owner;
    CmHirTypeId bool_hir;
    CmHirTypeId u8_hir;
} TestFixture;

typedef struct TestRuntime {
    CmParamEnv environment;
    CmTraitImplIndex index;
    CmTypeckContext typeck;
    CmTypeckInstantiation exact;
    CmParamEnvSubstitution substitution;
    CmTypeckTypeId bool_type;
    CmTypeckTypeId u8_type;
} TestRuntime;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static void init_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module, CmInternId name)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = cm_hir_def_id_none();
    item->name = name;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = test_span(1u, 20u);
}

static CmHirTypeId add_scalar(TestFixture *fixture, CmHirTypeKind kind,
    CmHirIntType integer_kind)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = integer_kind;
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_trait(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_type_struct(TestFixture *fixture, const char *name)
{
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_STRUCT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&fixture->hir, "T");
    parameter.span = test_span(3u, 4u);
    assert(cm_hir_add_generic_param(&fixture->hir, &parameter,
        &parameter_id) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_STRUCT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_trait_associated(TestFixture *fixture,
    CmHirDefId trait_definition, const char *name)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(1u, 20u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.parent_definition = trait_definition;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirTypeId add_projection(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(2u, 3u);
    type.data.projection_type.self_type = fixture->bool_hir;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&fixture->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirDefId add_bool_impl(TestFixture *fixture,
    CmHirDefId trait_definition)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_IMPL, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_IMPL, definition, fixture->root,
        CM_INTERN_ID_NONE);
    item.data.impl_item.self_type = fixture->bool_hir;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_impl_associated(TestFixture *fixture,
    CmHirDefId impl_definition, CmHirDefId trait_associated_definition,
    const char *name, CmHirTypeId target)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(&fixture->hir,
        fixture->crate_id, test_span(1u, 20u), &definition) == CM_HIR_OK);
    init_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, fixture->root,
        cm_hir_intern(&fixture->hir, name));
    item.parent_definition = impl_definition;
    item.data.type_alias_item.target = target;
    item.data.type_alias_item.trait_item_definition =
        trait_associated_definition;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_owner_equality(TestFixture *fixture,
    CmHirDefId trait_definition, CmHirDefId associated_definition,
    CmHirTypeId value)
{
    CmHirDefId definition;
    CmHirAssociatedTypeEquality equality;
    CmHirTraitPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->hir,
        fixture->crate_id, CM_HIR_ITEM_TRAIT, test_span(1u, 20u),
        &definition) == CM_HIR_OK);
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = associated_definition;
    equality.value = value;
    equality.span = test_span(6u, 8u);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = fixture->bool_hir;
    predicate.trait_type.definition = trait_definition;
    predicate.equalities = &equality;
    predicate.equality_count = 1u;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(4u, 10u);
    init_item(&item, CM_HIR_ITEM_TRAIT, definition, fixture->root,
        cm_hir_intern(&fixture->hir, "Owner"));
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->hir, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void fixture_init(TestFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->hir);
    assert(cm_hir_create_crate(&fixture->hir,
        cm_hir_intern(&fixture->hir, "projection_normalizer"),
        CM_HIR_EDITION_2024, test_span(0u, 30u), &fixture->crate_id,
        &fixture->root) == CM_HIR_OK);
    fixture->bool_hir = add_scalar(fixture, CM_HIR_TYPE_BOOL_KIND,
        CM_HIR_INT_U8);
    fixture->u8_hir = add_scalar(fixture, CM_HIR_TYPE_INTEGER_KIND,
        CM_HIR_INT_U8);
}

static void runtime_init(TestRuntime *runtime, TestFixture *fixture)
{
    memset(runtime, 0, sizeof(*runtime));
    assert(cm_param_env_init(&runtime->environment, &fixture->hir,
        fixture->owner) == CM_PARAM_ENV_READY);
    assert(cm_trait_impl_index_init(&runtime->index, &fixture->hir,
        fixture->crate_id, CM_TRAIT_IMPL_UNIVERSE_OPEN)
        == CM_TRAIT_SOLVER_PROVEN);
    cm_typeck_context_init(&runtime->typeck, &fixture->hir);
    assert(cm_typeck_import_hir_type(&runtime->typeck, fixture->bool_hir,
        &runtime->bool_type) == CM_TYPECK_OK);
    assert(cm_typeck_import_hir_type(&runtime->typeck, fixture->u8_hir,
        &runtime->u8_type) == CM_TYPECK_OK);
    cm_typeck_instantiation_init(&runtime->typeck, &runtime->exact);
    runtime->exact.parameter_owner = fixture->owner;
    runtime->exact.self_owner = fixture->owner;
    runtime->exact.self_type = runtime->bool_type;
    runtime->substitution.exact = &runtime->exact;
}

static void runtime_destroy(TestRuntime *runtime)
{
    cm_typeck_context_destroy(&runtime->typeck);
    cm_trait_impl_index_destroy(&runtime->index);
    cm_param_env_destroy(&runtime->environment);
}

static CmProjectionNormalizeLimits limits(size_t nodes, size_t projections)
{
    CmProjectionNormalizeLimits value;

    value.max_nodes = nodes;
    value.max_projection_steps = projections;
    return value;
}

static void assert_u8(TestRuntime *runtime, CmTypeckTypeId type)
{
    const CmTypeckType *resolved_type;

    assert(cm_typeck_resolve(&runtime->typeck, type, &type) == CM_TYPECK_OK);
    resolved_type = cm_typeck_get_type(&runtime->typeck, type);
    assert(resolved_type != NULL
        && resolved_type->kind == CM_TYPECK_TYPE_INTEGER
        && resolved_type->data.integer_type == CM_HIR_INT_U8);
}

static CmProjectionNormalizeResult normalize(TestFixture *fixture,
    TestRuntime *runtime, CmTypeckTypeId type, size_t projection_fuel)
{
    return cm_projection_normalize_type(&runtime->index,
        &runtime->environment, &runtime->typeck, &runtime->substitution,
        fixture->owner, type, NULL, limits(128u, projection_fuel));
}

static CmProjectionNormalizeResult normalize_traced(TestFixture *fixture,
    TestRuntime *runtime, CmTypeckTypeId type, size_t projection_fuel,
    CmProjectionNormalizeTrace *trace)
{
    return cm_projection_normalize_type_traced(&runtime->index,
        &runtime->environment, &runtime->typeck, &runtime->substitution,
        fixture->owner, type, NULL, limits(128u, projection_fuel), trace);
}

static void test_root_environment_and_impl(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId impl_associated_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTrace trace;
    const CmProjectionNormalizeStep *step;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Environment");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "Assoc");
    projection_hir = add_projection(&fixture, trait_definition,
        associated_definition);
    fixture.owner = add_owner_equality(&fixture, trait_definition,
        associated_definition, fixture.u8_hir);
    impl_definition = add_bool_impl(&fixture, trait_definition);
    impl_associated_definition = add_impl_associated(&fixture,
        impl_definition, associated_definition, "Assoc", fixture.bool_hir);
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&trace);
    {
        size_t type_count;

        type_count = cm_typeck_type_count(&runtime.typeck);
        result = normalize_traced(&fixture, &runtime, projection_type, 0u,
            &trace);
        assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW
            && result.cause
                == CM_PROJECTION_NORMALIZE_CAUSE_PROJECTION_LIMIT
            && result.type == CM_TYPECK_TYPE_NONE
            && result.projection_step_count == 0u
            && cm_projection_normalize_trace_count(&trace) == 0u
            && cm_typeck_type_count(&runtime.typeck) == type_count);
    }
    result = normalize_traced(&fixture, &runtime, projection_type, 1u,
        &trace);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 1u);
    assert(cm_projection_normalize_trace_count(&trace) == 1u
        && cm_projection_normalize_trace_step(&trace, 1u) == NULL);
    step = cm_projection_normalize_trace_step(&trace, 0u);
    assert(step != NULL && step->projection == projection_type
        && step->target == result.type
        && step->normalized_target == result.type
        && step->proof_origin == CM_TRAIT_PROOF_PARAM_ENV
        && step->param_env_fact_index == 0u
        && step->param_env_equality_index == 0u
        && cm_hir_def_id_is_none(step->impl_definition)
        && cm_hir_def_id_is_none(step->impl_associated_definition));
    assert_u8(&runtime, result.type);
    result = cm_projection_normalize_type_traced(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        fixture.owner, projection_type, NULL, limits(0u, 1u), &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && cm_projection_normalize_trace_count(&trace) == 0u);
    cm_projection_normalize_trace_clear(&trace);
    assert(cm_projection_normalize_trace_count(&trace) == 0u);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Impl");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "Assoc");
    projection_hir = add_projection(&fixture, trait_definition,
        associated_definition);
    impl_definition = add_bool_impl(&fixture, trait_definition);
    impl_associated_definition = add_impl_associated(&fixture,
        impl_definition, associated_definition, "Assoc", fixture.u8_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&trace);
    result = normalize_traced(&fixture, &runtime, projection_type, 1u,
        &trace);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 1u);
    assert(cm_projection_normalize_trace_count(&trace) == 1u);
    step = cm_projection_normalize_trace_step(&trace, 0u);
    assert(step != NULL && step->projection == projection_type
        && step->target == result.type
        && step->normalized_target == result.type
        && step->proof_origin == CM_TRAIT_PROOF_IMPL
        && step->param_env_fact_index == CM_TRAIT_PROOF_FACT_NONE
        && step->param_env_equality_index
            == CM_TRAIT_PROOF_EQUALITY_NONE
        && cm_hir_def_id_equal(step->impl_definition, impl_definition)
        && cm_hir_def_id_equal(step->impl_associated_definition,
            impl_associated_definition));
    assert_u8(&runtime, result.type);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_structural_rebuild_and_siblings(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckType tuple;
    CmTypeckType reference;
    CmTypeckTypeId elements[2];
    CmTypeckTypeId reference_type;
    CmTypeckTypeId tuple_type;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTrace trace;
    const CmProjectionNormalizeStep *first_step;
    const CmProjectionNormalizeStep *second_step;
    const CmTypeckType *normalized;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Nested");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "Assoc");
    projection_hir = add_projection(&fixture, trait_definition,
        associated_definition);
    impl_definition = add_bool_impl(&fixture, trait_definition);
    add_impl_associated(&fixture, impl_definition, associated_definition,
        "Assoc", fixture.u8_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&reference, 0, sizeof(reference));
    reference.kind = CM_TYPECK_TYPE_REFERENCE;
    reference.span = test_span(8u, 9u);
    reference.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    reference.data.reference_type.pointee = projection_type;
    reference.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_typeck_add_type(&runtime.typeck, &reference,
        &reference_type) == CM_TYPECK_OK);
    elements[0] = reference_type;
    elements[1] = projection_type;
    memset(&tuple, 0, sizeof(tuple));
    tuple.kind = CM_TYPECK_TYPE_TUPLE;
    tuple.span = test_span(10u, 20u);
    tuple.data.tuple_type.elements = elements;
    tuple.data.tuple_type.element_count = 2u;
    assert(cm_typeck_add_type(&runtime.typeck, &tuple, &tuple_type)
        == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&trace);
    result = normalize_traced(&fixture, &runtime, tuple_type, 2u, &trace);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 2u);
    assert(cm_projection_normalize_trace_count(&trace) == 2u);
    first_step = cm_projection_normalize_trace_step(&trace, 0u);
    second_step = cm_projection_normalize_trace_step(&trace, 1u);
    assert(first_step != NULL && second_step != NULL
        && first_step->projection == projection_type
        && first_step->target == first_step->normalized_target
        && second_step->projection == projection_type
        && second_step->target == second_step->normalized_target);
    normalized = cm_typeck_get_type(&runtime.typeck, result.type);
    assert(normalized != NULL && normalized->kind == CM_TYPECK_TYPE_TUPLE
        && normalized->data.tuple_type.element_count == 2u);
    {
        const CmTypeckType *normalized_reference;

        normalized_reference = cm_typeck_get_type(&runtime.typeck,
            normalized->data.tuple_type.elements[0]);
        assert(normalized_reference != NULL
            && normalized_reference->kind == CM_TYPECK_TYPE_REFERENCE
            && normalized_reference->data.reference_type.region.kind
                == CM_HIR_REGION_STATIC
            && normalized_reference->data.reference_type.mutability
                == CM_HIR_MUTABLE);
        assert_u8(&runtime,
            normalized_reference->data.reference_type.pointee);
    }
    assert_u8(&runtime, normalized->data.tuple_type.elements[1]);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_chain_fuel_cycle_and_rollback(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId first_trait;
    CmHirDefId first_associated;
    CmHirDefId first_impl;
    CmHirDefId second_trait;
    CmHirDefId second_associated;
    CmHirDefId second_impl;
    CmHirTypeId first_projection_hir;
    CmHirTypeId second_projection_hir;
    CmTypeckTypeId first_projection;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTrace trace;
    const CmProjectionNormalizeStep *first_step;
    const CmProjectionNormalizeStep *second_step;
    size_t type_count;

    fixture_init(&fixture);
    first_trait = add_trait(&fixture, "First");
    first_associated = add_trait_associated(&fixture, first_trait,
        "FirstAssoc");
    second_trait = add_trait(&fixture, "Second");
    second_associated = add_trait_associated(&fixture, second_trait,
        "SecondAssoc");
    first_projection_hir = add_projection(&fixture, first_trait,
        first_associated);
    second_projection_hir = add_projection(&fixture, second_trait,
        second_associated);
    first_impl = add_bool_impl(&fixture, first_trait);
    add_impl_associated(&fixture, first_impl, first_associated,
        "FirstAssoc", second_projection_hir);
    second_impl = add_bool_impl(&fixture, second_trait);
    add_impl_associated(&fixture, second_impl, second_associated,
        "SecondAssoc", fixture.u8_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, first_projection_hir,
        &first_projection) == CM_TYPECK_OK);
    cm_projection_normalize_trace_init(&trace);
    result = normalize(&fixture, &runtime, runtime.u8_type, 0u);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 0u);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = normalize_traced(&fixture, &runtime, first_projection, 1u,
        &trace);
    assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW
        && result.cause == CM_PROJECTION_NORMALIZE_CAUSE_PROJECTION_LIMIT
        && result.type == CM_TYPECK_TYPE_NONE
        && result.projection_step_count == 1u
        && cm_projection_normalize_trace_count(&trace) == 0u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    result = normalize_traced(&fixture, &runtime, first_projection, 2u,
        &trace);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 2u);
    assert(cm_projection_normalize_trace_count(&trace) == 2u);
    first_step = cm_projection_normalize_trace_step(&trace, 0u);
    second_step = cm_projection_normalize_trace_step(&trace, 1u);
    assert(first_step != NULL && second_step != NULL
        && first_step->projection == first_projection
        && first_step->target == second_step->projection
        && first_step->normalized_target == result.type
        && second_step->target == result.type
        && second_step->normalized_target == result.type
        && first_step->proof_origin == CM_TRAIT_PROOF_IMPL
        && second_step->proof_origin == CM_TRAIT_PROOF_IMPL);
    assert_u8(&runtime, result.type);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);

    fixture_init(&fixture);
    first_trait = add_trait(&fixture, "Cycle");
    first_associated = add_trait_associated(&fixture, first_trait,
        "CycleAssoc");
    first_projection_hir = add_projection(&fixture, first_trait,
        first_associated);
    first_impl = add_bool_impl(&fixture, first_trait);
    add_impl_associated(&fixture, first_impl, first_associated,
        "CycleAssoc", first_projection_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, first_projection_hir,
        &first_projection) == CM_TYPECK_OK);
    {
        CmTypeckType fresh_projection;
        CmTypeckTypeId fresh_projection_id;

        fresh_projection = *cm_typeck_get_type(&runtime.typeck,
            first_projection);
        fresh_projection.span = test_span(21u, 29u);
        assert(cm_typeck_add_type(&runtime.typeck, &fresh_projection,
            &fresh_projection_id) == CM_TYPECK_OK);
        first_projection = fresh_projection_id;
    }
    type_count = cm_typeck_type_count(&runtime.typeck);
    cm_projection_normalize_trace_init(&trace);
    result = normalize_traced(&fixture, &runtime, first_projection, 32u,
        &trace);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.cause == CM_PROJECTION_NORMALIZE_CAUSE_CYCLE
        && result.type == CM_TYPECK_TYPE_NONE
        && result.projection_step_count == 1u
        && cm_projection_normalize_trace_count(&trace) == 0u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_authentication_and_node_limit(void)
{
    TestFixture fixture;
    TestFixture foreign_fixture;
    TestRuntime runtime;
    CmTypeckContext foreign_typeck;
    CmTypeckContext same_hir_typeck;
    CmTypeckTypeId foreign_u8;
    CmTypeckTypeId same_hir_u8;
    CmProjectionNormalizeResult result;
    CmHirDefId wrong_owner;
    CmHirType appended;
    CmHirTypeId appended_id;
    CmProjectionNormalizeTrace trace;
    size_t type_count;

    fixture_init(&fixture);
    fixture.owner = add_trait(&fixture, "Owner");
    wrong_owner = add_trait(&fixture, "WrongOwner");
    runtime_init(&runtime, &fixture);
    cm_projection_normalize_trace_init(&trace);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = cm_projection_normalize_type_traced(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        wrong_owner, runtime.u8_type, NULL, limits(8u, 0u), &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime.exact.parameter_owner = wrong_owner;
    result = cm_projection_normalize_type_traced(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        fixture.owner, runtime.u8_type, NULL, limits(8u, 0u), &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime.exact.parameter_owner = fixture.owner;
    fixture_init(&foreign_fixture);
    foreign_fixture.owner = add_trait(&foreign_fixture, "ForeignOwner");
    cm_typeck_context_init(&foreign_typeck, &foreign_fixture.hir);
    assert(cm_typeck_import_hir_type(&foreign_typeck,
        foreign_fixture.u8_hir, &foreign_u8) == CM_TYPECK_OK);
    result = cm_projection_normalize_type(&runtime.index,
        &runtime.environment, &foreign_typeck, &runtime.substitution,
        fixture.owner, foreign_u8, NULL, limits(8u, 0u));
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&foreign_typeck) == 1u);
    cm_typeck_context_destroy(&foreign_typeck);
    cm_hir_context_destroy(&foreign_fixture.hir);
    cm_typeck_context_init(&same_hir_typeck, &fixture.hir);
    assert(cm_typeck_import_hir_type(&same_hir_typeck, fixture.u8_hir,
        &same_hir_u8) == CM_TYPECK_OK);
    result = cm_projection_normalize_type(&runtime.index,
        &runtime.environment, &same_hir_typeck, &runtime.substitution,
        fixture.owner, same_hir_u8, NULL, limits(8u, 0u));
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_typeck_type_count(&same_hir_typeck) == 1u);
    cm_typeck_context_destroy(&same_hir_typeck);
    result = cm_projection_normalize_type(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        fixture.owner, runtime.u8_type, NULL, limits(1u, 0u));
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.visited_node_count == 1u);
    result = cm_projection_normalize_type(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        fixture.owner, runtime.u8_type, NULL, limits(0u, 0u));
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE);
    memset(&appended, 0, sizeof(appended));
    appended.kind = CM_HIR_TYPE_UNIT_KIND;
    appended.span = test_span(24u, 25u);
    assert(cm_hir_add_type(&fixture.hir, &appended, &appended_id)
        == CM_HIR_OK);
    assert(!cm_param_env_is_current(&runtime.environment)
        && !cm_trait_impl_index_is_current(&runtime.index));
    result = cm_projection_normalize_type_traced(&runtime.index,
        &runtime.environment, &runtime.typeck, &runtime.substitution,
        fixture.owner, runtime.u8_type, NULL, limits(8u, 0u), &trace);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE
        && cm_projection_normalize_trace_count(&trace) == 0u);
    cm_projection_normalize_trace_destroy(&trace);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_adt_array_const_traversal(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId container_definition;
    CmHirTypeId projection_hir;
    CmTypeckTypeId projection_type;
    CmTypeckType array;
    CmTypeckType adt;
    CmTypeckGenericArg argument;
    CmTypeckTypeId array_type;
    CmTypeckTypeId adt_type;
    CmProjectionNormalizeResult result;
    const CmTypeckType *normalized_adt;
    const CmTypeckType *normalized_array;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Composite");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "CompositeAssoc");
    projection_hir = add_projection(&fixture, trait_definition,
        associated_definition);
    impl_definition = add_bool_impl(&fixture, trait_definition);
    add_impl_associated(&fixture, impl_definition, associated_definition,
        "CompositeAssoc", fixture.u8_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    container_definition = add_type_struct(&fixture, "Container");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, projection_hir,
        &projection_type) == CM_TYPECK_OK);
    memset(&array, 0, sizeof(array));
    array.kind = CM_TYPECK_TYPE_ARRAY;
    array.span = test_span(10u, 18u);
    array.data.array_type.element = projection_type;
    array.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    array.data.array_type.length.type = projection_type;
    array.data.array_type.length.data.value.low_bits = UINT64_C(7);
    array.data.array_type.length.data.value.high_bits = UINT64_C(11);
    assert(cm_typeck_add_type(&runtime.typeck, &array, &array_type)
        == CM_TYPECK_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = array_type;
    memset(&adt, 0, sizeof(adt));
    adt.kind = CM_TYPECK_TYPE_ADT;
    adt.span = test_span(8u, 20u);
    adt.data.named_type.definition = fixture.owner;
    adt.data.named_type.arguments = &argument;
    adt.data.named_type.argument_count = 1u;
    assert(cm_typeck_add_type(&runtime.typeck, &adt, &adt_type)
        == CM_TYPECK_OK);
    result = normalize(&fixture, &runtime, adt_type, 2u);
    assert(result.kind == CM_TRAIT_SOLVER_INVALID
        && result.type == CM_TYPECK_TYPE_NONE);
    adt.data.named_type.definition = container_definition;
    assert(cm_typeck_add_type(&runtime.typeck, &adt, &adt_type)
        == CM_TYPECK_OK);
    result = normalize(&fixture, &runtime, adt_type, 2u);
    assert(result.kind == CM_TRAIT_SOLVER_PROVEN
        && result.projection_step_count == 2u);
    normalized_adt = cm_typeck_get_type(&runtime.typeck, result.type);
    assert(normalized_adt != NULL
        && normalized_adt->kind == CM_TYPECK_TYPE_ADT
        && cm_hir_def_id_equal(normalized_adt->data.named_type.definition,
            container_definition)
        && normalized_adt->data.named_type.argument_count == 1u
        && normalized_adt->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    normalized_array = cm_typeck_get_type(&runtime.typeck,
        normalized_adt->data.named_type.arguments[0].data.type);
    assert(normalized_array != NULL
        && normalized_array->kind == CM_TYPECK_TYPE_ARRAY
        && normalized_array->span.start == 10u
        && normalized_array->span.end == 18u
        && normalized_array->data.array_type.length.kind
            == CM_HIR_CONST_VALUE
        && normalized_array->data.array_type.length.data.value.low_bits
            == UINT64_C(7)
        && normalized_array->data.array_type.length.data.value.high_bits
            == UINT64_C(11));
    assert_u8(&runtime, normalized_array->data.array_type.element);
    assert_u8(&runtime, normalized_array->data.array_type.length.type);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_mutual_cycle_and_late_child_rollback(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId first_trait;
    CmHirDefId first_associated;
    CmHirDefId first_impl;
    CmHirDefId second_trait;
    CmHirDefId second_associated;
    CmHirDefId second_impl;
    CmHirTypeId first_hir;
    CmHirTypeId second_hir;
    CmTypeckTypeId first_type;
    CmProjectionNormalizeResult result;
    CmProjectionNormalizeTrace trace;
    size_t type_count;

    fixture_init(&fixture);
    first_trait = add_trait(&fixture, "MutualFirst");
    first_associated = add_trait_associated(&fixture, first_trait,
        "MutualFirstAssoc");
    second_trait = add_trait(&fixture, "MutualSecond");
    second_associated = add_trait_associated(&fixture, second_trait,
        "MutualSecondAssoc");
    first_hir = add_projection(&fixture, first_trait, first_associated);
    second_hir = add_projection(&fixture, second_trait, second_associated);
    first_impl = add_bool_impl(&fixture, first_trait);
    add_impl_associated(&fixture, first_impl, first_associated,
        "MutualFirstAssoc", second_hir);
    second_impl = add_bool_impl(&fixture, second_trait);
    add_impl_associated(&fixture, second_impl, second_associated,
        "MutualSecondAssoc", first_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_import_hir_type(&runtime.typeck, first_hir,
        &first_type) == CM_TYPECK_OK);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = normalize(&fixture, &runtime, first_type, 32u);
    assert(result.kind == CM_TRAIT_SOLVER_AMBIGUOUS
        && result.cause == CM_PROJECTION_NORMALIZE_CAUSE_CYCLE
        && result.type == CM_TYPECK_TYPE_NONE
        && result.projection_step_count == 2u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);

    fixture_init(&fixture);
    first_trait = add_trait(&fixture, "Good");
    first_associated = add_trait_associated(&fixture, first_trait,
        "GoodAssoc");
    second_trait = add_trait(&fixture, "Missing");
    second_associated = add_trait_associated(&fixture, second_trait,
        "MissingAssoc");
    first_hir = add_projection(&fixture, first_trait, first_associated);
    second_hir = add_projection(&fixture, second_trait, second_associated);
    first_impl = add_bool_impl(&fixture, first_trait);
    {
        CmHirType reference;
        CmHirTypeId reference_hir;

        memset(&reference, 0, sizeof(reference));
        reference.kind = CM_HIR_TYPE_REFERENCE_KIND;
        reference.span = test_span(11u, 19u);
        reference.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
        reference.data.reference_type.pointee = fixture.u8_hir;
        reference.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_hir_add_type(&fixture.hir, &reference, &reference_hir)
            == CM_HIR_OK);
        add_impl_associated(&fixture, first_impl, first_associated,
            "GoodAssoc", reference_hir);
    }
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    {
        CmTypeckType tuple;
        CmTypeckTypeId tuple_elements[2];
        CmTypeckTypeId tuple_type;

        assert(cm_typeck_import_hir_type(&runtime.typeck, first_hir,
            &tuple_elements[0]) == CM_TYPECK_OK);
        assert(cm_typeck_import_hir_type(&runtime.typeck, second_hir,
            &tuple_elements[1]) == CM_TYPECK_OK);
        memset(&tuple, 0, sizeof(tuple));
        tuple.kind = CM_TYPECK_TYPE_TUPLE;
        tuple.span = test_span(10u, 20u);
        tuple.data.tuple_type.elements = tuple_elements;
        tuple.data.tuple_type.element_count = 2u;
        assert(cm_typeck_add_type(&runtime.typeck, &tuple, &tuple_type)
            == CM_TYPECK_OK);
        type_count = cm_typeck_type_count(&runtime.typeck);
        cm_projection_normalize_trace_init(&trace);
        result = cm_projection_normalize_type_traced(&runtime.index,
            &runtime.environment, &runtime.typeck, &runtime.substitution,
            fixture.owner, tuple_type, NULL, limits(4u, 8u), &trace);
        assert(result.kind == CM_TRAIT_SOLVER_OVERFLOW
            && result.cause == CM_PROJECTION_NORMALIZE_CAUSE_NODE_LIMIT
            && result.type == CM_TYPECK_TYPE_NONE
            && result.visited_node_count == 4u
            && result.projection_step_count == 1u
            && cm_projection_normalize_trace_count(&trace) == 0u
            && cm_typeck_type_count(&runtime.typeck) == type_count);
        result = normalize_traced(&fixture, &runtime, tuple_type, 8u,
            &trace);
        assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_METADATA
            && result.type == CM_TYPECK_TYPE_NONE
            && result.projection_step_count == 1u
            && cm_projection_normalize_trace_count(&trace) == 0u
            && cm_typeck_type_count(&runtime.typeck) == type_count);
        cm_projection_normalize_trace_destroy(&trace);
    }
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

static void test_inference_projection_defers_without_binding(void)
{
    TestFixture fixture;
    TestRuntime runtime;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmTypeckType projection;
    CmTypeckTypeId self_variable;
    CmTypeckTypeId projection_type;
    CmTypeckTypeId resolved;
    CmProjectionNormalizeResult result;
    size_t type_count;

    fixture_init(&fixture);
    trait_definition = add_trait(&fixture, "Inference");
    associated_definition = add_trait_associated(&fixture,
        trait_definition, "InferenceAssoc");
    impl_definition = add_bool_impl(&fixture, trait_definition);
    add_impl_associated(&fixture, impl_definition, associated_definition,
        "InferenceAssoc", fixture.u8_hir);
    fixture.owner = add_trait(&fixture, "Owner");
    runtime_init(&runtime, &fixture);
    assert(cm_typeck_new_variable(&runtime.typeck, CM_HIR_INFER_GENERAL,
        test_span(21u, 22u), &self_variable) == CM_TYPECK_OK);
    memset(&projection, 0, sizeof(projection));
    projection.kind = CM_TYPECK_TYPE_PROJECTION;
    projection.span = test_span(21u, 24u);
    projection.data.projection_type.self_type = self_variable;
    projection.data.projection_type.trait_type.definition =
        trait_definition;
    projection.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_typeck_add_type(&runtime.typeck, &projection,
        &projection_type) == CM_TYPECK_OK);
    type_count = cm_typeck_type_count(&runtime.typeck);
    result = normalize(&fixture, &runtime, projection_type, 4u);
    assert(result.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE
        && result.type == CM_TYPECK_TYPE_NONE
        && result.projection_step_count == 0u
        && cm_typeck_type_count(&runtime.typeck) == type_count);
    assert(cm_typeck_resolve(&runtime.typeck, self_variable, &resolved)
        == CM_TYPECK_OK && resolved == self_variable);
    runtime_destroy(&runtime);
    cm_hir_context_destroy(&fixture.hir);
}

int main(void)
{
    test_root_environment_and_impl();
    test_structural_rebuild_and_siblings();
    test_chain_fuel_cycle_and_rollback();
    test_authentication_and_node_limit();
    test_adt_array_const_traversal();
    test_mutual_cycle_and_late_child_rollback();
    test_inference_projection_defers_without_binding();
    puts("hir projection normalizer tests passed");
    return 0;
}
