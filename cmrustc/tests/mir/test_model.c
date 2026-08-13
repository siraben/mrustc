#include "cm/mir/model.h"

#include "cm/hir/instance.h"

#include "../../src/hir/instance_internal.h"

#include "cm/alloc.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf oom_jump;

static void jump_on_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    longjmp(oom_jump, 1);
}

typedef struct TestHir {
    CmHirContext context;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirTypeId alternate_u32_type;
    CmHirTypeId i32_type;
    CmHirTypeId shared_u32_type;
    CmHirTypeId mutable_u32_type;
    CmHirTypeId shared_outer_type;
    CmHirDefId identity_definition;
    CmHirBodyId identity_body;
    CmHirDefId probe_definition;
    CmHirBodyId probe_body;
    CmHirDefId add_definition;
    CmHirBodyId add_body;
    CmHirDefId add_max_definition;
    CmHirBodyId add_max_body;
    CmHirDefId add_nested_definition;
    CmHirBodyId add_nested_body;
    CmHirExprId add_nested_root;
    CmHirDefId call_nested_definition;
    CmHirBodyId call_nested_body;
    CmHirExprId call_nested_argument_root;
    CmHirDefId call_mono_definition;
    CmHirBodyId call_mono_body;
    CmHirDefId call_pair_definition;
    CmHirBodyId call_pair_body;
    CmHirExprId call_pair_first_root;
    CmHirExprId call_pair_second_root;
    CmHirExprId call_pair_call_root;
    CmHirDefId nested_calls_definition;
    CmHirBodyId nested_calls_body;
    CmHirExprId nested_calls_inner_call;
    CmHirExprId nested_calls_root;
    CmHirDefId let_definition;
    CmHirBodyId let_body;
    CmHirExprId let_first_input;
    CmHirExprId let_root;
    CmHirDefId inner_definition;
    CmHirTypeId inner_type;
    CmHirDefId outer_definition;
    CmHirTypeId outer_type;
    CmHirDefId aggregate_definition;
    CmHirBodyId aggregate_body;
    CmHirDefId projection_definition;
    CmHirBodyId projection_body;
    CmHirDefId blanket_trait_definition;
    CmHirDefId blanket_declared_method_definition;
    CmHirDefId blanket_impl_definition;
    CmHirDefId blanket_method_definition;
    CmHirBodyId blanket_method_body;
    CmHirTypeId blanket_parameter_type;
} TestHir;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirTypeId add_integer_type(CmHirContext *hir, CmHirIntType kind,
    uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(start, start + 1u);
    type.data.integer_type.kind = kind;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_named_struct(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name,
    const char *const *field_names, const CmHirTypeId *field_types,
    uint32_t field_count, uint32_t start, CmHirDefId *out_definition)
{
    CmHirField fields[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId type_id;
    uint32_t index;

    assert(field_count <= 2u);
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(start, start + 20u), out_definition)
        == CM_HIR_OK);
    memset(fields, 0, sizeof(fields));
    for (index = 0u; index < field_count; ++index) {
        fields[index].name = cm_hir_intern(hir, field_names[index]);
        fields[index].type = field_types[index];
        fields[index].visibility.kind = CM_HIR_VIS_PRIVATE;
        fields[index].visibility.restriction = cm_hir_def_id_none();
        fields[index].span = test_span(start, start + 20u);
    }
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = *out_definition;
    item.owner_module = module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 20u);
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = field_count == 0u ? NULL : fields;
    item.data.aggregate_item.field_count = field_count;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = item.span;
    type.data.named_type.definition = *out_definition;
    assert(cm_hir_add_type(hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static void add_one_argument_function(CmHirContext *hir,
    CmHirModuleId root_module, CmHirDefId definition, const char *name,
    CmHirTypeId parameter_type, CmHirTypeId return_type,
    CmHirGenericParamId generic_start, uint32_t generic_count,
    uint32_t start, CmHirBodyId *out_body)
{
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;

    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(hir, "x");
    parameter.type = parameter_type;
    parameter.span = test_span(start + 5u, start + 10u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;

    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = parameter_type;
    local.span = parameter.span;
    local.parameter_index = 0u;

    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = return_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = start;
    body.span = test_span(start, start + 30u);
    assert(cm_hir_add_body(hir, &body, out_body) == CM_HIR_OK);

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = generic_start;
    item.generic_parameter_count = generic_count;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = return_type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
}

static void add_two_argument_function(CmHirContext *hir,
    CmHirModuleId root_module, CmHirDefId definition, const char *name,
    CmHirTypeId type, uint32_t start, CmHirBodyId *out_body)
{
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t index;

    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    for (index = 0u; index < 2u; ++index) {
        parameters[index].name = cm_hir_intern(hir,
            index == 0u ? "x" : "y");
        parameters[index].type = type;
        parameters[index].span = test_span(start + 5u + index * 5u,
            start + 9u + index * 5u);
        parameters[index].binding_kind = CM_HIR_BINDING_NAMED;
        locals[index].name = parameters[index].name;
        locals[index].type = type;
        locals[index].span = parameters[index].span;
        locals[index].parameter_index = index;
    }

    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = start;
    body.span = test_span(start, start + 50u);
    assert(cm_hir_add_body(hir, &body, out_body) == CM_HIR_OK);

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
}

static void add_one_argument_let_function(CmHirContext *hir,
    CmHirModuleId root_module, CmHirDefId definition, const char *name,
    CmHirTypeId type, uint32_t start, CmHirBodyId *out_body)
{
    CmHirFunctionParameter parameter;
    CmHirLocal locals[3];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;

    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(hir, "x");
    parameter.type = type;
    parameter.span = test_span(start + 5u, start + 6u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;

    memset(locals, 0, sizeof(locals));
    locals[0].name = parameter.name;
    locals[0].type = type;
    locals[0].span = parameter.span;
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(hir, "first");
    locals[1].type = type;
    locals[1].span = test_span(start + 10u, start + 15u);
    locals[1].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
    locals[2].name = cm_hir_intern(hir, "second");
    locals[2].type = type;
    locals[2].span = test_span(start + 30u, start + 36u);
    locals[2].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;

    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = type;
    body.locals = locals;
    body.local_count = 3u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = start;
    body.span = test_span(start, start + 100u);
    assert(cm_hir_add_body(hir, &body, out_body) == CM_HIR_OK);

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
}

static void add_blanket_impl_method(TestHir *fixture,
    CmHirCrateId crate_id, CmHirModuleId root_module)
{
    CmHirFunctionParameter parameter;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirLocal local;
    CmHirBody body;
    CmHirExpr expression;
    CmHirExprId root;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType parameter_type;

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(1100u, 1180u),
        &fixture->blanket_trait_definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = fixture->blanket_trait_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&fixture->context, "Blanket");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(1100u, 1180u);
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->context, &item, &item_id)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(1110u, 1130u),
        &fixture->blanket_declared_method_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->context, "value");
    parameter.type = fixture->u32_type;
    parameter.span = test_span(1115u, 1116u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = fixture->blanket_declared_method_definition;
    item.owner_module = root_module;
    item.parent_definition = fixture->blanket_trait_definition;
    item.name = cm_hir_intern(&fixture->context, "value");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(1110u, 1130u);
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = fixture->u32_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = CM_HIR_BODY_NONE;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&fixture->context, &item, &item_id)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(1200u, 1300u),
        &fixture->blanket_impl_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = fixture->blanket_impl_definition;
    generic.index = 0u;
    generic.name = cm_hir_intern(&fixture->context, "T");
    generic.span = test_span(1205u, 1206u);
    assert(cm_hir_add_generic_param(&fixture->context, &generic,
        &generic_id) == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = generic.span;
    parameter_type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&fixture->context, &parameter_type,
        &fixture->blanket_parameter_type) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = fixture->blanket_impl_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = CM_INTERN_ID_NONE;
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(1200u, 1300u);
    item.generic_parameter_start = generic_id;
    item.generic_parameter_count = 1u;
    item.data.impl_item.self_type = fixture->blanket_parameter_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition =
        fixture->blanket_trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&fixture->context, &item, &item_id)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(1220u, 1280u),
        &fixture->blanket_method_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&fixture->context, "value");
    parameter.type = fixture->blanket_parameter_type;
    parameter.span = test_span(1230u, 1231u);
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&local, 0, sizeof(local));
    local.name = parameter.name;
    local.type = parameter.type;
    local.span = parameter.span;
    local.parameter_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = fixture->blanket_method_definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = fixture->blanket_parameter_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1220u;
    body.span = test_span(1220u, 1280u);
    assert(cm_hir_add_body(&fixture->context, &body,
        &fixture->blanket_method_body) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = fixture->blanket_method_definition;
    item.owner_module = root_module;
    item.parent_definition = fixture->blanket_impl_definition;
    item.name = cm_hir_intern(&fixture->context, "value");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type =
        fixture->blanket_parameter_type;
    item.data.function_item.signature.abi =
        cm_hir_intern(&fixture->context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = fixture->blanket_method_body;
    item.data.function_item.trait_item_definition =
        fixture->blanket_declared_method_definition;
    assert(cm_hir_add_item(&fixture->context, &item, &item_id)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->blanket_method_body;
    expression.type = fixture->blanket_parameter_type;
    expression.span = test_span(1250u, 1251u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->blanket_method_body, root) == CM_HIR_OK);
}

static void test_hir_init(TestHir *fixture)
{
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirType parameter_type;
    CmHirTypeId parameter_type_id;
    CmHirExpr expression;
    CmHirExprId identity_root;
    CmHirExprId probe_argument;
    CmHirExprId probe_call;
    CmHirExprId add_left;
    CmHirExprId add_right;
    CmHirExprId add_root;
    CmHirExprId add_max_left;
    CmHirExprId add_max_right;
    CmHirExprId add_max_root;
    CmHirExprId nested_left;
    CmHirExprId nested_one;
    CmHirExprId nested_right;
    CmHirExprId nested_inner;
    CmHirExprId nested_root;
    CmHirExprId call_nested_left;
    CmHirExprId call_nested_one;
    CmHirExprId call_nested_right;
    CmHirExprId call_nested_inner;
    CmHirExprId call_nested_argument;
    CmHirExprId call_nested_root;
    CmHirExprId call_mono_argument;
    CmHirExprId call_mono_root;
    CmHirExprId pair_first_left;
    CmHirExprId pair_first_one;
    CmHirExprId pair_first_right;
    CmHirExprId pair_first_inner;
    CmHirExprId pair_first_root;
    CmHirExprId pair_second_left;
    CmHirExprId pair_second_two;
    CmHirExprId pair_second_right;
    CmHirExprId pair_second_inner;
    CmHirExprId pair_second_root;
    CmHirExprId call_pair_root;
    CmHirExprId nested_inner_left;
    CmHirExprId nested_inner_one;
    CmHirExprId nested_inner_left_add;
    CmHirExprId nested_inner_right;
    CmHirExprId nested_inner_two;
    CmHirExprId nested_inner_right_add;
    CmHirExprId nested_inner_call;
    CmHirExprId nested_outer_left;
    CmHirExprId nested_outer_three;
    CmHirExprId nested_outer_left_add;
    CmHirExprId nested_outer_call;
    CmHirExprId let_first_one;
    CmHirExprId let_first_initializer;
    CmHirExprId let_first_argument;
    CmHirExprId let_second_initializer;
    CmHirExprId let_tail_second;
    CmHirExprId let_tail_first;
    CmHirExprId let_tail;
    CmHirStatement let_statements[2];
    CmHirTypeId substitutions[1];
    CmHirExprId arguments[2];

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->context);
    assert(cm_hir_create_crate(&fixture->context,
        cm_hir_intern(&fixture->context, "mir_model"),
        CM_HIR_EDITION_2021, test_span(0u, 1000u), &crate_id,
        &root_module) == CM_HIR_OK);
    fixture->u32_type = add_integer_type(&fixture->context,
        CM_HIR_INT_U32, 1u);
    fixture->u8_type = add_integer_type(&fixture->context,
        CM_HIR_INT_U8, 3u);
    fixture->alternate_u32_type = add_integer_type(&fixture->context,
        CM_HIR_INT_U32, 5u);
    fixture->i32_type = add_integer_type(&fixture->context,
        CM_HIR_INT_I32, 7u);
    {
        CmHirType reference;

        memset(&reference, 0, sizeof(reference));
        reference.kind = CM_HIR_TYPE_REFERENCE_KIND;
        reference.span = test_span(8u, 9u);
        reference.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
        reference.data.reference_type.pointee = fixture->u32_type;
        reference.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_hir_add_type(&fixture->context, &reference,
            &fixture->shared_u32_type) == CM_HIR_OK);
        reference.span = test_span(9u, 10u);
        reference.data.reference_type.mutability = CM_HIR_MUTABLE;
        assert(cm_hir_add_type(&fixture->context, &reference,
            &fixture->mutable_u32_type) == CM_HIR_OK);
    }

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 50u),
        &fixture->identity_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = fixture->identity_definition;
    generic.index = 0u;
    generic.name = cm_hir_intern(&fixture->context, "T");
    generic.span = test_span(15u, 16u);
    assert(cm_hir_add_generic_param(&fixture->context, &generic,
        &generic_id) == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = generic.span;
    parameter_type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&fixture->context, &parameter_type,
        &parameter_type_id) == CM_HIR_OK);
    add_one_argument_function(&fixture->context, root_module,
        fixture->identity_definition, "identity", parameter_type_id,
        parameter_type_id, generic_id, 1u, 10u,
        &fixture->identity_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->identity_body;
    expression.type = parameter_type_id;
    expression.span = test_span(30u, 31u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &identity_root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->identity_body, identity_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(70u, 120u),
        &fixture->probe_definition) == CM_HIR_OK);
    add_one_argument_function(&fixture->context, root_module,
        fixture->probe_definition, "probe", fixture->u32_type,
        fixture->u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u, 70u,
        &fixture->probe_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->probe_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(85u, 86u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &probe_argument)
        == CM_HIR_OK);
    substitutions[0] = fixture->u32_type;
    arguments[0] = probe_argument;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->probe_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(80u, 95u);
    expression.data.call.callee = fixture->identity_definition;
    expression.data.call.type_substitutions = substitutions;
    expression.data.call.type_substitution_count = 1u;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &probe_call)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->probe_body, probe_call) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(130u, 190u),
        &fixture->add_definition) == CM_HIR_OK);
    add_two_argument_function(&fixture->context, root_module,
        fixture->add_definition, "add", fixture->u32_type, 130u,
        &fixture->add_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->add_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(150u, 151u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_left)
        == CM_HIR_OK);
    expression.span = test_span(152u, 153u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_right)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->add_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(145u, 160u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = add_left;
    expression.data.binary.right = add_right;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->add_body, add_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(200u, 240u),
        &fixture->add_max_definition) == CM_HIR_OK);
    add_one_argument_function(&fixture->context, root_module,
        fixture->add_max_definition, "add_max", fixture->u32_type,
        fixture->u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u, 200u,
        &fixture->add_max_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->add_max_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(215u, 216u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_max_left)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->add_max_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(217u, 227u);
    expression.data.integer.low_bits = (uint64_t)UINT32_MAX;
    expression.data.integer.high_bits = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_max_right)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->add_max_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(210u, 228u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = add_max_left;
    expression.data.binary.right = add_max_right;
    assert(cm_hir_add_expr(&fixture->context, &expression, &add_max_root)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->add_max_body, add_max_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(245u, 295u),
        &fixture->add_nested_definition) == CM_HIR_OK);
    add_two_argument_function(&fixture->context, root_module,
        fixture->add_nested_definition, "add_nested", fixture->u32_type,
        245u, &fixture->add_nested_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->add_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(252u, 253u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &nested_left)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->add_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(263u, 264u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &nested_one)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->add_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(270u, 271u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression, &nested_right)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->add_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(260u, 280u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = nested_one;
    expression.data.binary.right = nested_right;
    assert(cm_hir_add_expr(&fixture->context, &expression, &nested_inner)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->add_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(250u, 285u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = nested_left;
    expression.data.binary.right = nested_inner;
    assert(cm_hir_add_expr(&fixture->context, &expression, &nested_root)
        == CM_HIR_OK);
    fixture->add_nested_root = nested_root;
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->add_nested_body, nested_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(300u, 380u),
        &fixture->call_nested_definition) == CM_HIR_OK);
    add_two_argument_function(&fixture->context, root_module,
        fixture->call_nested_definition, "call_nested", fixture->u32_type,
        300u, &fixture->call_nested_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(315u, 316u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(320u, 321u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_one) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(325u, 326u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(319u, 330u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = call_nested_one;
    expression.data.binary.right = call_nested_right;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_inner) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(314u, 331u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = call_nested_left;
    expression.data.binary.right = call_nested_inner;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_argument) == CM_HIR_OK);
    fixture->call_nested_argument_root = call_nested_argument;
    substitutions[0] = fixture->u32_type;
    arguments[0] = call_nested_argument;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->call_nested_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(310u, 345u);
    expression.data.call.callee = fixture->identity_definition;
    expression.data.call.type_substitutions = substitutions;
    expression.data.call.type_substitution_count = 1u;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_nested_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->call_nested_body, call_nested_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(390u, 440u),
        &fixture->call_mono_definition) == CM_HIR_OK);
    add_one_argument_function(&fixture->context, root_module,
        fixture->call_mono_definition, "call_mono", fixture->u32_type,
        fixture->u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u, 390u,
        &fixture->call_mono_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_mono_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(405u, 406u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_mono_argument) == CM_HIR_OK);
    arguments[0] = call_mono_argument;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->call_mono_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(400u, 420u);
    expression.data.call.callee = fixture->add_max_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_mono_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->call_mono_body, call_mono_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(450u, 570u),
        &fixture->call_pair_definition) == CM_HIR_OK);
    add_two_argument_function(&fixture->context, root_module,
        fixture->call_pair_definition, "call_pair", fixture->u32_type,
        450u, &fixture->call_pair_body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(465u, 466u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_first_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(470u, 471u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_first_one) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(475u, 476u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_first_right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(469u, 480u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = pair_first_one;
    expression.data.binary.right = pair_first_right;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_first_inner) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(464u, 481u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = pair_first_left;
    expression.data.binary.right = pair_first_inner;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_first_root) == CM_HIR_OK);
    fixture->call_pair_first_root = pair_first_root;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(483u, 484u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_second_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(486u, 487u);
    expression.data.integer.low_bits = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_second_two) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(490u, 491u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_second_right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(482u, 488u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = pair_second_left;
    expression.data.binary.right = pair_second_two;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_second_inner) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(481u, 493u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = pair_second_inner;
    expression.data.binary.right = pair_second_right;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &pair_second_root) == CM_HIR_OK);
    fixture->call_pair_second_root = pair_second_root;

    arguments[0] = pair_first_root;
    arguments[1] = pair_second_root;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->call_pair_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(455u, 495u);
    expression.data.call.callee = fixture->add_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &call_pair_root) == CM_HIR_OK);
    fixture->call_pair_call_root = call_pair_root;
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->call_pair_body, call_pair_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(600u, 750u),
        &fixture->nested_calls_definition) == CM_HIR_OK);
    add_two_argument_function(&fixture->context, root_module,
        fixture->nested_calls_definition, "nested_calls",
        fixture->u32_type, 600u, &fixture->nested_calls_body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(610u, 611u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(612u, 613u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_one) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(609u, 614u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = nested_inner_left;
    expression.data.binary.right = nested_inner_one;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_left_add) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(616u, 617u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(618u, 619u);
    expression.data.integer.low_bits = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_two) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(615u, 620u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = nested_inner_right;
    expression.data.binary.right = nested_inner_two;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_right_add) == CM_HIR_OK);

    arguments[0] = nested_inner_left_add;
    arguments[1] = nested_inner_right_add;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(608u, 622u);
    expression.data.call.callee = fixture->add_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_inner_call) == CM_HIR_OK);
    fixture->nested_calls_inner_call = nested_inner_call;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(625u, 626u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_outer_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(627u, 628u);
    expression.data.integer.low_bits = 3u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_outer_three) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(624u, 629u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = nested_outer_left;
    expression.data.binary.right = nested_outer_three;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_outer_left_add) == CM_HIR_OK);

    arguments[0] = nested_inner_call;
    arguments[1] = nested_outer_left_add;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->nested_calls_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(605u, 632u);
    expression.data.call.callee = fixture->add_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &nested_outer_call) == CM_HIR_OK);
    fixture->nested_calls_root = nested_outer_call;
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->nested_calls_body, nested_outer_call) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(800u, 920u),
        &fixture->let_definition) == CM_HIR_OK);
    add_one_argument_let_function(&fixture->context, root_module,
        fixture->let_definition, "let_replay", fixture->u32_type, 800u,
        &fixture->let_body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(812u, 813u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &fixture->let_first_input) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(816u, 817u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_first_one) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(811u, 818u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->let_first_input;
    expression.data.binary.right = let_first_one;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_first_initializer) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(833u, 838u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_first_argument) == CM_HIR_OK);
    arguments[0] = let_first_argument;
    arguments[1] = let_first_argument;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(829u, 846u);
    expression.data.call.callee = fixture->add_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_second_initializer) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(853u, 859u);
    expression.data.local.local_index = 2u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_tail_second) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(862u, 867u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &let_tail_first) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(852u, 868u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = let_tail_second;
    expression.data.binary.right = let_tail_first;
    assert(cm_hir_add_expr(&fixture->context, &expression, &let_tail)
        == CM_HIR_OK);

    memset(let_statements, 0, sizeof(let_statements));
    let_statements[0].kind = CM_HIR_STATEMENT_LET;
    let_statements[0].span = test_span(807u, 820u);
    let_statements[0].data.let_statement.local_index = 1u;
    let_statements[0].data.let_statement.initializer =
        let_first_initializer;
    let_statements[1].kind = CM_HIR_STATEMENT_LET;
    let_statements[1].span = test_span(825u, 848u);
    let_statements[1].data.let_statement.local_index = 2u;
    let_statements[1].data.let_statement.initializer =
        let_second_initializer;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.owner_body = fixture->let_body;
    expression.type = fixture->u32_type;
    expression.span = test_span(805u, 870u);
    expression.data.block.statements = let_statements;
    expression.data.block.statement_count = 2u;
    expression.data.block.tail_expression = let_tail;
    assert(cm_hir_add_expr(&fixture->context, &expression,
        &fixture->let_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&fixture->context,
        fixture->let_body, fixture->let_root) == CM_HIR_OK);

    {
        static const char *const inner_names[1] = { "value" };
        static const char *const outer_names[2] = { "left", "inner" };
        CmHirTypeId inner_fields[1];
        CmHirTypeId outer_fields[2];
        CmHirExprId inner_value;
        CmHirExprId inner_aggregate;
        CmHirExprId outer_left;
        CmHirExprId outer_aggregate;
        CmHirExprId projection_base;
        CmHirExprId projection_inner;
        CmHirAggregateFieldValue inner_values[1];
        CmHirAggregateFieldValue outer_values[2];

        inner_fields[0] = fixture->u32_type;
        fixture->inner_type = add_named_struct(&fixture->context, crate_id,
            root_module, "Inner", inner_names, inner_fields, 1u, 930u,
            &fixture->inner_definition);
        outer_fields[0] = fixture->u32_type;
        outer_fields[1] = fixture->inner_type;
        fixture->outer_type = add_named_struct(&fixture->context, crate_id,
            root_module, "Outer", outer_names, outer_fields, 2u, 952u,
            &fixture->outer_definition);
        {
            CmHirType reference;

            memset(&reference, 0, sizeof(reference));
            reference.kind = CM_HIR_TYPE_REFERENCE_KIND;
            reference.span = test_span(974u, 976u);
            reference.data.reference_type.region.kind =
                CM_HIR_REGION_ERASED;
            reference.data.reference_type.pointee = fixture->outer_type;
            reference.data.reference_type.mutability = CM_HIR_IMMUTABLE;
            assert(cm_hir_add_type(&fixture->context, &reference,
                &fixture->shared_outer_type) == CM_HIR_OK);
        }

        assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
            CM_HIR_ITEM_FUNCTION, test_span(700u, 730u),
            &fixture->aggregate_definition) == CM_HIR_OK);
        add_one_argument_function(&fixture->context, root_module,
            fixture->aggregate_definition, "make_outer", fixture->u32_type,
            fixture->outer_type, CM_HIR_GENERIC_PARAM_NONE, 0u, 700u,
            &fixture->aggregate_body);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_LOCAL;
        expression.owner_body = fixture->aggregate_body;
        expression.type = fixture->u32_type;
        expression.span = test_span(710u, 711u);
        expression.data.local.local_index = 0u;
        assert(cm_hir_add_expr(&fixture->context, &expression, &inner_value)
            == CM_HIR_OK);
        memset(inner_values, 0, sizeof(inner_values));
        inner_values[0].field_index = 0u;
        inner_values[0].value = inner_value;
        inner_values[0].span = test_span(709u, 714u);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_AGGREGATE;
        expression.owner_body = fixture->aggregate_body;
        expression.type = fixture->inner_type;
        expression.span = test_span(708u, 715u);
        expression.data.aggregate.definition = fixture->inner_definition;
        expression.data.aggregate.fields = inner_values;
        expression.data.aggregate.field_count = 1u;
        assert(cm_hir_add_expr(&fixture->context, &expression,
            &inner_aggregate) == CM_HIR_OK);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_LOCAL;
        expression.owner_body = fixture->aggregate_body;
        expression.type = fixture->u32_type;
        expression.span = test_span(720u, 721u);
        expression.data.local.local_index = 0u;
        assert(cm_hir_add_expr(&fixture->context, &expression, &outer_left)
            == CM_HIR_OK);
        memset(outer_values, 0, sizeof(outer_values));
        outer_values[0].field_index = 1u;
        outer_values[0].value = inner_aggregate;
        outer_values[0].span = test_span(707u, 716u);
        outer_values[1].field_index = 0u;
        outer_values[1].value = outer_left;
        outer_values[1].span = test_span(718u, 722u);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_AGGREGATE;
        expression.owner_body = fixture->aggregate_body;
        expression.type = fixture->outer_type;
        expression.span = test_span(705u, 725u);
        expression.data.aggregate.definition = fixture->outer_definition;
        expression.data.aggregate.fields = outer_values;
        expression.data.aggregate.field_count = 2u;
        assert(cm_hir_add_expr(&fixture->context, &expression,
            &outer_aggregate) == CM_HIR_OK);
        assert(cm_hir_set_body_root_expression(&fixture->context,
            fixture->aggregate_body, outer_aggregate) == CM_HIR_OK);

        assert(cm_hir_reserve_item_definition_as(&fixture->context, crate_id,
            CM_HIR_ITEM_FUNCTION, test_span(740u, 770u),
            &fixture->projection_definition) == CM_HIR_OK);
        add_one_argument_function(&fixture->context, root_module,
            fixture->projection_definition, "project_value",
            fixture->outer_type, fixture->u32_type,
            CM_HIR_GENERIC_PARAM_NONE, 0u, 740u,
            &fixture->projection_body);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_LOCAL;
        expression.owner_body = fixture->projection_body;
        expression.type = fixture->outer_type;
        expression.span = test_span(748u, 749u);
        expression.data.local.local_index = 0u;
        assert(cm_hir_add_expr(&fixture->context, &expression,
            &projection_base) == CM_HIR_OK);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_FIELD;
        expression.owner_body = fixture->projection_body;
        expression.type = fixture->inner_type;
        expression.span = test_span(748u, 755u);
        expression.data.field.base = projection_base;
        expression.data.field.definition = fixture->outer_definition;
        expression.data.field.field_index = 1u;
        assert(cm_hir_add_expr(&fixture->context, &expression,
            &projection_inner) == CM_HIR_OK);
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_FIELD;
        expression.owner_body = fixture->projection_body;
        expression.type = fixture->u32_type;
        expression.span = test_span(748u, 762u);
        expression.data.field.base = projection_inner;
        expression.data.field.definition = fixture->inner_definition;
        expression.data.field.field_index = 0u;
        assert(cm_hir_add_expr(&fixture->context, &expression,
            &outer_aggregate) == CM_HIR_OK);
        assert(cm_hir_set_body_root_expression(&fixture->context,
            fixture->projection_body, outer_aggregate) == CM_HIR_OK);
    }
}

static void init_identity_mir(CmMirBody *body, CmMirLocal locals[2],
    CmMirStatement *statement, CmMirBasicBlock *block,
    const TestHir *fixture, CmHirTypeId *substitution, CmHirTypeId type)
{
    memset(locals, 0, 2u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = type;

    memset(statement, 0, sizeof(*statement));
    statement->kind = CM_MIR_STATEMENT_ASSIGN;
    statement->data.assign.destination = 0u;
    statement->data.assign.value.kind = CM_MIR_RVALUE_USE;
    statement->data.assign.value.type = type;
    statement->data.assign.value.data.use.kind = CM_MIR_OPERAND_MOVE;
    statement->data.assign.value.data.use.type = type;
    statement->data.assign.value.data.use.data.local = 1u;

    memset(block, 0, sizeof(*block));
    block->statements = statement;
    block->statement_count = 1u;
    block->terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->identity_definition;
    body->instance.substitutions = substitution;
    body->instance.substitution_count = 1u;
    body->owner = fixture->identity_definition;
    body->source_body = fixture->identity_body;
    body->locals = locals;
    body->local_count = 2u;
    body->basic_blocks = block;
    body->basic_block_count = 1u;
}

static void init_probe_mir(CmMirBody *body, CmMirLocal locals[2],
    CmMirOperand *argument, CmHirTypeId *callee_substitution,
    CmMirBasicBlock blocks[2], const TestHir *fixture,
    CmMirBodyId callee_id)
{
    memset(locals, 0, 2u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = fixture->u32_type;

    memset(argument, 0, sizeof(*argument));
    argument->kind = CM_MIR_OPERAND_MOVE;
    argument->type = fixture->u32_type;
    argument->data.local = 1u;

    memset(blocks, 0, 2u * sizeof(*blocks));
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = 0u;
    blocks[0].terminator.data.call.arguments = argument;
    blocks[0].terminator.data.call.argument_count = 1u;
    blocks[0].terminator.data.call.callee_instance = callee_id;
    blocks[0].terminator.data.call.callee.definition =
        fixture->identity_definition;
    blocks[0].terminator.data.call.callee.substitutions =
        callee_substitution;
    blocks[0].terminator.data.call.callee.substitution_count = 1u;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->probe_definition;
    body->owner = fixture->probe_definition;
    body->source_body = fixture->probe_body;
    body->locals = locals;
    body->local_count = 2u;
    body->basic_blocks = blocks;
    body->basic_block_count = 2u;
}

static void init_add_mir(CmMirBody *body, CmMirLocal locals[3],
    CmMirStatement *statement, CmMirBasicBlock *block,
    const TestHir *fixture)
{
    uint32_t index;

    memset(locals, 0, 3u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    for (index = 1u; index < 3u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_ARGUMENT;
        locals[index].type = fixture->u32_type;
    }
    memset(statement, 0, sizeof(*statement));
    statement->kind = CM_MIR_STATEMENT_ASSIGN;
    statement->data.assign.destination = CM_MIR_RETURN_LOCAL;
    statement->data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statement->data.assign.value.type = fixture->u32_type;
    statement->data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statement->data.assign.value.data.binary.left.kind = CM_MIR_OPERAND_MOVE;
    statement->data.assign.value.data.binary.left.type = fixture->u32_type;
    statement->data.assign.value.data.binary.left.data.local = 1u;
    statement->data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statement->data.assign.value.data.binary.right.type = fixture->u32_type;
    statement->data.assign.value.data.binary.right.data.local = 2u;
    memset(block, 0, sizeof(*block));
    block->statements = statement;
    block->statement_count = 1u;
    block->terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->add_definition;
    body->owner = fixture->add_definition;
    body->source_body = fixture->add_body;
    body->locals = locals;
    body->local_count = 3u;
    body->basic_blocks = block;
    body->basic_block_count = 1u;
}

static void init_let_mir(CmMirBody *body, CmMirLocal locals[4],
    CmMirStatement statements[2], CmMirOperand arguments[2],
    CmMirBasicBlock blocks[2], const TestHir *fixture,
    CmMirBodyId add_id)
{
    uint32_t index;

    memset(locals, 0, 4u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = fixture->u32_type;
    for (index = 2u; index < 4u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_USER;
        locals[index].type = fixture->u32_type;
    }

    memset(statements, 0, 2u * sizeof(*statements));
    statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[0].data.assign.destination = 2u;
    statements[0].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[0].data.assign.value.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[0].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[0].data.assign.value.data.binary.left.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.left.data.local = 1u;
    statements[0].data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statements[0].data.assign.value.data.binary.right.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.right.data.u32_value = 1u;

    statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[1].data.assign.destination = CM_MIR_RETURN_LOCAL;
    statements[1].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[1].data.assign.value.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[1].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.left.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.left.data.local = 3u;
    statements[1].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.right.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.right.data.local = 2u;

    memset(arguments, 0, 2u * sizeof(*arguments));
    for (index = 0u; index < 2u; ++index) {
        arguments[index].kind = CM_MIR_OPERAND_MOVE;
        arguments[index].type = fixture->u32_type;
        arguments[index].data.local = 2u;
    }

    memset(blocks, 0, 2u * sizeof(*blocks));
    blocks[0].statements = &statements[0];
    blocks[0].statement_count = 1u;
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = 3u;
    blocks[0].terminator.data.call.arguments = arguments;
    blocks[0].terminator.data.call.argument_count = 2u;
    blocks[0].terminator.data.call.callee_instance = add_id;
    blocks[0].terminator.data.call.callee.definition = fixture->add_definition;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].statements = &statements[1];
    blocks[1].statement_count = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->let_definition;
    body->owner = fixture->let_definition;
    body->source_body = fixture->let_body;
    body->locals = locals;
    body->local_count = 4u;
    body->basic_blocks = blocks;
    body->basic_block_count = 2u;
}

static void init_add_max_mir(CmMirBody *body, CmMirLocal locals[2],
    CmMirStatement *statement, CmMirBasicBlock *block,
    const TestHir *fixture)
{
    memset(locals, 0, 2u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = fixture->u32_type;
    memset(statement, 0, sizeof(*statement));
    statement->kind = CM_MIR_STATEMENT_ASSIGN;
    statement->data.assign.destination = CM_MIR_RETURN_LOCAL;
    statement->data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statement->data.assign.value.type = fixture->u32_type;
    statement->data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statement->data.assign.value.data.binary.left.kind = CM_MIR_OPERAND_MOVE;
    statement->data.assign.value.data.binary.left.type = fixture->u32_type;
    statement->data.assign.value.data.binary.left.data.local = 1u;
    statement->data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statement->data.assign.value.data.binary.right.type = fixture->u32_type;
    statement->data.assign.value.data.binary.right.data.u32_value =
        UINT32_MAX;
    memset(block, 0, sizeof(*block));
    block->statements = statement;
    block->statement_count = 1u;
    block->terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->add_max_definition;
    body->owner = fixture->add_max_definition;
    body->source_body = fixture->add_max_body;
    body->locals = locals;
    body->local_count = 2u;
    body->basic_blocks = block;
    body->basic_block_count = 1u;
}

static void init_add_nested_mir(CmMirBody *body, CmMirLocal locals[4],
    CmMirStatement statements[2], CmMirBasicBlock *block,
    const TestHir *fixture)
{
    uint32_t index;

    memset(locals, 0, 4u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    for (index = 1u; index < 3u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_ARGUMENT;
        locals[index].type = fixture->u32_type;
    }
    locals[3].kind = CM_MIR_LOCAL_TEMPORARY;
    locals[3].type = fixture->u32_type;

    memset(statements, 0, 2u * sizeof(*statements));
    statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[0].data.assign.destination = 3u;
    statements[0].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[0].data.assign.value.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[0].data.assign.value.data.binary.left.kind =
        CM_MIR_CONSTANT_U32;
    statements[0].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.left.data.u32_value = 1u;
    statements[0].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[0].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.right.data.local = 2u;

    statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[1].data.assign.destination = CM_MIR_RETURN_LOCAL;
    statements[1].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[1].data.assign.value.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[1].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.left.data.local = 1u;
    statements[1].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.right.data.local = 3u;

    memset(block, 0, sizeof(*block));
    block->statements = statements;
    block->statement_count = 2u;
    block->terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->add_nested_definition;
    body->owner = fixture->add_nested_definition;
    body->source_body = fixture->add_nested_body;
    body->locals = locals;
    body->local_count = 4u;
    body->basic_blocks = block;
    body->basic_block_count = 1u;
}

static void init_call_nested_mir(CmMirBody *body, CmMirLocal locals[5],
    CmMirStatement statements[2], CmMirOperand *argument,
    CmHirTypeId *callee_substitution, CmMirBasicBlock blocks[2],
    const TestHir *fixture, CmMirBodyId callee_id)
{
    uint32_t index;

    memset(locals, 0, 5u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    for (index = 1u; index < 3u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_ARGUMENT;
        locals[index].type = fixture->u32_type;
    }
    for (index = 3u; index < 5u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_TEMPORARY;
        locals[index].type = fixture->u32_type;
    }

    memset(statements, 0, 2u * sizeof(*statements));
    statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[0].data.assign.destination = 3u;
    statements[0].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[0].data.assign.value.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[0].data.assign.value.data.binary.left.kind =
        CM_MIR_CONSTANT_U32;
    statements[0].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.left.data.u32_value = 1u;
    statements[0].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[0].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.right.data.local = 2u;

    statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[1].data.assign.destination = 4u;
    statements[1].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[1].data.assign.value.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[1].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.left.data.local = 1u;
    statements[1].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.right.data.local = 3u;

    memset(argument, 0, sizeof(*argument));
    argument->kind = CM_MIR_OPERAND_MOVE;
    argument->type = fixture->u32_type;
    argument->data.local = 4u;

    memset(blocks, 0, 2u * sizeof(*blocks));
    blocks[0].statements = statements;
    blocks[0].statement_count = 2u;
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = CM_MIR_RETURN_LOCAL;
    blocks[0].terminator.data.call.arguments = argument;
    blocks[0].terminator.data.call.argument_count = 1u;
    blocks[0].terminator.data.call.callee_instance = callee_id;
    blocks[0].terminator.data.call.callee.definition =
        fixture->identity_definition;
    blocks[0].terminator.data.call.callee.substitutions =
        callee_substitution;
    blocks[0].terminator.data.call.callee.substitution_count = 1u;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->call_nested_definition;
    body->owner = fixture->call_nested_definition;
    body->source_body = fixture->call_nested_body;
    body->locals = locals;
    body->local_count = 5u;
    body->basic_blocks = blocks;
    body->basic_block_count = 2u;
}

static void init_call_mono_mir(CmMirBody *body, CmMirLocal locals[2],
    CmMirOperand *argument, CmMirBasicBlock blocks[2],
    const TestHir *fixture, CmMirBodyId callee_id)
{
    memset(locals, 0, 2u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = fixture->u32_type;
    memset(argument, 0, sizeof(*argument));
    argument->kind = CM_MIR_OPERAND_MOVE;
    argument->type = fixture->u32_type;
    argument->data.local = 1u;
    memset(blocks, 0, 2u * sizeof(*blocks));
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = CM_MIR_RETURN_LOCAL;
    blocks[0].terminator.data.call.arguments = argument;
    blocks[0].terminator.data.call.argument_count = 1u;
    blocks[0].terminator.data.call.callee_instance = callee_id;
    blocks[0].terminator.data.call.callee.definition =
        fixture->add_max_definition;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->call_mono_definition;
    body->owner = fixture->call_mono_definition;
    body->source_body = fixture->call_mono_body;
    body->locals = locals;
    body->local_count = 2u;
    body->basic_blocks = blocks;
    body->basic_block_count = 2u;
}

static void init_call_pair_mir(CmMirBody *body, CmMirLocal locals[7],
    CmMirStatement statements[4], CmMirOperand arguments[2],
    CmMirBasicBlock blocks[2], const TestHir *fixture,
    CmMirBodyId callee_id)
{
    uint32_t index;

    memset(locals, 0, 7u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    for (index = 1u; index < 3u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_ARGUMENT;
        locals[index].type = fixture->u32_type;
    }
    for (index = 3u; index < 7u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_TEMPORARY;
        locals[index].type = fixture->u32_type;
    }

    memset(statements, 0, 4u * sizeof(*statements));
    statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[0].data.assign.destination = 3u;
    statements[0].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[0].data.assign.value.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[0].data.assign.value.data.binary.left.kind =
        CM_MIR_CONSTANT_U32;
    statements[0].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.left.data.u32_value = 1u;
    statements[0].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[0].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.right.data.local = 2u;

    statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[1].data.assign.destination = 4u;
    statements[1].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[1].data.assign.value.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[1].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.left.data.local = 1u;
    statements[1].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.right.data.local = 3u;

    statements[2].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[2].data.assign.destination = 5u;
    statements[2].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[2].data.assign.value.type = fixture->u32_type;
    statements[2].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[2].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[2].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[2].data.assign.value.data.binary.left.data.local = 1u;
    statements[2].data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statements[2].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[2].data.assign.value.data.binary.right.data.u32_value = 2u;

    statements[3].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[3].data.assign.destination = 6u;
    statements[3].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[3].data.assign.value.type = fixture->u32_type;
    statements[3].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[3].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[3].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[3].data.assign.value.data.binary.left.data.local = 5u;
    statements[3].data.assign.value.data.binary.right.kind =
        CM_MIR_OPERAND_MOVE;
    statements[3].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[3].data.assign.value.data.binary.right.data.local = 2u;

    memset(arguments, 0, 2u * sizeof(*arguments));
    arguments[0].kind = CM_MIR_OPERAND_MOVE;
    arguments[0].type = fixture->u32_type;
    arguments[0].data.local = 4u;
    arguments[1].kind = CM_MIR_OPERAND_MOVE;
    arguments[1].type = fixture->u32_type;
    arguments[1].data.local = 6u;

    memset(blocks, 0, 2u * sizeof(*blocks));
    blocks[0].statements = statements;
    blocks[0].statement_count = 4u;
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = CM_MIR_RETURN_LOCAL;
    blocks[0].terminator.data.call.arguments = arguments;
    blocks[0].terminator.data.call.argument_count = 2u;
    blocks[0].terminator.data.call.callee_instance = callee_id;
    blocks[0].terminator.data.call.callee.definition =
        fixture->add_definition;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->call_pair_definition;
    body->owner = fixture->call_pair_definition;
    body->source_body = fixture->call_pair_body;
    body->locals = locals;
    body->local_count = 7u;
    body->basic_blocks = blocks;
    body->basic_block_count = 2u;
}

static void init_nested_calls_mir(CmMirBody *body, CmMirLocal locals[7],
    CmMirStatement statements[3], CmMirOperand inner_arguments[2],
    CmMirOperand outer_arguments[2], CmMirBasicBlock blocks[3],
    const TestHir *fixture, CmMirBodyId callee_id)
{
    uint32_t index;

    memset(locals, 0, 7u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    for (index = 1u; index < 3u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_ARGUMENT;
        locals[index].type = fixture->u32_type;
    }
    for (index = 3u; index < 7u; ++index) {
        locals[index].kind = CM_MIR_LOCAL_TEMPORARY;
        locals[index].type = fixture->u32_type;
    }

    memset(statements, 0, 3u * sizeof(*statements));
    statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[0].data.assign.destination = 3u;
    statements[0].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[0].data.assign.value.type = fixture->u32_type;
    statements[0].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[0].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[0].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.left.data.local = 1u;
    statements[0].data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statements[0].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[0].data.assign.value.data.binary.right.data.u32_value = 1u;

    statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[1].data.assign.destination = 4u;
    statements[1].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[1].data.assign.value.type = fixture->u32_type;
    statements[1].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[1].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[1].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.left.data.local = 2u;
    statements[1].data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statements[1].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[1].data.assign.value.data.binary.right.data.u32_value = 2u;

    statements[2].kind = CM_MIR_STATEMENT_ASSIGN;
    statements[2].data.assign.destination = 6u;
    statements[2].data.assign.value.kind = CM_MIR_RVALUE_BINARY;
    statements[2].data.assign.value.type = fixture->u32_type;
    statements[2].data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_ADD;
    statements[2].data.assign.value.data.binary.left.kind =
        CM_MIR_OPERAND_MOVE;
    statements[2].data.assign.value.data.binary.left.type =
        fixture->u32_type;
    statements[2].data.assign.value.data.binary.left.data.local = 1u;
    statements[2].data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    statements[2].data.assign.value.data.binary.right.type =
        fixture->u32_type;
    statements[2].data.assign.value.data.binary.right.data.u32_value = 3u;

    memset(inner_arguments, 0, 2u * sizeof(*inner_arguments));
    inner_arguments[0].kind = CM_MIR_OPERAND_MOVE;
    inner_arguments[0].type = fixture->u32_type;
    inner_arguments[0].data.local = 3u;
    inner_arguments[1].kind = CM_MIR_OPERAND_MOVE;
    inner_arguments[1].type = fixture->u32_type;
    inner_arguments[1].data.local = 4u;
    memset(outer_arguments, 0, 2u * sizeof(*outer_arguments));
    outer_arguments[0].kind = CM_MIR_OPERAND_MOVE;
    outer_arguments[0].type = fixture->u32_type;
    outer_arguments[0].data.local = 5u;
    outer_arguments[1].kind = CM_MIR_OPERAND_MOVE;
    outer_arguments[1].type = fixture->u32_type;
    outer_arguments[1].data.local = 6u;

    memset(blocks, 0, 3u * sizeof(*blocks));
    blocks[0].statements = statements;
    blocks[0].statement_count = 2u;
    blocks[0].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[0].terminator.data.call.destination = 5u;
    blocks[0].terminator.data.call.arguments = inner_arguments;
    blocks[0].terminator.data.call.argument_count = 2u;
    blocks[0].terminator.data.call.callee_instance = callee_id;
    blocks[0].terminator.data.call.callee.definition =
        fixture->add_definition;
    blocks[0].terminator.data.call.target = 1u;
    blocks[1].statements = &statements[2];
    blocks[1].statement_count = 1u;
    blocks[1].terminator.kind = CM_MIR_TERMINATOR_CALL;
    blocks[1].terminator.data.call.destination = CM_MIR_RETURN_LOCAL;
    blocks[1].terminator.data.call.arguments = outer_arguments;
    blocks[1].terminator.data.call.argument_count = 2u;
    blocks[1].terminator.data.call.callee_instance = callee_id;
    blocks[1].terminator.data.call.callee.definition =
        fixture->add_definition;
    blocks[1].terminator.data.call.target = 2u;
    blocks[2].terminator.kind = CM_MIR_TERMINATOR_RETURN;

    memset(body, 0, sizeof(*body));
    body->instance.definition = fixture->nested_calls_definition;
    body->owner = fixture->nested_calls_definition;
    body->source_body = fixture->nested_calls_body;
    body->locals = locals;
    body->local_count = 7u;
    body->basic_blocks = blocks;
    body->basic_block_count = 3u;
}

static void assert_rejected(CmMirContext *mir, const CmHirContext *hir,
    const CmMirBody *body, CmMirStatus expected)
{
    size_t count;
    CmMirBodyId id;

    count = cm_mir_body_count(mir);
    id = 99u;
    assert(cm_mir_add_monomorphized_body(mir, hir, body, &id) == expected);
    assert(id == CM_MIR_BODY_NONE && cm_mir_body_count(mir) == count);
}

static void assert_nested_model(CmMirContext *mir, TestHir *fixture)
{
    CmMirBody body;
    CmMirLocal locals[4];
    CmMirStatement statements[2];
    CmMirBasicBlock block;
    CmMirBodyId id;
    const CmMirBody *stored;
    CmMirContext oom_mir;
    CmMirBodyId oom_id;
    CmMirBody invalid;
    CmMirLocal invalid_locals[5];
    CmMirStatement invalid_statements[3];
    CmMirBasicBlock invalid_block;
    CmHirExpr *mutable_root;
    CmHirExprId saved_left;

    init_add_nested_mir(&body, locals, statements, &block, fixture);

    cm_mir_context_init(&oom_mir);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(1u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_mir_add_monomorphized_body(&oom_mir,
            &fixture->context, &body, &oom_id);
        assert(0 && "nested MIR deep copy unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(&oom_mir) == 0u
        && oom_mir.hir_owner == NULL);
    cm_mir_context_destroy(&oom_mir);

    assert(cm_mir_add_monomorphized_body(mir, &fixture->context, &body,
        &id) == CM_MIR_OK);
    assert(id == 6u && cm_mir_body_count(mir) == 6u);
    statements[0].data.assign.value.data.binary.left.data.u32_value = 9u;
    statements[1].data.assign.value.data.binary.right.data.local = 2u;
    locals[3].kind = CM_MIR_LOCAL_ARGUMENT;
    stored = cm_mir_get_body(mir, id);
    assert(stored != NULL && stored->locals != locals
        && stored->locals[3].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->basic_blocks[0].statements != statements
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.left.data.u32_value == 1u
        && stored->basic_blocks[0].statements[1].data.assign.value.data
            .binary.right.data.local == 3u);
    init_add_nested_mir(&body, locals, statements, &block, fixture);

    invalid = body;
    invalid_block = block;
    invalid.basic_blocks = &invalid_block;
    invalid_block.statements = invalid_statements;
    invalid_statements[0] = statements[1];
    invalid_statements[1] = statements[0];
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_statements[1].data.assign.value.data.binary.left.data.local = 3u;
    invalid_statements[1].data.assign.value.data.binary.right.data.local = 1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_statements[0].data.assign.destination = CM_MIR_RETURN_LOCAL;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_locals, locals, 4u * sizeof(*locals));
    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid.locals = invalid_locals;
    invalid_locals[3].kind = CM_MIR_LOCAL_ARGUMENT;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[3] = locals[3];
    invalid_locals[3].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[3].type = fixture->alternate_u32_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    invalid_locals[3] = locals[3];
    invalid_statements[1].data.assign.value.data.binary.right.data.local = 2u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_locals[4].kind = CM_MIR_LOCAL_TEMPORARY;
    invalid_locals[4].type = fixture->u32_type;
    invalid.local_count = 5u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    invalid.local_count = 4u;
    invalid_statements[2] = statements[1];
    invalid_block.statement_count = 3u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    invalid = body;
    mutable_root = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->add_nested_root - 1u);
    assert(mutable_root != NULL && mutable_root->kind == CM_HIR_EXPR_BINARY);
    saved_left = mutable_root->data.binary.left;
    mutable_root->data.binary.left = fixture->add_nested_root;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_root->data.binary.left = saved_left;
}

static void assert_call_nested_model(CmMirContext *mir, TestHir *fixture,
    CmMirBodyId callee_id, CmMirBodyId wrong_callee_id)
{
    CmMirBody body;
    CmMirLocal locals[5];
    CmMirStatement statements[2];
    CmMirOperand argument;
    CmHirTypeId callee_substitution;
    CmMirBasicBlock blocks[2];
    CmMirBodyId id;
    const CmMirBody *stored;
    CmMirBodyId oom_id;
    CmMirBody invalid;
    CmMirLocal invalid_locals[6];
    CmMirStatement invalid_statements[3];
    CmMirOperand invalid_argument;
    CmHirTypeId invalid_substitution;
    CmMirBasicBlock invalid_blocks[2];
    CmHirExpr *mutable_argument_root;
    CmHirExprId saved_right;

    callee_substitution = fixture->u32_type;
    init_call_nested_mir(&body, locals, statements, &argument,
        &callee_substitution, blocks, fixture, callee_id);

    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_mir_add_monomorphized_body(mir,
            &fixture->context, &body, &oom_id);
        assert(0 && "nested-call MIR deep copy unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(mir) == 6u
        && mir->hir_owner == &fixture->context);

    assert(cm_mir_add_monomorphized_body(mir, &fixture->context, &body,
        &id) == CM_MIR_OK);
    assert(id == 7u && cm_mir_body_count(mir) == 7u);
    statements[0].data.assign.value.data.binary.left.data.u32_value = 9u;
    statements[1].data.assign.value.data.binary.right.data.local = 2u;
    argument.data.local = 3u;
    callee_substitution = fixture->alternate_u32_type;
    stored = cm_mir_get_body(mir, id);
    assert(stored != NULL && stored->locals != locals
        && stored->basic_blocks != blocks
        && stored->basic_blocks[0].statements != statements
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.left.data.u32_value == 1u
        && stored->basic_blocks[0].statements[1].data.assign.value.data
            .binary.right.data.local == 3u
        && stored->basic_blocks[0].terminator.data.call.arguments
            != &argument
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 4u
        && stored->basic_blocks[0].terminator.data.call.callee.substitutions
            != &callee_substitution
        && stored->basic_blocks[0].terminator.data.call.callee
            .substitutions[0] == fixture->u32_type);

    callee_substitution = fixture->u32_type;
    init_call_nested_mir(&body, locals, statements, &argument,
        &callee_substitution, blocks, fixture, callee_id);
    invalid = body;
    memcpy(invalid_locals, locals, 5u * sizeof(*locals));
    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_argument = argument;
    invalid_substitution = callee_substitution;
    memcpy(invalid_blocks, blocks, 2u * sizeof(*blocks));
    invalid.locals = invalid_locals;
    invalid.basic_blocks = invalid_blocks;
    invalid_blocks[0].statements = invalid_statements;
    invalid_blocks[0].terminator.data.call.arguments = &invalid_argument;
    invalid_blocks[0].terminator.data.call.callee.substitutions =
        &invalid_substitution;

    invalid_statements[0] = statements[1];
    invalid_statements[1] = statements[0];
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_statements[0].data.assign.value.data.binary.right.data.local =
        4u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_statements[1].data.assign.destination = 3u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_statements[1].data.assign.value.data.binary.left.data.local =
        3u;
    invalid_statements[1].data.assign.value.data.binary.right.data.local =
        1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    memcpy(invalid_statements, statements, 2u * sizeof(*statements));
    invalid_argument.data.local = 3u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_argument = argument;

    invalid_locals[4].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[4] = locals[4];

    invalid_locals[5].kind = CM_MIR_LOCAL_TEMPORARY;
    invalid_locals[5].type = fixture->u32_type;
    invalid.local_count = 6u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid.local_count = 5u;

    invalid_statements[2] = statements[1];
    invalid_blocks[0].statement_count = 3u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].statement_count = 1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].statement_count = 2u;

    invalid_blocks[0].terminator.data.call.target = 0u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.target = 1u;
    invalid_blocks[0].terminator.data.call.callee_instance = wrong_callee_id;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.callee_instance = callee_id;
    invalid_blocks[0].terminator.data.call.callee.definition =
        fixture->probe_definition;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.callee.definition =
        fixture->identity_definition;

    mutable_argument_root = (CmHirExpr *)cm_vec_at(
        &fixture->context.expressions,
        (size_t)fixture->call_nested_argument_root - 1u);
    assert(mutable_argument_root != NULL
        && mutable_argument_root->kind == CM_HIR_EXPR_BINARY);
    saved_right = mutable_argument_root->data.binary.right;
    mutable_argument_root->data.binary.right =
        fixture->call_nested_argument_root;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_argument_root->data.binary.right = saved_right;
}

static void assert_monomorphic_call_models(CmMirContext *mir,
    TestHir *fixture, CmMirBodyId one_arg_callee_id,
    CmMirBodyId two_arg_callee_id, CmMirBodyId wrong_callee_id)
{
    CmMirBody mono_body;
    CmMirLocal mono_locals[2];
    CmMirOperand mono_argument;
    CmMirBasicBlock mono_blocks[2];
    CmMirBodyId mono_id;
    CmMirBody pair_body;
    CmMirLocal pair_locals[7];
    CmMirStatement pair_statements[4];
    CmMirOperand pair_arguments[2];
    CmMirBasicBlock pair_blocks[2];
    CmMirBodyId pair_id;
    CmMirBodyId oom_id;
    const CmMirBody *stored;
    CmMirBody invalid;
    CmMirLocal invalid_locals[8];
    CmMirStatement invalid_statements[5];
    CmMirOperand invalid_arguments[3];
    CmMirBasicBlock invalid_blocks[2];
    CmHirExpr *mutable_call;
    CmHirExpr *mutable_first;
    CmHirExpr *mutable_second;
    CmHirExprId saved_first;
    CmHirExprId saved_second;

    init_call_mono_mir(&mono_body, mono_locals, &mono_argument,
        mono_blocks, fixture, one_arg_callee_id);
    assert(cm_mir_add_monomorphized_body(mir, &fixture->context,
        &mono_body, &mono_id) == CM_MIR_OK);
    assert(mono_id == 8u && cm_mir_body_count(mir) == 8u);
    mono_argument.data.local = 0u;
    stored = cm_mir_get_body(mir, mono_id);
    assert(stored != NULL
        && stored->basic_blocks[0].terminator.data.call.arguments
            != &mono_argument
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 1u
        && stored->basic_blocks[0].terminator.data.call.callee
            .substitution_count == 0u
        && stored->basic_blocks[0].terminator.data.call.callee
            .substitutions == NULL);

    init_call_pair_mir(&pair_body, pair_locals, pair_statements,
        pair_arguments, pair_blocks, fixture, two_arg_callee_id);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_mir_add_monomorphized_body(mir, &fixture->context,
            &pair_body, &oom_id);
        assert(0 && "two-argument MIR deep copy unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(mir) == 8u
        && mir->hir_owner == &fixture->context);

    assert(cm_mir_add_monomorphized_body(mir, &fixture->context,
        &pair_body, &pair_id) == CM_MIR_OK);
    assert(pair_id == 9u && cm_mir_body_count(mir) == 9u);
    pair_statements[0].data.assign.value.data.binary.left.data.u32_value =
        9u;
    pair_arguments[0].data.local = 3u;
    pair_arguments[1].data.local = 5u;
    stored = cm_mir_get_body(mir, pair_id);
    assert(stored != NULL && stored->locals != pair_locals
        && stored->basic_blocks != pair_blocks
        && stored->basic_blocks[0].statements != pair_statements
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.left.data.u32_value == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments
            != pair_arguments
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 4u
        && stored->basic_blocks[0].terminator.data.call.arguments[1].data
            .local == 6u);

    init_call_pair_mir(&pair_body, pair_locals, pair_statements,
        pair_arguments, pair_blocks, fixture, two_arg_callee_id);
    invalid = pair_body;
    memcpy(invalid_locals, pair_locals, 7u * sizeof(*pair_locals));
    memcpy(invalid_statements, pair_statements,
        4u * sizeof(*pair_statements));
    memcpy(invalid_arguments, pair_arguments,
        2u * sizeof(*pair_arguments));
    memcpy(invalid_blocks, pair_blocks, 2u * sizeof(*pair_blocks));
    invalid.locals = invalid_locals;
    invalid.basic_blocks = invalid_blocks;
    invalid_blocks[0].statements = invalid_statements;
    invalid_blocks[0].terminator.data.call.arguments = invalid_arguments;

    invalid_arguments[0] = pair_arguments[1];
    invalid_arguments[1] = pair_arguments[0];
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    memcpy(invalid_arguments, pair_arguments,
        2u * sizeof(*pair_arguments));

    invalid_blocks[0].terminator.data.call.argument_count = 1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.argument_count = 2u;

    invalid_arguments[1].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_arguments[1] = pair_arguments[1];
    invalid_arguments[1].data.local = 5u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_arguments[1] = pair_arguments[1];

    invalid_blocks[0].terminator.data.call.callee_instance = wrong_callee_id;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.callee_instance =
        two_arg_callee_id;
    invalid_blocks[0].terminator.data.call.callee.definition =
        fixture->add_max_definition;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.callee.definition =
        fixture->add_definition;

    invalid_statements[0] = pair_statements[2];
    invalid_statements[1] = pair_statements[3];
    invalid_statements[2] = pair_statements[0];
    invalid_statements[3] = pair_statements[1];
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    memcpy(invalid_statements, pair_statements,
        4u * sizeof(*pair_statements));
    invalid_statements[2].data.assign.destination = 4u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statements[2] = pair_statements[2];
    invalid_statements[2].data.assign.value.data.binary.left.data.local =
        6u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statements[2] = pair_statements[2];

    invalid_locals[6].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[6] = pair_locals[6];
    invalid_locals[7].kind = CM_MIR_LOCAL_TEMPORARY;
    invalid_locals[7].type = fixture->u32_type;
    invalid.local_count = 8u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid.local_count = 7u;

    mutable_call = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->call_pair_call_root - 1u);
    assert(mutable_call != NULL && mutable_call->kind == CM_HIR_EXPR_CALL
        && mutable_call->data.call.argument_count == 2u);
    saved_first = mutable_call->data.call.arguments[0];
    saved_second = mutable_call->data.call.arguments[1];
    mutable_call->data.call.arguments[0] = saved_second;
    mutable_call->data.call.arguments[1] = saved_first;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_call->data.call.arguments[0] = saved_first;
    mutable_call->data.call.arguments[1] = saved_second;

    mutable_first = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->call_pair_first_root - 1u);
    assert(mutable_first != NULL
        && mutable_first->kind == CM_HIR_EXPR_BINARY);
    saved_first = mutable_first->data.binary.left;
    mutable_first->data.binary.left = fixture->call_pair_first_root;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_first->data.binary.left = saved_first;

    mutable_second = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->call_pair_second_root - 1u);
    assert(mutable_second != NULL
        && mutable_second->kind == CM_HIR_EXPR_BINARY);
    saved_second = mutable_second->data.binary.right;
    mutable_second->data.binary.right = fixture->call_pair_second_root;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_second->data.binary.right = saved_second;
}

static void assert_nested_call_cfg_model(CmMirContext *mir,
    TestHir *fixture, CmMirBodyId callee_id, CmMirBodyId wrong_callee_id)
{
    CmMirBody body;
    CmMirLocal locals[7];
    CmMirStatement statements[3];
    CmMirOperand inner_arguments[2];
    CmMirOperand outer_arguments[2];
    CmMirBasicBlock blocks[3];
    CmMirBodyId id;
    CmMirBodyId oom_id;
    const CmMirBody *stored;
    CmMirBody invalid;
    CmMirLocal invalid_locals[8];
    CmMirStatement invalid_statements[4];
    CmMirOperand invalid_inner_arguments[3];
    CmMirOperand invalid_outer_arguments[3];
    CmMirBasicBlock invalid_blocks[4];
    CmHirExpr *mutable_inner;
    CmHirExpr *mutable_outer;
    CmHirExprId saved_expression;

    init_nested_calls_mir(&body, locals, statements, inner_arguments,
        outer_arguments, blocks, fixture, callee_id);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_mir_add_monomorphized_body(mir, &fixture->context,
            &body, &oom_id);
        assert(0 && "nested-call CFG deep copy unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(mir) == 9u
        && mir->hir_owner == &fixture->context);

    assert(cm_mir_add_monomorphized_body(mir, &fixture->context, &body,
        &id) == CM_MIR_OK);
    assert(id == 10u && cm_mir_body_count(mir) == 10u);
    statements[0].data.assign.value.data.binary.right.data.u32_value = 9u;
    inner_arguments[0].data.local = 4u;
    outer_arguments[1].data.local = 5u;
    blocks[0].terminator.data.call.target = 2u;
    stored = cm_mir_get_body(mir, id);
    assert(stored != NULL && stored->locals != locals
        && stored->basic_blocks != blocks
        && stored->basic_blocks[0].statements != statements
        && stored->basic_blocks[1].statements != &statements[2]
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.right.data.u32_value == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments
            != inner_arguments
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 3u
        && stored->basic_blocks[0].terminator.data.call.target == 1u
        && stored->basic_blocks[1].terminator.data.call.arguments
            != outer_arguments
        && stored->basic_blocks[1].terminator.data.call.arguments[1].data
            .local == 6u);

    init_nested_calls_mir(&body, locals, statements, inner_arguments,
        outer_arguments, blocks, fixture, callee_id);
    invalid = body;
    memcpy(invalid_locals, locals, 7u * sizeof(*locals));
    memcpy(invalid_statements, statements, 3u * sizeof(*statements));
    memcpy(invalid_inner_arguments, inner_arguments,
        2u * sizeof(*inner_arguments));
    memcpy(invalid_outer_arguments, outer_arguments,
        2u * sizeof(*outer_arguments));
    memcpy(invalid_blocks, blocks, 3u * sizeof(*blocks));
    invalid.locals = invalid_locals;
    invalid.basic_blocks = invalid_blocks;
    invalid_blocks[0].statements = invalid_statements;
    invalid_blocks[0].terminator.data.call.arguments =
        invalid_inner_arguments;
    invalid_blocks[1].statements = &invalid_statements[2];
    invalid_blocks[1].terminator.data.call.arguments =
        invalid_outer_arguments;

    invalid_statements[0] = statements[1];
    invalid_statements[1] = statements[0];
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    memcpy(invalid_statements, statements, 3u * sizeof(*statements));

    invalid_blocks[0].statement_count = 3u;
    invalid_blocks[1].statements = NULL;
    invalid_blocks[1].statement_count = 0u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].statement_count = 2u;
    invalid_blocks[1].statements = &invalid_statements[2];
    invalid_blocks[1].statement_count = 1u;

    invalid_blocks[0].terminator.data.call.destination = 6u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.destination = 5u;
    invalid_blocks[0].terminator.data.call.target = 2u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.target = 1u;
    invalid_blocks[1].terminator.data.call.destination = 5u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[1].terminator.data.call.destination =
        CM_MIR_RETURN_LOCAL;
    invalid_blocks[1].terminator.data.call.target = 1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[1].terminator.data.call.target = 2u;

    invalid_blocks[0].terminator.data.call.callee_instance = wrong_callee_id;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.callee_instance = callee_id;
    invalid_blocks[1].terminator.data.call.callee.definition =
        fixture->add_max_definition;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[1].terminator.data.call.callee.definition =
        fixture->add_definition;

    invalid_blocks[0].terminator.data.call.argument_count = 1u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.argument_count = 2u;
    invalid_inner_arguments[0].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_inner_arguments[0] = inner_arguments[0];
    invalid_outer_arguments[1].data.local = 5u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_outer_arguments[1] = outer_arguments[1];

    invalid_statements[2].data.assign.destination = 5u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statements[2] = statements[2];
    invalid_statements[3] = statements[2];
    invalid_blocks[1].statement_count = 2u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[1].statement_count = 1u;
    invalid_locals[6].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[6] = locals[6];
    invalid_locals[7].kind = CM_MIR_LOCAL_TEMPORARY;
    invalid_locals[7].type = fixture->u32_type;
    invalid.local_count = 8u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid.local_count = 7u;

    invalid.basic_block_count = 2u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid.basic_block_count = 4u;
    memset(&invalid_blocks[3], 0, sizeof(invalid_blocks[3]));
    invalid_blocks[3].terminator.kind = CM_MIR_TERMINATOR_RETURN;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid.basic_block_count = 3u;

    mutable_inner = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->nested_calls_inner_call - 1u);
    assert(mutable_inner != NULL && mutable_inner->kind == CM_HIR_EXPR_CALL
        && mutable_inner->data.call.argument_count == 2u);
    saved_expression = mutable_inner->data.call.arguments[0];
    mutable_inner->data.call.arguments[0] =
        fixture->nested_calls_inner_call;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_inner->data.call.arguments[0] = saved_expression;

    mutable_outer = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->nested_calls_root - 1u);
    assert(mutable_outer != NULL && mutable_outer->kind == CM_HIR_EXPR_CALL
        && mutable_outer->data.call.argument_count == 2u);
    saved_expression = mutable_outer->data.call.arguments[1];
    mutable_outer->data.call.arguments[1] = fixture->nested_calls_root;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_outer->data.call.arguments[1] = saved_expression;
}

static void assert_let_model(CmMirContext *mir, TestHir *fixture,
    CmMirBodyId add_id)
{
    CmMirBody body;
    CmMirLocal locals[4];
    CmMirStatement statements[2];
    CmMirOperand arguments[2];
    CmMirBasicBlock blocks[2];
    CmMirBodyId id;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirLocalKind saved_kind;
    CmHirExpr *mutable_input;
    uint32_t saved_local_index;
    CmHirContext foreign_hir;

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    locals[2].kind = CM_MIR_LOCAL_TEMPORARY;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);
    locals[2].kind = CM_MIR_LOCAL_USER;
    locals[2].type = fixture->u8_type;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    statements[0].data.assign.destination = 3u;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    mutable_input = (CmHirExpr *)cm_vec_at(&fixture->context.expressions,
        (size_t)fixture->let_first_input - 1u);
    assert(mutable_input != NULL
        && mutable_input->kind == CM_HIR_EXPR_LOCAL);
    saved_local_index = mutable_input->data.local.local_index;
    mutable_input->data.local.local_index = 1u;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);
    mutable_input->data.local.local_index = saved_local_index;

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    blocks[0].terminator.data.call.destination = 2u;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);
    blocks[0].terminator.data.call.destination = 3u;
    blocks[0].terminator.data.call.target = 0u;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    statements[1] = statements[0];
    blocks[0].statements = statements;
    blocks[0].statement_count = 2u;
    assert_rejected(mir, &fixture->context, &body,
        CM_MIR_INVARIANT_VIOLATION);

    init_let_mir(&body, locals, statements, arguments, blocks, fixture,
        add_id);
    assert(cm_mir_add_monomorphized_body(mir, &fixture->context, &body,
        &id) == CM_MIR_OK);
    stored = cm_mir_get_body(mir, id);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->locals[2].kind == CM_MIR_LOCAL_USER
        && stored->locals[3].kind == CM_MIR_LOCAL_USER
        && stored->basic_blocks[0].terminator.data.call.argument_count == 2u
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 2u
        && stored->basic_blocks[0].terminator.data.call.arguments[1].data
            .local == 2u
        && stored->basic_blocks[1].statements[0].data.assign.value.data
            .binary.left.data.local == 3u
        && stored->basic_blocks[1].statements[0].data.assign.value.data
            .binary.right.data.local == 2u);
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context, id)
        == CM_MIR_OK);

    mutable_stored = (CmMirBody *)stored;
    saved_kind = mutable_stored->locals[2].kind;
    mutable_stored->locals[2].kind = CM_MIR_LOCAL_TEMPORARY;
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context, id)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[2].kind = saved_kind;
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context, id)
        == CM_MIR_OK);
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context,
        CM_MIR_BODY_NONE) == CM_MIR_INVALID_ID);
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context,
        id + 1u) == CM_MIR_INVALID_ID);

    cm_hir_context_init(&foreign_hir);
    assert(cm_mir_validate_monomorphized_body(mir, &foreign_hir, id)
        == CM_MIR_INVALID_ARGUMENT);
    cm_hir_context_destroy(&foreign_hir);
}

static void assert_place_aggregate_model(CmMirContext *mir,
    TestHir *fixture)
{
    CmMirBody aggregate_body;
    CmMirLocal aggregate_locals[3];
    CmMirStatement aggregate_statements[2];
    CmMirAggregateField inner_fields[1];
    CmMirAggregateField outer_fields[2];
    CmMirBasicBlock aggregate_block;
    CmMirBodyId aggregate_id;
    CmMirBody projection_body;
    CmMirLocal projection_locals[2];
    CmMirStatement projection_statement;
    CmMirFieldProjection projections[2];
    CmMirBasicBlock projection_block;
    CmMirBodyId projection_id;
    const CmMirBody *stored;
    CmMirPlace place;
    CmMirBody invalid;
    CmMirStatement invalid_statements[2];
    CmMirAggregateField invalid_outer_fields[2];
    FILE *stream;
    char buffer[16384];
    size_t length;

    memset(aggregate_locals, 0, sizeof(aggregate_locals));
    aggregate_locals[0].kind = CM_MIR_LOCAL_RETURN;
    aggregate_locals[0].type = fixture->outer_type;
    aggregate_locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    aggregate_locals[1].type = fixture->u32_type;
    aggregate_locals[2].kind = CM_MIR_LOCAL_TEMPORARY;
    aggregate_locals[2].type = fixture->inner_type;
    memset(inner_fields, 0, sizeof(inner_fields));
    inner_fields[0].field_index = 0u;
    inner_fields[0].value.kind = CM_MIR_OPERAND_MOVE;
    inner_fields[0].value.type = fixture->u32_type;
    inner_fields[0].value.data.local = 1u;
    memset(outer_fields, 0, sizeof(outer_fields));
    outer_fields[0].field_index = 0u;
    outer_fields[0].value.kind = CM_MIR_OPERAND_MOVE;
    outer_fields[0].value.type = fixture->u32_type;
    outer_fields[0].value.data.local = 1u;
    outer_fields[1].field_index = 1u;
    outer_fields[1].value.kind = CM_MIR_OPERAND_MOVE;
    outer_fields[1].value.type = fixture->inner_type;
    outer_fields[1].value.data.local = 2u;
    memset(aggregate_statements, 0, sizeof(aggregate_statements));
    aggregate_statements[0].kind = CM_MIR_STATEMENT_ASSIGN;
    aggregate_statements[0].data.assign.destination = 2u;
    aggregate_statements[0].data.assign.value.kind =
        CM_MIR_RVALUE_AGGREGATE;
    aggregate_statements[0].data.assign.value.type = fixture->inner_type;
    aggregate_statements[0].data.assign.value.span =
        test_span(708u, 715u);
    aggregate_statements[0].data.assign.value.data.aggregate.definition =
        fixture->inner_definition;
    aggregate_statements[0].data.assign.value.data.aggregate.fields =
        inner_fields;
    aggregate_statements[0].data.assign.value.data.aggregate.field_count = 1u;
    aggregate_statements[1].kind = CM_MIR_STATEMENT_ASSIGN;
    aggregate_statements[1].data.assign.destination = CM_MIR_RETURN_LOCAL;
    aggregate_statements[1].data.assign.value.kind =
        CM_MIR_RVALUE_AGGREGATE;
    aggregate_statements[1].data.assign.value.type = fixture->outer_type;
    aggregate_statements[1].data.assign.value.span =
        test_span(705u, 725u);
    aggregate_statements[1].data.assign.value.data.aggregate.definition =
        fixture->outer_definition;
    aggregate_statements[1].data.assign.value.data.aggregate.fields =
        outer_fields;
    aggregate_statements[1].data.assign.value.data.aggregate.field_count = 2u;
    memset(&aggregate_block, 0, sizeof(aggregate_block));
    aggregate_block.statements = aggregate_statements;
    aggregate_block.statement_count = 2u;
    aggregate_block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(&aggregate_body, 0, sizeof(aggregate_body));
    aggregate_body.instance.definition = fixture->aggregate_definition;
    aggregate_body.owner = fixture->aggregate_definition;
    aggregate_body.source_body = fixture->aggregate_body;
    aggregate_body.locals = aggregate_locals;
    aggregate_body.local_count = 3u;
    aggregate_body.basic_blocks = &aggregate_block;
    aggregate_body.basic_block_count = 1u;

    invalid = aggregate_body;
    memcpy(invalid_statements, aggregate_statements,
        sizeof(aggregate_statements));
    memcpy(invalid_outer_fields, outer_fields, sizeof(outer_fields));
    invalid.basic_blocks = &aggregate_block;
    aggregate_block.statements = invalid_statements;
    invalid_statements[1].data.assign.value.data.aggregate.fields =
        invalid_outer_fields;
    invalid_outer_fields[0].field_index = 1u;
    invalid_outer_fields[1].field_index = 0u;
    assert_rejected(mir, &fixture->context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    aggregate_block.statements = aggregate_statements;

    assert(cm_mir_add_monomorphized_body(mir, &fixture->context,
        &aggregate_body, &aggregate_id) == CM_MIR_OK);
    inner_fields[0].value.data.local = 0u;
    outer_fields[0].field_index = 1u;
    stored = cm_mir_get_body(mir, aggregate_id);
    assert(stored != NULL
        && stored->basic_blocks[0].statements != aggregate_statements
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .aggregate.fields != inner_fields
        && stored->basic_blocks[0].statements[1].data.assign.value.data
            .aggregate.fields != outer_fields
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .aggregate.fields[0].value.data.local == 1u
        && stored->basic_blocks[0].statements[1].data.assign.value.data
            .aggregate.fields[0].field_index == 0u);
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context,
        aggregate_id) == CM_MIR_OK);

    memset(projection_locals, 0, sizeof(projection_locals));
    projection_locals[0].kind = CM_MIR_LOCAL_RETURN;
    projection_locals[0].type = fixture->u32_type;
    projection_locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    projection_locals[1].type = fixture->outer_type;
    memset(projections, 0, sizeof(projections));
    projections[0].definition = fixture->outer_definition;
    projections[0].field_index = 1u;
    projections[1].definition = fixture->inner_definition;
    projections[1].field_index = 0u;
    memset(&place, 0, sizeof(place));
    place.base = 1u;
    place.type = fixture->u32_type;
    place.projections = projections;
    place.projection_count = 2u;
    place.span = test_span(748u, 762u);
    memset(&projection_statement, 0, sizeof(projection_statement));
    projection_statement.kind = CM_MIR_STATEMENT_ASSIGN;
    projection_statement.data.assign.destination = CM_MIR_RETURN_LOCAL;
    projection_statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    projection_statement.data.assign.value.type = fixture->u32_type;
    projection_statement.data.assign.value.data.use.kind =
        CM_MIR_OPERAND_COPY_PLACE;
    projection_statement.data.assign.value.data.use.type = fixture->u32_type;
    projection_statement.data.assign.value.data.use.data.place = place;
    memset(&projection_block, 0, sizeof(projection_block));
    projection_block.statements = &projection_statement;
    projection_block.statement_count = 1u;
    projection_block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(&projection_body, 0, sizeof(projection_body));
    projection_body.instance.definition = fixture->projection_definition;
    projection_body.owner = fixture->projection_definition;
    projection_body.source_body = fixture->projection_body;
    projection_body.locals = projection_locals;
    projection_body.local_count = 2u;
    projection_body.basic_blocks = &projection_block;
    projection_body.basic_block_count = 1u;

    assert(cm_mir_validate_place(&fixture->context, &projection_body, &place)
        == CM_MIR_OK);
    {
        CmMirPlace base_place;

        memset(&base_place, 0, sizeof(base_place));
        base_place.base = 1u;
        base_place.type = fixture->outer_type;
        base_place.span = test_span(748u, 749u);
        assert(cm_mir_validate_place(&fixture->context, &projection_body,
            &base_place) == CM_MIR_OK);
    }
    place.projections[1].field_index = 1u;
    assert(cm_mir_validate_place(&fixture->context, &projection_body, &place)
        == CM_MIR_INVARIANT_VIOLATION);
    place.projections[1].field_index = 0u;
    place.span.source = 2u;
    assert(cm_mir_validate_place(&fixture->context, &projection_body, &place)
        == CM_MIR_INVARIANT_VIOLATION);
    place.span = test_span(748u, 762u);
    assert(cm_mir_add_monomorphized_body(mir, &fixture->context,
        &projection_body, &projection_id) == CM_MIR_OK);
    projections[0].field_index = 0u;
    stored = cm_mir_get_body(mir, projection_id);
    assert(stored != NULL
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .data.place.projections != projections
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .data.place.projections[0].field_index == 1u);
    assert(cm_mir_validate_monomorphized_body(mir, &fixture->context,
        projection_id) == CM_MIR_OK);

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "aggregate(") != NULL
        && strstr(buffer, "field(index=0,value=") != NULL
        && strstr(buffer, "copy(place(_1.field(") != NULL);
    assert(fclose(stream) == 0);
}

static void assert_borrow_dereference_schema(TestHir *fixture)
{
    CmMirContext dump_context;
    CmMirBody body;
    CmMirLocal locals[4];
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirPlace place;
    CmMirPlaceProjection projections[2];
    CmMirRvalue borrow;
    FILE *stream;
    char buffer[4096];
    size_t length;

    memset(locals, 0, sizeof(locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = fixture->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = fixture->u32_type;
    locals[2].kind = CM_MIR_LOCAL_TEMPORARY;
    locals[2].type = fixture->shared_u32_type;
    locals[3].kind = CM_MIR_LOCAL_TEMPORARY;
    locals[3].type = fixture->shared_outer_type;
    memset(&body, 0, sizeof(body));
    body.instance.definition = fixture->projection_definition;
    body.owner = fixture->projection_definition;
    body.source_body = fixture->projection_body;
    body.locals = locals;
    body.local_count = 4u;

    memset(&place, 0, sizeof(place));
    place.base = 2u;
    place.type = fixture->u32_type;
    place.span = test_span(748u, 752u);
    memset(projections, 0, sizeof(projections));
    projections[0].kind = CM_MIR_PROJECTION_DEREFERENCE;
    place.projections = projections;
    place.projection_count = 1u;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_OK);

    projections[0].definition = fixture->outer_definition;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_INVARIANT_VIOLATION);
    projections[0].definition = cm_hir_def_id_none();
    projections[0].field_index = 1u;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_INVARIANT_VIOLATION);
    projections[0].field_index = 0u;
    projections[0].kind = (CmMirPlaceProjectionKind)99;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_INVARIANT_VIOLATION);

    memset(projections, 0, sizeof(projections));
    projections[0].kind = CM_MIR_PROJECTION_DEREFERENCE;
    projections[1].kind = CM_MIR_PROJECTION_FIELD;
    projections[1].definition = fixture->outer_definition;
    projections[1].field_index = 0u;
    place.base = 3u;
    place.projections = projections;
    place.projection_count = 2u;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_OK);
    projections[1].kind = CM_MIR_PROJECTION_DEREFERENCE;
    assert(cm_mir_validate_place(&fixture->context, &body, &place)
        == CM_MIR_INVARIANT_VIOLATION);

    memset(&borrow, 0, sizeof(borrow));
    borrow.kind = CM_MIR_RVALUE_BORROW;
    borrow.type = fixture->shared_u32_type;
    borrow.span = test_span(748u, 754u);
    borrow.data.borrow.kind = CM_MIR_BORROW_SHARED;
    borrow.data.borrow.source.base = 1u;
    borrow.data.borrow.source.type = fixture->u32_type;
    borrow.data.borrow.source.span = test_span(750u, 751u);
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_OK);

    borrow.data.borrow.kind = (CmMirBorrowKind)99;
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_INVARIANT_VIOLATION);
    borrow.data.borrow.kind = CM_MIR_BORROW_SHARED;
    borrow.type = fixture->mutable_u32_type;
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_INVARIANT_VIOLATION);
    borrow.type = fixture->shared_u32_type;
    borrow.data.borrow.source.type = fixture->outer_type;
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_INVARIANT_VIOLATION);
    borrow.data.borrow.source.type = fixture->u32_type;
    borrow.data.borrow.source.span = test_span(747u, 751u);
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_INVARIANT_VIOLATION);
    borrow.data.borrow.source.span = test_span(750u, 751u);
    borrow.span.source = 2u;
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 0u)
        == CM_MIR_INVARIANT_VIOLATION);
    borrow.span = test_span(748u, 754u);
    assert(cm_mir_validate_rvalue(&fixture->context, &body, &borrow, 16u)
        == CM_MIR_INVALID_ARGUMENT);
    assert(cm_mir_validate_rvalue(NULL, &body, &borrow, 0u)
        == CM_MIR_INVALID_ARGUMENT);

    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = 2u;
    statement.data.assign.value = borrow;
    statement.data.assign.value.data.borrow.source.base = 3u;
    statement.data.assign.value.data.borrow.source.type = fixture->outer_type;
    statement.data.assign.value.data.borrow.source.projections = projections;
    statement.data.assign.value.data.borrow.source.projection_count = 1u;
    statement.data.assign.value.data.borrow.source.span =
        test_span(750u, 751u);
    projections[0].kind = CM_MIR_PROJECTION_DEREFERENCE;
    projections[0].definition = cm_hir_def_id_none();
    projections[0].field_index = 0u;
    memset(&block, 0, sizeof(block));
    block.statements = &statement;
    block.statement_count = 1u;
    block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    body.basic_blocks = &block;
    body.basic_block_count = 1u;
    cm_mir_context_init(&dump_context);
    cm_vec_push(&dump_context.bodies, &body);
    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &dump_context) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "borrow(shared,place(_3.deref,") != NULL);
    assert(fclose(stream) == 0);
    dump_context.bodies.len = 0u;
    cm_mir_context_destroy(&dump_context);
}

static void assert_legacy_constant(const TestHir *fixture)
{
    static const char expected[] =
        "mir-v8 pointer-bits=0\n"
        "body#1 owner=1:2 source-body#1 locals=1 blocks=1\n"
        "local body#1 _0 kind=return type=ty#4\n"
        "block body#1 bb0 statements=1\n"
        "statement body#1 bb0[0] assign _0 = "
        "use(const-i32(7):ty#4) type=ty#4\n"
        "terminator body#1 bb0 return\n";
    CmMirContext mir;
    CmMirLocal local;
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirBody body;
    CmMirBodyId id;
    const CmMirBody *stored;
    FILE *stream;
    char buffer[1024];
    size_t length;

    memset(&local, 0, sizeof(local));
    local.kind = CM_MIR_LOCAL_RETURN;
    local.type = fixture->i32_type;
    memset(&statement, 0, sizeof(statement));
    statement.kind = CM_MIR_STATEMENT_ASSIGN;
    statement.data.assign.destination = CM_MIR_RETURN_LOCAL;
    statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    statement.data.assign.value.type = fixture->i32_type;
    statement.data.assign.value.data.use.kind = CM_MIR_CONSTANT_I32;
    statement.data.assign.value.data.use.type = fixture->i32_type;
    statement.data.assign.value.data.use.data.i32_value = 7;
    memset(&block, 0, sizeof(block));
    block.statements = &statement;
    block.statement_count = 1u;
    block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(&body, 0, sizeof(body));
    body.owner = fixture->identity_definition;
    body.source_body = fixture->identity_body;
    body.locals = &local;
    body.local_count = 1u;
    body.basic_blocks = &block;
    body.basic_block_count = 1u;

    cm_mir_context_init(&mir);
    assert(cm_mir_add_body(&mir, &body, &id) == CM_MIR_OK && id == 1u);
    statement.data.assign.value.data.use.data.i32_value = 99;
    stored = cm_mir_get_body(&mir, id);
    assert(stored != NULL
        && stored->basic_blocks[0].statements != &statement
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .data.i32_value == 7);
    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strcmp(buffer, expected) == 0);
    assert(fclose(stream) == 0);
    cm_mir_context_destroy(&mir);
}

static void assert_dump(const CmMirContext *mir)
{
    static const char expected[] =
        "mir-v8 pointer-bits=0\n"
        "body#1 instance=1:2<ty#1> source-body#1 locals=2 blocks=1\n"
        "local body#1 _0 kind=return type=ty#1\n"
        "local body#1 _1 kind=argument type=ty#1\n"
        "statement body#1 bb0[0] assign _0 = "
        "use(move _1:ty#1) type=ty#1\n"
        "terminator body#1 bb0 return\n"
        "body#2 instance=1:2<ty#3> source-body#1 locals=2 blocks=1\n"
        "local body#2 _0 kind=return type=ty#3\n"
        "local body#2 _1 kind=argument type=ty#3\n"
        "statement body#2 bb0[0] assign _0 = "
        "use(move _1:ty#3) type=ty#3\n"
        "terminator body#2 bb0 return\n"
        "body#3 instance=1:3 source-body#2 locals=2 blocks=2\n"
        "local body#3 _0 kind=return type=ty#1\n"
        "local body#3 _1 kind=argument type=ty#1\n"
        "terminator body#3 bb0 call destination=_0 callee=body#1 "
        "instance=1:2<ty#1> args=[move _1:ty#1] target=bb1\n"
        "terminator body#3 bb1 return\n"
        "body#4 instance=1:4 source-body#3 locals=3 blocks=1\n"
        "local body#4 _0 kind=return type=ty#1\n"
        "local body#4 _1 kind=argument type=ty#1\n"
        "local body#4 _2 kind=argument type=ty#1\n"
        "statement body#4 bb0[0] assign _0 = "
        "binary(add,move _1:ty#1,move _2:ty#1) type=ty#1\n"
        "terminator body#4 bb0 return\n"
        "body#5 instance=1:5 source-body#4 locals=2 blocks=1\n"
        "local body#5 _0 kind=return type=ty#1\n"
        "local body#5 _1 kind=argument type=ty#1\n"
        "statement body#5 bb0[0] assign _0 = "
        "binary(add,move _1:ty#1,const-u32(4294967295):ty#1) "
        "type=ty#1\n"
        "terminator body#5 bb0 return\n"
        "body#6 instance=1:6 source-body#5 locals=4 blocks=1\n"
        "local body#6 _0 kind=return type=ty#1\n"
        "local body#6 _1 kind=argument type=ty#1\n"
        "local body#6 _2 kind=argument type=ty#1\n"
        "local body#6 _3 kind=temporary type=ty#1\n"
        "statement body#6 bb0[0] assign _3 = "
        "binary(add,const-u32(1):ty#1,move _2:ty#1) type=ty#1\n"
        "statement body#6 bb0[1] assign _0 = "
        "binary(add,move _1:ty#1,move _3:ty#1) type=ty#1\n"
        "terminator body#6 bb0 return\n"
        "body#7 instance=1:7 source-body#6 locals=5 blocks=2\n"
        "local body#7 _0 kind=return type=ty#1\n"
        "local body#7 _1 kind=argument type=ty#1\n"
        "local body#7 _2 kind=argument type=ty#1\n"
        "local body#7 _3 kind=temporary type=ty#1\n"
        "local body#7 _4 kind=temporary type=ty#1\n"
        "statement body#7 bb0[0] assign _3 = "
        "binary(add,const-u32(1):ty#1,move _2:ty#1) type=ty#1\n"
        "statement body#7 bb0[1] assign _4 = "
        "binary(add,move _1:ty#1,move _3:ty#1) type=ty#1\n"
        "terminator body#7 bb0 call destination=_0 callee=body#1 "
        "instance=1:2<ty#1> args=[move _4:ty#1] target=bb1\n"
        "terminator body#7 bb1 return\n"
        "body#8 instance=1:8 source-body#7 locals=2 blocks=2\n"
        "local body#8 _0 kind=return type=ty#1\n"
        "local body#8 _1 kind=argument type=ty#1\n"
        "terminator body#8 bb0 call destination=_0 callee=body#5 "
        "instance=1:5 args=[move _1:ty#1] target=bb1\n"
        "terminator body#8 bb1 return\n"
        "body#9 instance=1:9 source-body#8 locals=7 blocks=2\n"
        "local body#9 _0 kind=return type=ty#1\n"
        "local body#9 _1 kind=argument type=ty#1\n"
        "local body#9 _2 kind=argument type=ty#1\n"
        "local body#9 _3 kind=temporary type=ty#1\n"
        "local body#9 _4 kind=temporary type=ty#1\n"
        "local body#9 _5 kind=temporary type=ty#1\n"
        "local body#9 _6 kind=temporary type=ty#1\n"
        "statement body#9 bb0[0] assign _3 = "
        "binary(add,const-u32(1):ty#1,move _2:ty#1) type=ty#1\n"
        "statement body#9 bb0[1] assign _4 = "
        "binary(add,move _1:ty#1,move _3:ty#1) type=ty#1\n"
        "statement body#9 bb0[2] assign _5 = "
        "binary(add,move _1:ty#1,const-u32(2):ty#1) type=ty#1\n"
        "statement body#9 bb0[3] assign _6 = "
        "binary(add,move _5:ty#1,move _2:ty#1) type=ty#1\n"
        "terminator body#9 bb0 call destination=_0 callee=body#4 "
        "instance=1:4 args=[move _4:ty#1,move _6:ty#1] target=bb1\n"
        "terminator body#9 bb1 return\n";
    static const char expected_nested[] =
        "body#10 instance=1:10 source-body#9 locals=7 blocks=3\n"
        "local body#10 _0 kind=return type=ty#1\n"
        "local body#10 _1 kind=argument type=ty#1\n"
        "local body#10 _2 kind=argument type=ty#1\n"
        "local body#10 _3 kind=temporary type=ty#1\n"
        "local body#10 _4 kind=temporary type=ty#1\n"
        "local body#10 _5 kind=temporary type=ty#1\n"
        "local body#10 _6 kind=temporary type=ty#1\n"
        "statement body#10 bb0[0] assign _3 = "
        "binary(add,move _1:ty#1,const-u32(1):ty#1) type=ty#1\n"
        "statement body#10 bb0[1] assign _4 = "
        "binary(add,move _2:ty#1,const-u32(2):ty#1) type=ty#1\n"
        "terminator body#10 bb0 call destination=_5 callee=body#4 "
        "instance=1:4 args=[move _3:ty#1,move _4:ty#1] target=bb1\n"
        "statement body#10 bb1[0] assign _6 = "
        "binary(add,move _1:ty#1,const-u32(3):ty#1) type=ty#1\n"
        "terminator body#10 bb1 call destination=_0 callee=body#4 "
        "instance=1:4 args=[move _5:ty#1,move _6:ty#1] target=bb2\n"
        "terminator body#10 bb2 return\n";
    FILE *stream;
    char buffer[8192];
    size_t length;
    size_t prefix_length;

    stream = tmpfile();
    assert(stream != NULL);
    assert(cm_mir_dump(stream, mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    prefix_length = strlen(expected);
    assert(length == prefix_length + strlen(expected_nested)
        && memcmp(buffer, expected, prefix_length) == 0
        && strcmp(buffer + prefix_length, expected_nested) == 0);
    assert(fclose(stream) == 0);
}

static void test_context_pointer_bits(void)
{
    CmMirContext context;

    cm_mir_context_init(&context);
    assert(cm_mir_context_pointer_bits(&context) == 0u);
    assert(cm_mir_context_set_pointer_bits(&context, 16u)
        == CM_MIR_INVALID_ARGUMENT);
    assert(cm_mir_context_set_pointer_bits(&context, 32u) == CM_MIR_OK);
    assert(cm_mir_context_pointer_bits(&context) == 32u);
    assert(cm_mir_context_set_pointer_bits(&context, 32u) == CM_MIR_OK);
    assert(cm_mir_context_set_pointer_bits(&context, 64u)
        == CM_MIR_INVARIANT_VIOLATION);
    assert(cm_mir_context_pointer_bits(&context) == 32u);
    cm_mir_context_destroy(&context);
}

static void test_publication_atomicity(TestHir *fixture)
{
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirGenericArg argument;
    CmMirContext mir;
    CmMirPublication publication;
    CmMirBody identity;
    CmMirLocal identity_locals[2];
    CmMirStatement identity_statement;
    CmMirBasicBlock identity_block;
    CmHirTypeId substitution;
    CmMirBodyId reserved;
    CmMirBodyId duplicate;
    const CmMirBody *stored;
    uint64_t context_lifetime;
    uint64_t admission_capability;
    uint64_t admission_generation;

    memset(&admission, 0, sizeof(admission));
    cm_hir_instance_spec_init(&spec);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture->u32_type;
    spec.selected_callable = fixture->identity_definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    reachable.body = fixture->identity_body;
    reachable.spec = &spec;
    admission_result = cm_semantic_admit_typed_leaf_instances(&admission,
        &fixture->context, fixture->identity_definition.crate_id,
        &reachable, 1u);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);

    substitution = fixture->u32_type;
    init_identity_mir(&identity, identity_locals, &identity_statement,
        &identity_block, fixture, &substitution, fixture->u32_type);
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    cm_mir_context_init(&mir);
    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    assert(cm_mir_publication_reserve(&publication,
        fixture->identity_definition, &substitution, 1u,
        fixture->identity_body, &reserved) == CM_MIR_OK
        && reserved == 1u);
    duplicate = 99u;
    assert(cm_mir_publication_reserve(&publication,
        fixture->identity_definition, &substitution, 1u,
        fixture->identity_body, &duplicate) == CM_MIR_INVARIANT_VIOLATION
        && duplicate == CM_MIR_BODY_NONE);
    assert(cm_mir_body_count(&mir) == 0u
        && cm_mir_get_body(&mir, reserved) == NULL
        && cm_mir_publication_get_body(&publication, reserved) == NULL);
    assert(cm_mir_publication_define(&publication, reserved + 1u,
        &identity) == CM_MIR_INVALID_ID);
    assert(cm_mir_publication_validate(&publication)
        == CM_MIR_INVARIANT_VIOLATION);
    assert(cm_mir_publication_commit(&publication)
        == CM_MIR_INVARIANT_VIOLATION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_publication_destroy(&publication);
    assert(cm_mir_body_count(&mir) == 0u && mir.hir_owner == NULL);

    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    assert(cm_mir_publication_reserve(&publication,
        fixture->identity_definition, &substitution, 1u,
        fixture->identity_body, &reserved) == CM_MIR_OK);
    context_lifetime = mir.lifetime_id;
    cm_mir_context_destroy(&mir);
    cm_mir_context_init(&mir);
    assert(mir.lifetime_id != UINT64_C(0)
        && mir.lifetime_id != context_lifetime
        && cm_mir_publication_validate(&publication)
            == CM_MIR_INVALID_ADMISSION
        && cm_mir_publication_commit(&publication)
            == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_publication_destroy(&publication);

    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    assert(cm_mir_publication_reserve(&publication,
        fixture->identity_definition, &substitution, 1u,
        fixture->identity_body, &reserved) == CM_MIR_OK);
    admission_capability = cm_semantic_admission_capability_id(&admission);
    admission_generation = cm_semantic_admission_generation(&admission);
    assert(admission_capability != UINT64_C(0));
    cm_semantic_admission_destroy(&admission);
    assert(cm_semantic_admission_capability_id(&admission)
        == UINT64_C(0));
    admission_result = cm_semantic_admit_typed_leaf_instances(&admission,
        &fixture->context, fixture->identity_definition.crate_id,
        &reachable, 1u);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK
        && cm_semantic_admission_generation(&admission)
            == admission_generation
        && cm_semantic_admission_capability_id(&admission)
            != UINT64_C(0)
        && cm_semantic_admission_capability_id(&admission)
            != admission_capability
        && cm_mir_publication_validate(&publication)
            == CM_MIR_INVALID_ADMISSION
        && cm_mir_publication_commit(&publication)
            == CM_MIR_INVALID_ADMISSION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_publication_destroy(&publication);

    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    assert(cm_mir_publication_reserve(&publication,
        fixture->identity_definition, &substitution, 1u,
        fixture->identity_body, &reserved) == CM_MIR_OK
        && reserved == 1u);
    assert(cm_mir_publication_define(&publication, reserved, &identity)
        == CM_MIR_OK);
    identity_locals[1].type = fixture->u8_type;
    stored = cm_mir_publication_get_body(&publication, reserved);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->locals != identity_locals
        && stored->locals[1].type == fixture->u32_type
        && cm_mir_body_count(&mir) == 0u
        && cm_mir_get_body(&mir, reserved) == NULL);
    assert(cm_mir_publication_validate(&publication) == CM_MIR_OK);
    assert(cm_mir_publication_commit(&publication) == CM_MIR_OK
        && publication.implementation == NULL
        && cm_mir_body_count(&mir) == 1u);
    stored = cm_mir_get_body(&mir, reserved);
    assert(stored != NULL && stored->locals[1].type == fixture->u32_type
        && cm_mir_validate_admitted_monomorphized_body(&mir, &admission,
            reserved) == CM_MIR_OK);

    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&admission);
}

static void test_canonical_publication_ownership(TestHir *fixture)
{
    CmSemanticAdmission admission;
    CmSemanticAdmissionResult admission_result;
    CmSemanticReachableInstance reachable;
    CmHirInstanceSpec spec;
    CmHirInstanceSpec alias_spec;
    CmHirGenericArg argument;
    CmHirGenericArg alias_argument;
    CmHirCanonicalInstance canonical;
    CmHirCanonicalInstance alias_canonical;
    CmMirInstance first;
    CmMirInstance alias;
    CmMirInstance distinct;
    CmMirInstance inconsistent;
    CmMirInstance malformed;
    CmMirInstance borrowed;
    CmMirContext mir;
    CmMirPublication publication;
    CmMirBody identity;
    CmMirBody probe;
    CmMirLocal identity_locals[2];
    CmMirLocal probe_locals[2];
    CmMirStatement identity_statement;
    CmMirBasicBlock identity_block;
    CmMirBasicBlock probe_blocks[2];
    CmMirOperand probe_argument;
    CmHirTypeId substitution;
    unsigned char *distinct_bytes;
    unsigned char expected_first;
    CmMirBodyId first_id;
    CmMirBodyId probe_id;
    CmMirBodyId found;
    CmHirBodyId source_body;
    const CmMirBody *defined;
    const CmMirBody *stored;

    memset(&admission, 0, sizeof(admission));
    cm_hir_instance_spec_init(&spec);
    cm_hir_instance_spec_init(&alias_spec);
    memset(&argument, 0, sizeof(argument));
    memset(&alias_argument, 0, sizeof(alias_argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = fixture->u32_type;
    alias_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    alias_argument.data.type = fixture->alternate_u32_type;
    spec.selected_callable = fixture->identity_definition;
    spec.item_arguments = &argument;
    spec.item_argument_count = 1u;
    alias_spec.selected_callable = fixture->identity_definition;
    alias_spec.item_arguments = &alias_argument;
    alias_spec.item_argument_count = 1u;
    reachable.body = fixture->identity_body;
    reachable.spec = &spec;
    admission_result = cm_semantic_admit_typed_leaf_instances(&admission,
        &fixture->context, fixture->identity_definition.crate_id,
        &reachable, 1u);
    assert(admission_result.status == CM_SEMANTIC_ADMISSION_OK);

    cm_hir_canonical_instance_init(&canonical);
    cm_hir_canonical_instance_init(&alias_canonical);
    assert(cm_hir_canonical_instance_encode(&fixture->context,
        fixture->identity_definition.crate_id, &spec, &canonical)
            == CM_HIR_INSTANCE_OK);
    assert(cm_hir_canonical_instance_encode(&fixture->context,
        fixture->identity_definition.crate_id, &alias_spec,
        &alias_canonical) == CM_HIR_INSTANCE_OK);
    assert(canonical.size != 0u && canonical.size == alias_canonical.size
        && memcmp(canonical.bytes, alias_canonical.bytes,
            canonical.size) == 0);

    substitution = fixture->u32_type;
    memset(&first, 0, sizeof(first));
    first.definition = canonical.definition;
    first.substitutions = &substitution;
    first.substitution_count = 1u;
    first.body = canonical.body;
    first.identity_bytes = canonical.bytes;
    first.identity_size = canonical.size;
    alias = first;
    alias.substitutions = &alias_argument.data.type;
    alias.identity_bytes = alias_canonical.bytes;
    distinct_bytes = (unsigned char *)cm_alloc(canonical.size);
    memcpy(distinct_bytes, canonical.bytes, canonical.size);
    distinct_bytes[canonical.size - 1u] ^= 1u;
    distinct = first;
    distinct.identity_bytes = distinct_bytes;
    inconsistent = first;
    inconsistent.substitutions = &fixture->u8_type;
    malformed = first;
    malformed.identity_bytes = NULL;
    found = 99u;
    assert(cm_mir_find_canonical(NULL, &first, &found)
            == CM_MIR_INVALID_ARGUMENT
        && found == CM_MIR_BODY_NONE);

    init_identity_mir(&identity, identity_locals, &identity_statement,
        &identity_block, fixture, &substitution, fixture->u32_type);
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    identity.instance = distinct;
    cm_mir_context_init(&mir);
    found = 99u;
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_BODY;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 0u);
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 0u);
    identity.instance = first;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_OK && found == 1u
        && cm_mir_body_count(&mir) == 1u);
    cm_mir_context_destroy(&mir);

    cm_mir_context_init(&mir);
    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    found = 99u;
    assert(cm_mir_publication_reserve_canonical(&publication, &malformed,
        fixture->identity_body, &found) == CM_MIR_INVALID_ARGUMENT
        && found == CM_MIR_BODY_NONE);
    found = 99u;
    assert(cm_mir_publication_reserve_canonical(&publication, &inconsistent,
        fixture->identity_body, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE);
    assert(cm_mir_publication_reserve_canonical(&publication, &first,
        fixture->identity_body, &first_id) == CM_MIR_OK
        && first_id == 1u);
    found = 99u;
    assert(cm_mir_publication_find_instance(&publication,
        first.definition, first.substitutions, first.substitution_count,
        &found) == CM_MIR_INVALID_ID && found == CM_MIR_BODY_NONE);
    assert(cm_mir_publication_find_canonical(&publication, &alias,
        &found) == CM_MIR_OK && found == first_id);
    found = 99u;
    assert(cm_mir_publication_reserve_canonical(&publication, &alias,
        fixture->identity_body, &found) == CM_MIR_INVARIANT_VIOLATION
        && found == CM_MIR_BODY_NONE);
    memset(&borrowed, 0, sizeof(borrowed));
    source_body = CM_HIR_BODY_NONE;
    assert(cm_mir_publication_get_instance(&publication, first_id,
        &borrowed, &source_body) == CM_MIR_OK
        && source_body == fixture->identity_body
        && borrowed.identity_bytes != canonical.bytes
        && borrowed.substitutions != &substitution
        && borrowed.identity_size == canonical.size);
    expected_first = canonical.bytes[0];
    canonical.bytes[0] ^= 1u;
    assert(borrowed.identity_bytes[0] == expected_first);
    canonical.bytes[0] = expected_first;

    identity.instance = distinct;
    assert(cm_mir_publication_define(&publication, first_id, &identity)
        == CM_MIR_INVARIANT_VIOLATION);
    identity.instance = first;
    assert(cm_mir_publication_define(&publication, first_id, &identity)
        == CM_MIR_OK);
    defined = cm_mir_publication_get_body(&publication, first_id);
    assert(defined != NULL && defined->instance.identity_bytes != NULL
        && defined->instance.identity_bytes != canonical.bytes
        && defined->instance.identity_bytes != borrowed.identity_bytes
        && memcmp(defined->instance.identity_bytes, canonical.bytes,
            canonical.size) == 0);
    canonical.bytes[0] ^= 1u;
    substitution = fixture->u8_type;
    assert(defined->instance.identity_bytes[0] == expected_first
        && defined->instance.substitutions[0] == fixture->u32_type);
    canonical.bytes[0] = expected_first;
    substitution = fixture->u32_type;
    assert(cm_mir_publication_validate(&publication) == CM_MIR_OK);
    assert(cm_mir_publication_commit(&publication) == CM_MIR_OK);
    stored = cm_mir_get_body(&mir, first_id);
    assert(stored != NULL && stored->instance.identity_bytes != NULL
        && stored->instance.identity_bytes[0] == expected_first);
    assert(cm_mir_find_canonical(&mir, &alias, &found) == CM_MIR_OK
        && found == first_id);
    found = 99u;
    assert(cm_mir_find_instance(&mir, first.definition,
        first.substitutions, first.substitution_count,
        &found) == CM_MIR_INVALID_ID && found == CM_MIR_BODY_NONE);

    cm_mir_publication_init(&publication);
    assert(cm_mir_publication_begin(&publication, &mir, &admission)
        == CM_MIR_OK);
    found = 99u;
    assert(cm_mir_publication_reserve_canonical(&publication, &distinct,
        fixture->identity_body, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 1u);
    assert(cm_mir_find_canonical(&mir, &first, &found) == CM_MIR_OK
        && found == first_id);
    cm_mir_publication_destroy(&publication);

    cm_mir_context_destroy(&mir);

    /* The one-shot admitted path authenticates the same exact identity. */
    cm_mir_context_init(&mir);
    init_identity_mir(&identity, identity_locals, &identity_statement,
        &identity_block, fixture, &substitution, fixture->u32_type);
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    identity.instance = distinct;
    found = 99u;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 0u);
    identity.instance = inconsistent;
    found = 99u;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 0u);
    identity.instance = first;
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_BODY;
    found = 99u;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &found) == CM_MIR_INVALID_ADMISSION
        && found == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 0u);
    identity.semantic_evidence = CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    assert(cm_mir_add_admitted_monomorphized_body(&mir, &admission,
        &identity, &first_id) == CM_MIR_OK && first_id == 1u);
    cm_mir_context_destroy(&mir);

    /* Body and call storage own independent copies of the same key. */
    cm_mir_context_init(&mir);
    init_identity_mir(&identity, identity_locals, &identity_statement,
        &identity_block, fixture, &substitution, fixture->u32_type);
    identity.instance = first;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context,
        &identity, &first_id) == CM_MIR_OK && first_id == 1u);
    init_probe_mir(&probe, probe_locals, &probe_argument, &substitution,
        probe_blocks, fixture, first_id);
    probe_blocks[0].terminator.data.call.callee = alias;
    alias_argument.data.type = fixture->u8_type;
    probe_id = 99u;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context, &probe,
        &probe_id) == CM_MIR_INVARIANT_VIOLATION
        && probe_id == CM_MIR_BODY_NONE && cm_mir_body_count(&mir) == 1u);
    alias_argument.data.type = fixture->alternate_u32_type;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context, &probe,
        &probe_id) == CM_MIR_OK && probe_id == 2u);
    stored = cm_mir_get_body(&mir, probe_id);
    assert(stored != NULL
        && stored->basic_blocks[0].terminator.data.call.callee.identity_bytes
            != alias.identity_bytes
        && stored->basic_blocks[0].terminator.data.call.callee.identity_bytes
            != cm_mir_get_body(&mir, first_id)->instance.identity_bytes
        && memcmp(stored->basic_blocks[0].terminator.data.call.callee
                .identity_bytes, canonical.bytes, canonical.size) == 0);
    alias_canonical.bytes[0] ^= 1u;
    assert(stored->basic_blocks[0].terminator.data.call.callee
            .identity_bytes[0] == expected_first);
    alias_canonical.bytes[0] = expected_first;
    cm_mir_context_destroy(&mir);

    cm_free(distinct_bytes);
    cm_hir_canonical_instance_destroy(&alias_canonical);
    cm_hir_canonical_instance_destroy(&canonical);
    cm_semantic_admission_destroy(&admission);
}

static void test_canonical_blanket_impl_materialization(TestHir *fixture)
{
    CmHirCanonicalInstance canonical_u32;
    CmHirCanonicalInstance canonical_u8;
    CmHirGenericArg argument;
    CmHirInstanceSpec spec;
    CmHirTypeId substitution;
    CmMirBasicBlock block;
    CmMirBody body;
    CmMirBodyId id;
    CmMirContext mir;
    CmMirLocal locals[2];
    CmMirStatement statement;

    add_blanket_impl_method(fixture, fixture->identity_definition.crate_id,
        1u);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    cm_hir_instance_spec_init(&spec);
    spec.selected_callable = fixture->blanket_method_definition;
    spec.declared_trait_callable =
        fixture->blanket_declared_method_definition;
    spec.enclosing_impl = fixture->blanket_impl_definition;
    spec.enclosing_impl_arguments = &argument;
    spec.enclosing_impl_argument_count = 1u;
    spec.implemented_trait = fixture->blanket_trait_definition;
    spec.self_owner = fixture->blanket_impl_definition;
    cm_hir_canonical_instance_init(&canonical_u32);
    cm_hir_canonical_instance_init(&canonical_u8);

    argument.data.type = fixture->u32_type;
    spec.self_type = fixture->u32_type;
    assert(cm_hir_canonical_instance_encode(&fixture->context, 1u, &spec,
        &canonical_u32) == CM_HIR_INSTANCE_OK);
    argument.data.type = fixture->u8_type;
    spec.self_type = fixture->u8_type;
    assert(cm_hir_canonical_instance_encode(&fixture->context, 1u, &spec,
        &canonical_u8) == CM_HIR_INSTANCE_OK);

    cm_mir_context_init(&mir);
    substitution = fixture->u8_type;
    init_identity_mir(&body, locals, &statement, &block, fixture,
        &substitution, fixture->u8_type);
    body.instance.definition = fixture->blanket_method_definition;
    body.instance.body = fixture->blanket_method_body;
    body.instance.identity_bytes = canonical_u8.bytes;
    body.instance.identity_size = canonical_u8.size;
    body.owner = fixture->blanket_method_definition;
    body.source_body = fixture->blanket_method_body;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context, &body,
        &id) == CM_MIR_OK && id == 1u);
    cm_mir_context_destroy(&mir);

    cm_mir_context_init(&mir);
    body.instance.identity_bytes = canonical_u32.bytes;
    body.instance.identity_size = canonical_u32.size;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context, &body,
        &id) == CM_MIR_INVARIANT_VIOLATION
        && cm_mir_body_count(&mir) == 0u);
    body.instance.identity_bytes = canonical_u8.bytes;
    body.instance.identity_size = canonical_u8.size;
    body.instance.substitutions = NULL;
    body.instance.substitution_count = 0u;
    assert(cm_mir_add_monomorphized_body(&mir, &fixture->context, &body,
        &id) == CM_MIR_INVARIANT_VIOLATION
        && cm_mir_body_count(&mir) == 0u);
    cm_mir_context_destroy(&mir);

    cm_hir_canonical_instance_destroy(&canonical_u8);
    cm_hir_canonical_instance_destroy(&canonical_u32);
}

int main(void)
{
    TestHir fixture;
    CmMirContext mir;
    CmMirBody identity;
    CmMirLocal identity_locals[2];
    CmMirStatement identity_statement;
    CmMirBasicBlock identity_block;
    CmHirTypeId identity_substitution;
    CmMirBodyId identity_id;
    CmMirBodyId oom_id;
    CmMirContext oom_mir;
    const CmMirBody *stored;
    CmMirBody variant;
    CmMirLocal variant_locals[2];
    CmMirStatement variant_statement;
    CmMirBasicBlock variant_block;
    CmHirTypeId variant_substitution;
    CmMirBodyId variant_id;
    CmMirBody probe;
    CmMirLocal probe_locals[2];
    CmMirOperand probe_argument;
    CmHirTypeId callee_substitution;
    CmMirBasicBlock probe_blocks[2];
    CmMirBodyId probe_id;
    CmMirBody add;
    CmMirLocal add_locals[3];
    CmMirStatement add_statement;
    CmMirBasicBlock add_block;
    CmMirBodyId add_id;
    CmMirBody invalid_add;
    CmMirLocal invalid_add_locals[3];
    CmMirStatement invalid_add_statement;
    CmMirBasicBlock invalid_add_block;
    CmMirBody add_max;
    CmMirLocal add_max_locals[2];
    CmMirStatement add_max_statement;
    CmMirBasicBlock add_max_block;
    CmMirBodyId add_max_id;
    CmMirBody invalid_add_max;
    CmMirStatement invalid_add_max_statement;
    CmMirBasicBlock invalid_add_max_block;
    CmMirBody invalid;
    CmMirLocal invalid_locals[2];
    CmMirStatement invalid_statement;
    CmMirBasicBlock invalid_block;
    CmMirOperand invalid_argument;
    CmMirBasicBlock invalid_blocks[2];
    CmHirTypeId invalid_substitution;
    CmHirContext foreign_hir;

    test_context_pointer_bits();
    test_hir_init(&fixture);
    test_publication_atomicity(&fixture);
    test_canonical_publication_ownership(&fixture);
    test_canonical_blanket_impl_materialization(&fixture);
    assert_legacy_constant(&fixture);
    cm_mir_context_init(&mir);

    identity_substitution = fixture.u32_type;
    init_identity_mir(&identity, identity_locals, &identity_statement,
        &identity_block, &fixture, &identity_substitution,
        fixture.u32_type);

    cm_mir_context_init(&oom_mir);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(1u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_mir_add_monomorphized_body(&oom_mir, &fixture.context,
            &identity, &oom_id);
        assert(0 && "MIR deep-copy allocation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(&oom_mir) == 0u
        && oom_mir.hir_owner == NULL);
    cm_mir_context_destroy(&oom_mir);

    assert(cm_mir_add_monomorphized_body(&mir, &fixture.context, &identity,
        &identity_id) == CM_MIR_OK);
    assert(identity_id == 1u && cm_mir_body_count(&mir) == 1u);

    identity_substitution = fixture.u8_type;
    identity_locals[0].type = fixture.u8_type;
    identity_statement.data.assign.value.data.use.data.local = 0u;
    stored = cm_mir_get_body(&mir, identity_id);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->instance.substitutions != &identity_substitution
        && stored->instance.substitutions[0] == fixture.u32_type
        && stored->locals != identity_locals
        && stored->locals[0].type == fixture.u32_type
        && stored->basic_blocks != &identity_block
        && stored->basic_blocks[0].statements != &identity_statement
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .data.local
            == 1u);
    identity_substitution = fixture.u32_type;
    identity_locals[0].type = fixture.u32_type;
    identity_statement.data.assign.value.data.use.data.local = 1u;

    assert_rejected(&mir, &fixture.context, &identity,
        CM_MIR_INVARIANT_VIOLATION);

    invalid_substitution = fixture.u8_type;
    init_identity_mir(&invalid, invalid_locals, &invalid_statement,
        &invalid_block, &fixture, &invalid_substitution, fixture.u8_type);
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_substitution = (CmHirTypeId)9999u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid = identity;
    invalid.instance.substitutions = NULL;
    invalid.instance.substitution_count = 0u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    invalid = identity;
    memcpy(invalid_locals, identity_locals, sizeof(invalid_locals));
    invalid.locals = invalid_locals;
    invalid_locals[1].kind = CM_MIR_LOCAL_RETURN;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_locals[1] = identity_locals[1];
    invalid_locals[1].type = fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    invalid = identity;
    invalid_statement = identity_statement;
    invalid_block = identity_block;
    invalid_block.statements = &invalid_statement;
    invalid.basic_blocks = &invalid_block;
    invalid_statement.data.assign.value.data.use.data.local = 2u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statement = identity_statement;
    invalid_statement.data.assign.value.data.use.type = fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statement = identity_statement;
    invalid_statement.data.assign.value.data.use.kind = CM_MIR_CONSTANT_I32;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statement = identity_statement;
    invalid_statement.data.assign.value.data.use.kind =
        (CmMirConstantKind)99;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statement = identity_statement;
    invalid_statement.kind = (CmMirStatementKind)99;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_statement = identity_statement;
    invalid_block.terminator.kind = (CmMirTerminatorKind)99;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    variant_substitution = fixture.alternate_u32_type;
    init_identity_mir(&variant, variant_locals, &variant_statement,
        &variant_block, &fixture, &variant_substitution,
        fixture.alternate_u32_type);
    assert(cm_mir_add_monomorphized_body(&mir, &fixture.context, &variant,
        &variant_id) == CM_MIR_OK);
    assert(variant_id == 2u && cm_mir_body_count(&mir) == 2u);

    callee_substitution = fixture.u32_type;
    init_probe_mir(&probe, probe_locals, &probe_argument,
        &callee_substitution, probe_blocks, &fixture, identity_id);
    assert(cm_mir_add_monomorphized_body(&mir, &fixture.context, &probe,
        &probe_id) == CM_MIR_OK);
    assert(probe_id == 3u && cm_mir_body_count(&mir) == 3u);

    callee_substitution = fixture.u8_type;
    probe_argument.data.local = 0u;
    probe_blocks[0].terminator.data.call.target = 0u;
    stored = cm_mir_get_body(&mir, probe_id);
    assert(stored != NULL
        && stored->basic_blocks[0].terminator.data.call.arguments
            != &probe_argument
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local
            == 1u
        && stored->basic_blocks[0].terminator.data.call.callee.substitutions
            != &callee_substitution
        && stored->basic_blocks[0].terminator.data.call.callee
            .substitutions[0] == fixture.u32_type
        && stored->basic_blocks[0].terminator.data.call.target == 1u);
    callee_substitution = fixture.u32_type;
    probe_argument.data.local = 1u;
    probe_blocks[0].terminator.data.call.target = 1u;

    init_probe_mir(&invalid, invalid_locals, &invalid_argument,
        &invalid_substitution, invalid_blocks, &fixture, identity_id);
    invalid_substitution = fixture.u32_type;
    invalid_blocks[0].terminator.data.call.callee_instance = 99u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0] = probe_blocks[0];
    invalid_blocks[0].terminator.data.call.arguments = &invalid_argument;
    invalid_blocks[0].terminator.data.call.callee.substitutions =
        &invalid_substitution;
    invalid_substitution = fixture.alternate_u32_type;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_substitution = fixture.u32_type;
    invalid_blocks[0].terminator.data.call.destination = 2u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.destination = 0u;
    invalid_blocks[0].terminator.data.call.target = 2u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.target = 1u;
    invalid_blocks[0].terminator.data.call.argument_count = 0u;
    invalid_blocks[0].terminator.data.call.arguments = NULL;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_blocks[0].terminator.data.call.argument_count = 1u;
    invalid_blocks[0].terminator.data.call.arguments = &invalid_argument;
    invalid_argument = probe_argument;
    invalid_argument.data.local = 2u;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_argument = probe_argument;
    invalid_argument.type = fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_argument = probe_argument;
    invalid_argument.kind = CM_MIR_CONSTANT_I32;
    assert_rejected(&mir, &fixture.context, &invalid,
        CM_MIR_INVARIANT_VIOLATION);

    init_add_mir(&add, add_locals, &add_statement, &add_block, &fixture);
    assert(cm_mir_add_monomorphized_body(&mir, &fixture.context, &add,
        &add_id) == CM_MIR_OK);
    assert(add_id == 4u && cm_mir_body_count(&mir) == 4u);
    add_statement.data.assign.value.data.binary.left.data.local = 0u;
    add_statement.data.assign.value.data.binary.right.data.local = 1u;
    stored = cm_mir_get_body(&mir, add_id);
    assert(stored != NULL
        && stored->basic_blocks[0].statements != &add_statement
        && stored->basic_blocks[0].statements[0].data.assign.value.kind
            == CM_MIR_RVALUE_BINARY
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.left.data.local == 1u
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.right.data.local == 2u);
    add_statement.data.assign.value.data.binary.left.data.local = 1u;
    add_statement.data.assign.value.data.binary.right.data.local = 2u;

    invalid_add = add;
    memcpy(invalid_add_locals, add_locals, sizeof(invalid_add_locals));
    invalid_add_statement = add_statement;
    invalid_add_block = add_block;
    invalid_add.locals = invalid_add_locals;
    invalid_add.basic_blocks = &invalid_add_block;
    invalid_add_block.statements = &invalid_add_statement;
    invalid_add_statement.data.assign.value.kind = (CmMirRvalueKind)99;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.operator_kind =
        (CmMirBinaryOp)99;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.operator_kind =
        CM_MIR_BINARY_SUBTRACT;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.type = fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.left.type =
        fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.left.data.local = 3u;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.left.kind =
        CM_MIR_CONSTANT_I32;
    invalid_add_statement.data.assign.value.data.binary.left.data.i32_value =
        1;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.left.data.local = 2u;
    invalid_add_statement.data.assign.value.data.binary.right.data.local = 1u;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_U32;
    invalid_add_statement.data.assign.value.data.binary.right.data.u32_value =
        UINT32_MAX;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_statement = add_statement;
    invalid_add_statement.data.assign.destination = 1u;
    assert_rejected(&mir, &fixture.context, &invalid_add,
        CM_MIR_INVARIANT_VIOLATION);

    init_add_max_mir(&add_max, add_max_locals, &add_max_statement,
        &add_max_block, &fixture);
    assert(cm_mir_add_monomorphized_body(&mir, &fixture.context, &add_max,
        &add_max_id) == CM_MIR_OK);
    assert(add_max_id == 5u && cm_mir_body_count(&mir) == 5u);
    add_max_statement.data.assign.value.data.binary.right.data.u32_value = 0u;
    stored = cm_mir_get_body(&mir, add_max_id);
    assert(stored != NULL
        && stored->basic_blocks[0].statements != &add_max_statement
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.right.kind == CM_MIR_CONSTANT_U32
        && stored->basic_blocks[0].statements[0].data.assign.value.data
            .binary.right.data.u32_value == UINT32_MAX);
    add_max_statement.data.assign.value.data.binary.right.data.u32_value =
        UINT32_MAX;

    invalid_add_max = add_max;
    invalid_add_max_statement = add_max_statement;
    invalid_add_max_block = add_max_block;
    invalid_add_max.basic_blocks = &invalid_add_max_block;
    invalid_add_max_block.statements = &invalid_add_max_statement;
    invalid_add_max_statement.data.assign.value.data.binary.right.data
        .u32_value = UINT32_MAX - 1u;
    assert_rejected(&mir, &fixture.context, &invalid_add_max,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_max_statement = add_max_statement;
    invalid_add_max_statement.data.assign.value.data.binary.right.kind =
        CM_MIR_CONSTANT_I32;
    invalid_add_max_statement.data.assign.value.data.binary.right.data
        .i32_value = -1;
    assert_rejected(&mir, &fixture.context, &invalid_add_max,
        CM_MIR_INVARIANT_VIOLATION);
    invalid_add_max_statement = add_max_statement;
    invalid_add_max_statement.data.assign.value.data.binary.right.type =
        fixture.u8_type;
    assert_rejected(&mir, &fixture.context, &invalid_add_max,
        CM_MIR_INVARIANT_VIOLATION);

    assert_nested_model(&mir, &fixture);
    assert_call_nested_model(&mir, &fixture, identity_id, variant_id);
    assert_monomorphic_call_models(&mir, &fixture, add_max_id, add_id,
        identity_id);
    assert_nested_call_cfg_model(&mir, &fixture, add_id, add_max_id);

    cm_hir_context_init(&foreign_hir);
    assert_rejected(&mir, &foreign_hir, &probe, CM_MIR_INVALID_ARGUMENT);
    cm_hir_context_destroy(&foreign_hir);

    assert_dump(&mir);
    assert_dump(&mir);
    assert(cm_mir_get_body(&mir, CM_MIR_BODY_NONE) == NULL);
    assert(cm_mir_get_body(&mir, 11u) == NULL);
    assert_let_model(&mir, &fixture, add_id);
    assert_place_aggregate_model(&mir, &fixture);
    assert_borrow_dereference_schema(&fixture);

    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&fixture.context);
    puts("mir model tests: ok");
    return 0;
}
