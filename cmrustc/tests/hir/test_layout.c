#include "cm/hir/layout.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirTypeId add_integer_type(CmHirContext *context,
    CmHirIntType kind, uint32_t start)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(start, start + 1u);
    type.data.integer_type.kind = kind;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirTypeId add_adt_type(CmHirContext *context,
    CmHirDefId definition, uint32_t start)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(start, start + 1u);
    type.data.named_type.definition = definition;
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirField named_field(CmHirContext *context, const char *name,
    CmHirTypeId type, uint32_t start)
{
    CmHirField field;

    memset(&field, 0, sizeof(field));
    field.name = cm_hir_intern(context, name);
    field.type = type;
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(start, start + 1u);
    return field;
}

static CmHirField tuple_field(CmHirTypeId type, uint32_t start)
{
    CmHirField field;

    memset(&field, 0, sizeof(field));
    field.name = CM_INTERN_ID_NONE;
    field.type = type;
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(start, start + 1u);
    return field;
}

static void bind_aggregate(CmHirContext *context, CmHirModuleId module,
    CmHirDefId definition, CmHirItemKind kind, const char *name,
    CmHirAggregateForm form, CmHirField *fields, uint32_t field_count,
    CmHirAttribute *attributes, uint32_t attribute_count,
    CmHirGenericParamId generic_start, uint32_t generic_count,
    uint32_t start)
{
    CmHirItem item;
    CmHirItemId item_id;

    memset(&item, 0, sizeof(item));
    item.kind = kind;
    item.definition = definition;
    item.owner_module = module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 10u);
    item.attributes = attributes;
    item.attribute_count = attribute_count;
    item.generic_parameter_start = generic_start;
    item.generic_parameter_count = generic_count;
    item.data.aggregate_item.form = form;
    item.data.aggregate_item.fields = fields;
    item.data.aggregate_item.field_count = field_count;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
}

static CmHirTypeId add_aggregate(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirItemKind kind,
    const char *name, CmHirAggregateForm form, CmHirField *fields,
    uint32_t field_count, uint32_t start, CmHirDefId *out_definition)
{
    assert(cm_hir_reserve_item_definition_as(context, crate_id, kind,
        test_span(start, start + 10u), out_definition) == CM_HIR_OK);
    bind_aggregate(context, module, *out_definition, kind, name, form,
        fields, field_count, NULL, 0u, CM_HIR_GENERIC_PARAM_NONE, 0u,
        start);
    return add_adt_type(context, *out_definition, start + 11u);
}

static CmHirDefId add_empty_enum(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, uint32_t start)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(context, crate_id,
        CM_HIR_ITEM_ENUM, test_span(start, start + 10u), &definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_ENUM;
    item.definition = definition;
    item.owner_module = module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(context, "NotAStruct");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 10u);
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void expect_failure_unchanged(const CmHirContext *context,
    CmHirDefId definition, uint32_t capacity,
    CmHirLayoutStatus expected_status)
{
    CmHirNamedStructLayout layout;
    CmHirNamedStructLayout saved_layout;
    CmHirFieldLayout fields[4];
    CmHirFieldLayout saved_fields[4];

    memset(&layout, 0xa5, sizeof(layout));
    memset(fields, 0x5a, sizeof(fields));
    saved_layout = layout;
    memcpy(saved_fields, fields, sizeof(fields));
    assert(cm_hir_layout_named_struct(context, 64u, definition, &layout,
        fields, capacity) == expected_status);
    assert(memcmp(&layout, &saved_layout, sizeof(layout)) == 0);
    assert(memcmp(fields, saved_fields, sizeof(fields)) == 0);
}

static void test_successful_layout(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId i32_type;
    CmHirTypeId u32_type;
    CmHirTypeId inner_type;
    CmHirTypeId outer_type;
    CmHirField inner_fields[2];
    CmHirField outer_fields[3];
    CmHirField reordered_fields[2];
    CmHirDefId inner_definition;
    CmHirDefId outer_definition;
    CmHirDefId reordered_definition;
    CmHirNamedStructLayout layout32;
    CmHirNamedStructLayout layout64;
    CmHirFieldLayout fields32[3];
    CmHirFieldLayout fields64[3];
    CmHirFieldLayout reordered_layout[2];

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "layout_success"), CM_HIR_EDITION_2024,
        test_span(0u, 500u), &crate_id, &root_module) == CM_HIR_OK);
    i32_type = add_integer_type(&context, CM_HIR_INT_I32, 1u);
    u32_type = add_integer_type(&context, CM_HIR_INT_U32, 3u);
    inner_fields[0] = named_field(&context, "signed_value", i32_type, 10u);
    inner_fields[1] = named_field(&context, "unsigned_value", u32_type,
        12u);
    inner_type = add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "Inner", CM_HIR_AGGREGATE_NAMED,
        inner_fields, 2u, 20u, &inner_definition);
    outer_fields[0] = named_field(&context, "head", u32_type, 40u);
    outer_fields[1] = named_field(&context, "inner", inner_type, 42u);
    outer_fields[2] = named_field(&context, "tail", i32_type, 44u);
    outer_type = add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "Outer", CM_HIR_AGGREGATE_NAMED,
        outer_fields, 3u, 50u, &outer_definition);
    assert(outer_type != CM_HIR_TYPE_NONE);

    assert(cm_hir_layout_named_struct(&context, 32u, outer_definition,
        &layout32, fields32, 3u) == CM_HIR_LAYOUT_OK);
    assert(cm_hir_def_id_equal(layout32.definition, outer_definition));
    assert(layout32.size == 16u && layout32.alignment == 4u
        && layout32.field_count == 3u);
    assert(fields32[0].type == u32_type && fields32[0].offset == 0u
        && fields32[0].size == 4u && fields32[0].alignment == 4u);
    assert(fields32[1].type == inner_type && fields32[1].offset == 4u
        && fields32[1].size == 8u && fields32[1].alignment == 4u);
    assert(fields32[2].type == i32_type && fields32[2].offset == 12u
        && fields32[2].size == 4u && fields32[2].alignment == 4u);
    assert(cm_hir_layout_named_struct(&context, 64u, outer_definition,
        &layout64, fields64, 3u) == CM_HIR_LAYOUT_OK);
    assert(layout64.size == layout32.size
        && layout64.alignment == layout32.alignment
        && layout64.field_count == layout32.field_count);
    assert(memcmp(fields32, fields64, sizeof(fields32)) == 0);

    reordered_fields[0] = named_field(&context, "inner", inner_type, 70u);
    reordered_fields[1] = named_field(&context, "head", u32_type, 72u);
    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "Reordered", CM_HIR_AGGREGATE_NAMED,
        reordered_fields, 2u, 80u, &reordered_definition);
    assert(cm_hir_layout_named_struct(&context, 64u,
        reordered_definition, &layout64, reordered_layout, 2u)
        == CM_HIR_LAYOUT_OK);
    assert(layout64.size == 12u && reordered_layout[0].type == inner_type
        && reordered_layout[0].offset == 0u
        && reordered_layout[0].size == 8u
        && reordered_layout[1].type == u32_type
        && reordered_layout[1].offset == 8u);

    expect_failure_unchanged(&context, outer_definition, 2u,
        CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY);
    memset(&layout64, 0xa5, sizeof(layout64));
    layout32 = layout64;
    assert(cm_hir_layout_named_struct(&context, 64u, outer_definition,
        &layout64, NULL, 3u) == CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY);
    assert(memcmp(&layout64, &layout32, sizeof(layout64)) == 0);
    assert(cm_hir_layout_named_struct(&context, 16u, outer_definition,
        &layout64, fields64, 3u) == CM_HIR_LAYOUT_INVALID_ARGUMENT);
    assert(cm_hir_layout_named_struct(&context, 64u, outer_definition,
        NULL, fields64, 3u) == CM_HIR_LAYOUT_INVALID_ARGUMENT);
    cm_hir_context_destroy(&context);
}

static void test_rejected_shapes_are_atomic(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId foreign_crate;
    CmHirModuleId root_module;
    CmHirModuleId foreign_root;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirTypeId foreign_type;
    CmHirTypeId cycle_a_type;
    CmHirTypeId cycle_b_type;
    CmHirField fields[2];
    CmHirField one_field[1];
    CmHirDefId empty_definition;
    CmHirDefId tuple_definition;
    CmHirDefId union_definition;
    CmHirDefId repr_definition;
    CmHirDefId generic_definition;
    CmHirDefId bad_leaf_definition;
    CmHirDefId foreign_definition;
    CmHirDefId cross_crate_definition;
    CmHirDefId cycle_a_definition;
    CmHirDefId cycle_b_definition;
    CmHirDefId enum_definition;
    CmHirAttribute repr_attribute;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirDefId invalid_definition;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "layout_rejections"),
        CM_HIR_EDITION_2024, test_span(0u, 900u), &crate_id,
        &root_module) == CM_HIR_OK);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "layout_foreign"), CM_HIR_EDITION_2024,
        test_span(901u, 1200u), &foreign_crate, &foreign_root) == CM_HIR_OK);
    u32_type = add_integer_type(&context, CM_HIR_INT_U32, 1u);
    u8_type = add_integer_type(&context, CM_HIR_INT_U8, 3u);

    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "Empty", CM_HIR_AGGREGATE_NAMED, NULL, 0u,
        20u, &empty_definition);
    one_field[0] = tuple_field(u32_type, 40u);
    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "Tuple", CM_HIR_AGGREGATE_TUPLE, one_field,
        1u, 50u, &tuple_definition);
    one_field[0] = named_field(&context, "value", u32_type, 70u);
    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_UNION, "Union", CM_HIR_AGGREGATE_NAMED, one_field,
        1u, 80u, &union_definition);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(100u, 110u), &repr_definition)
        == CM_HIR_OK);
    memset(&repr_attribute, 0, sizeof(repr_attribute));
    repr_attribute.metadata = cm_hir_intern(&context, "repr(C)");
    repr_attribute.span = test_span(100u, 104u);
    repr_attribute.source_attribute = 1u;
    one_field[0] = named_field(&context, "value", u32_type, 105u);
    bind_aggregate(&context, root_module, repr_definition,
        CM_HIR_ITEM_STRUCT, "Represented", CM_HIR_AGGREGATE_NAMED,
        one_field, 1u, &repr_attribute, 1u, CM_HIR_GENERIC_PARAM_NONE, 0u,
        100u);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(130u, 140u), &generic_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = generic_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(131u, 132u);
    assert(cm_hir_add_generic_param(&context, &parameter, &parameter_id)
        == CM_HIR_OK);
    one_field[0] = named_field(&context, "value", u32_type, 135u);
    bind_aggregate(&context, root_module, generic_definition,
        CM_HIR_ITEM_STRUCT, "Generic", CM_HIR_AGGREGATE_NAMED, one_field,
        1u, NULL, 0u, parameter_id, 1u, 130u);

    fields[0] = named_field(&context, "good", u32_type, 160u);
    fields[1] = named_field(&context, "bad", u8_type, 162u);
    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "BadLeaf", CM_HIR_AGGREGATE_NAMED, fields,
        2u, 170u, &bad_leaf_definition);

    one_field[0] = named_field(&context, "value", u32_type, 200u);
    foreign_type = add_aggregate(&context, foreign_crate, foreign_root,
        CM_HIR_ITEM_STRUCT, "Foreign", CM_HIR_AGGREGATE_NAMED, one_field,
        1u, 210u, &foreign_definition);
    one_field[0] = named_field(&context, "foreign", foreign_type, 230u);
    (void)add_aggregate(&context, crate_id, root_module,
        CM_HIR_ITEM_STRUCT, "CrossCrate", CM_HIR_AGGREGATE_NAMED,
        one_field, 1u, 240u, &cross_crate_definition);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(270u, 280u), &cycle_a_definition)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(281u, 291u), &cycle_b_definition)
        == CM_HIR_OK);
    cycle_a_type = add_adt_type(&context, cycle_a_definition, 292u);
    cycle_b_type = add_adt_type(&context, cycle_b_definition, 294u);
    one_field[0] = named_field(&context, "b", cycle_b_type, 296u);
    bind_aggregate(&context, root_module, cycle_a_definition,
        CM_HIR_ITEM_STRUCT, "CycleA", CM_HIR_AGGREGATE_NAMED, one_field,
        1u, NULL, 0u, CM_HIR_GENERIC_PARAM_NONE, 0u, 270u);
    one_field[0] = named_field(&context, "a", cycle_a_type, 298u);
    bind_aggregate(&context, root_module, cycle_b_definition,
        CM_HIR_ITEM_STRUCT, "CycleB", CM_HIR_AGGREGATE_NAMED, one_field,
        1u, NULL, 0u, CM_HIR_GENERIC_PARAM_NONE, 0u, 281u);
    enum_definition = add_empty_enum(&context, crate_id, root_module, 320u);

    expect_failure_unchanged(&context, empty_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, tuple_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, union_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, repr_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, generic_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, bad_leaf_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, cross_crate_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);
    expect_failure_unchanged(&context, cycle_a_definition, 4u,
        CM_HIR_LAYOUT_RECURSIVE_TYPE);
    expect_failure_unchanged(&context, enum_definition, 4u,
        CM_HIR_LAYOUT_UNSUPPORTED_TYPE);

    invalid_definition.crate_id = crate_id;
    invalid_definition.index = UINT32_MAX;
    expect_failure_unchanged(&context, invalid_definition, 4u,
        CM_HIR_LAYOUT_INVALID_DEFINITION);
    expect_failure_unchanged(&context, cm_hir_def_id_none(), 4u,
        CM_HIR_LAYOUT_INVALID_ARGUMENT);
    cm_hir_context_destroy(&context);
}

static void test_status_names(void)
{
    assert(strcmp(cm_hir_layout_status_name(CM_HIR_LAYOUT_OK), "ok") == 0);
    assert(strcmp(cm_hir_layout_status_name(CM_HIR_LAYOUT_INVALID_ARGUMENT),
        "invalid-argument") == 0);
    assert(strcmp(cm_hir_layout_status_name(
        CM_HIR_LAYOUT_INVALID_DEFINITION), "invalid-definition") == 0);
    assert(strcmp(cm_hir_layout_status_name(CM_HIR_LAYOUT_UNSUPPORTED_TYPE),
        "unsupported-type") == 0);
    assert(strcmp(cm_hir_layout_status_name(CM_HIR_LAYOUT_RECURSIVE_TYPE),
        "recursive-type") == 0);
    assert(strcmp(cm_hir_layout_status_name(
        CM_HIR_LAYOUT_INSUFFICIENT_CAPACITY), "insufficient-capacity")
        == 0);
    assert(strcmp(cm_hir_layout_status_name(CM_HIR_LAYOUT_OVERFLOW),
        "overflow") == 0);
    assert(strcmp(cm_hir_layout_status_name((CmHirLayoutStatus)999),
        "unknown") == 0);
}

int main(void)
{
    test_successful_layout();
    test_rejected_shapes_are_atomic();
    test_status_names();
    return 0;
}
