#include "cm/mir/lower.h"

#include "cm/alloc.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf lower_oom_jump;

static void jump_on_lower_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    longjmp(lower_oom_jump, 1);
}

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

static CmHirTypeId add_bool_type(CmHirContext *hir, uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = test_span(start, start + 1u);
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirTypeId add_shared_reference_type(CmHirContext *hir,
    CmHirTypeId pointee, uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(start, start + 1u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(hir, &type, &id) == CM_HIR_OK);
    return id;
}

static CmHirBodyId add_function_body(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, const char *name,
    CmHirTypeId return_type, CmHirBodyState state, uint64_t low_bits,
    uint64_t high_bits, uint32_t start)
{
    CmHirDefId definition;
    CmHirExpr integer;
    CmHirExpr block;
    CmHirExprId integer_id;
    CmHirExprId block_id;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(start, start + 20u), &definition)
        == CM_HIR_OK);
    block_id = CM_HIR_EXPR_NONE;
    if (state == CM_HIR_BODY_TYPED) {
        memset(&integer, 0, sizeof(integer));
        integer.kind = CM_HIR_EXPR_INTEGER;
        integer.type = return_type;
        integer.span = test_span(start + 10u, start + 12u);
        integer.data.integer.low_bits = low_bits;
        integer.data.integer.high_bits = high_bits;
        assert(cm_hir_add_expr(hir, &integer, &integer_id) == CM_HIR_OK);
        memset(&block, 0, sizeof(block));
        block.kind = CM_HIR_EXPR_BLOCK;
        block.type = return_type;
        block.span = test_span(start + 9u, start + 13u);
        block.data.block.tail_expression = integer_id;
        assert(cm_hir_add_expr(hir, &block, &block_id) == CM_HIR_OK);
    }
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = state;
    body.expected_type = return_type;
    body.source = 1u;
    body.source_expression_id = start + 1u;
    body.root_expression = block_id;
    body.span = test_span(start, start + 20u);
    assert(cm_hir_add_body(hir, &body, &body_id) == CM_HIR_OK);

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 20u);
    item.data.function_item.signature.return_type = return_type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "C");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    return body_id;
}

static void assert_dump(const CmMirContext *mir)
{
    static const char expected[] =
        "mir-v9 pointer-bits=0\n"
        "body#1 owner=1:2 source-body#1 locals=1 blocks=1\n"
        "local body#1 _0 kind=return type=ty#1\n"
        "block body#1 bb0 statements=1\n"
        "statement body#1 bb0[0] assign _0 = "
        "use(const-i32(7):ty#1) type=ty#1\n"
        "terminator body#1 bb0 return\n";
    FILE *stream;
    char buffer[512];
    size_t length;

    stream = tmpfile();
    assert(stream != NULL);
    assert(cm_mir_dump(stream, mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strcmp(buffer, expected) == 0);
    assert(fclose(stream) == 0);
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
    body.origin = cm_hir_body_origin_item_source(definition);
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
    body.origin = cm_hir_body_origin_item_source(definition);
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

static void add_mixed_argument_function(CmHirContext *hir,
    CmHirModuleId root_module, CmHirDefId definition, const char *name,
    CmHirTypeId first_type, CmHirTypeId second_type,
    CmHirTypeId return_type, uint32_t start, CmHirBodyId *out_body)
{
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;

    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    parameters[0].name = cm_hir_intern(hir, "value");
    parameters[0].type = first_type;
    parameters[0].span = test_span(start + 5u, start + 10u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = cm_hir_intern(hir, "offset");
    parameters[1].type = second_type;
    parameters[1].span = test_span(start + 12u, start + 18u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    locals[0].name = parameters[0].name;
    locals[0].type = first_type;
    locals[0].span = parameters[0].span;
    locals[0].parameter_index = 0u;
    locals[1].name = parameters[1].name;
    locals[1].type = second_type;
    locals[1].span = parameters[1].span;
    locals[1].parameter_index = 1u;

    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = return_type;
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
    item.data.function_item.signature.return_type = return_type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
}

typedef struct CallTreeFixture {
    CmHirDefId identity_definition;
    CmHirBodyId identity_body;
    CmHirBodyId caller_body;
    CmHirExprId first_local;
    CmHirExprId constant;
    CmHirExprId inner;
    CmHirExprId argument_root;
    CmHirExprId call;
} CallTreeFixture;

static void add_call_tree_fixture(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type, CallTreeFixture *fixture)
{
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirType parameter_type;
    CmHirTypeId parameter_type_id;
    CmHirDefId caller_definition;
    CmHirExpr expression;
    CmHirExprId identity_root;
    CmHirExprId second_local;
    CmHirTypeId substitution;
    CmHirExprId argument;

    memset(fixture, 0, sizeof(*fixture));
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(290u, 320u),
        &fixture->identity_definition) == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = fixture->identity_definition;
    generic.index = 0u;
    generic.name = cm_hir_intern(hir, "T");
    generic.span = test_span(294u, 295u);
    assert(cm_hir_add_generic_param(hir, &generic, &generic_id)
        == CM_HIR_OK);
    memset(&parameter_type, 0, sizeof(parameter_type));
    parameter_type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type.span = generic.span;
    parameter_type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(hir, &parameter_type, &parameter_type_id)
        == CM_HIR_OK);
    add_one_argument_function(hir, root_module,
        fixture->identity_definition, "call_identity", parameter_type_id,
        parameter_type_id, generic_id, 1u, 290u,
        &fixture->identity_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->identity_body;
    expression.type = parameter_type_id;
    expression.span = test_span(305u, 306u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &identity_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->identity_body,
        identity_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(330u, 360u), &caller_definition)
        == CM_HIR_OK);
    add_one_argument_function(hir, root_module, caller_definition,
        "call_nested", u32_type, u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u,
        330u, &fixture->caller_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(342u, 343u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->first_local)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(346u, 347u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->constant)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(350u, 351u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &second_local) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(345u, 352u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->constant;
    expression.data.binary.right = second_local;
    assert(cm_hir_add_expr(hir, &expression, &fixture->inner)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(341u, 353u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->first_local;
    expression.data.binary.right = fixture->inner;
    assert(cm_hir_add_expr(hir, &expression, &fixture->argument_root)
        == CM_HIR_OK);
    substitution = u32_type;
    argument = fixture->argument_root;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(340u, 355u);
    expression.data.call.callee = fixture->identity_definition;
    expression.data.call.type_substitutions = &substitution;
    expression.data.call.type_substitution_count = 1u;
    expression.data.call.arguments = &argument;
    expression.data.call.argument_count = 1u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->call) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->caller_body,
        fixture->call) == CM_HIR_OK);
}

typedef struct OrdinaryCallFixture {
    CmHirDefId unary_definition;
    CmHirBodyId unary_body;
    CmHirBodyId unary_caller_body;
    CmHirExprId unary_argument;
    CmHirExprId unary_call;
    CmHirDefId binary_definition;
    CmHirBodyId binary_body;
    CmHirBodyId binary_caller_body;
    CmHirExprId first_inner;
    CmHirExprId first_root;
    CmHirExprId maximum;
    CmHirExprId second_inner;
    CmHirExprId second_root;
    CmHirExprId binary_call;
    CmHirBodyId nested_caller_body;
    CmHirExprId nested_inner_call;
    CmHirExprId nested_later_add;
    CmHirExprId nested_three;
    CmHirExprId nested_outer_call;
    CmHirExprId add_after_call_root;
} OrdinaryCallFixture;

static void add_ordinary_call_fixture(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type, OrdinaryCallFixture *fixture)
{
    CmHirDefId caller_definition;
    CmHirExpr expression;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId constant;
    CmHirExprId first_y;
    CmHirExprId second_y;
    CmHirExprId second_x;
    CmHirExprId nested_left_add;
    CmHirExprId nested_right_add;
    CmHirExprId arguments[2];

    memset(fixture, 0, sizeof(*fixture));
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(370u, 400u),
        &fixture->unary_definition) == CM_HIR_OK);
    add_one_argument_function(hir, root_module, fixture->unary_definition,
        "ordinary_identity", u32_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 370u, &fixture->unary_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->unary_body;
    expression.type = u32_type;
    expression.span = test_span(385u, 386u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->unary_body, left)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(410u, 440u), &caller_definition)
        == CM_HIR_OK);
    add_one_argument_function(hir, root_module, caller_definition,
        "call_ordinary_identity", u32_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 410u,
        &fixture->unary_caller_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->unary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(425u, 426u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->unary_argument)
        == CM_HIR_OK);
    arguments[0] = fixture->unary_argument;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->unary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(422u, 428u);
    expression.data.call.callee = fixture->unary_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 1u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->unary_call)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir,
        fixture->unary_caller_body, fixture->unary_call) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(450u, 500u),
        &fixture->binary_definition) == CM_HIR_OK);
    add_two_argument_function(hir, root_module,
        fixture->binary_definition, "ordinary_add", u32_type, 450u,
        &fixture->binary_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->binary_body;
    expression.type = u32_type;
    expression.span = test_span(475u, 476u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    expression.span = test_span(478u, 479u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->binary_body;
    expression.type = u32_type;
    expression.span = test_span(474u, 480u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->binary_body, left)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(510u, 560u), &caller_definition)
        == CM_HIR_OK);
    add_two_argument_function(hir, root_module, caller_definition,
        "call_ordinary_add", u32_type, 510u,
        &fixture->binary_caller_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(530u, 531u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(533u, 534u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(hir, &expression, &constant) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(535u, 536u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &first_y) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(532u, 537u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = constant;
    expression.data.binary.right = first_y;
    assert(cm_hir_add_expr(hir, &expression, &fixture->first_inner)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(529u, 538u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left;
    expression.data.binary.right = fixture->first_inner;
    assert(cm_hir_add_expr(hir, &expression, &fixture->first_root)
        == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(540u, 541u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &second_y) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(542u, 543u);
    expression.data.integer.low_bits = (uint64_t)UINT32_MAX;
    assert(cm_hir_add_expr(hir, &expression, &fixture->maximum)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(539u, 544u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = second_y;
    expression.data.binary.right = fixture->maximum;
    assert(cm_hir_add_expr(hir, &expression, &fixture->second_inner)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(545u, 546u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &second_x) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(538u, 547u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->second_inner;
    expression.data.binary.right = second_x;
    assert(cm_hir_add_expr(hir, &expression, &fixture->second_root)
        == CM_HIR_OK);

    arguments[0] = fixture->first_root;
    arguments[1] = fixture->second_root;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->binary_caller_body;
    expression.type = u32_type;
    expression.span = test_span(528u, 548u);
    expression.data.call.callee = fixture->binary_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->binary_call)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir,
        fixture->binary_caller_body, fixture->binary_call) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(570u, 620u), &caller_definition)
        == CM_HIR_OK);
    add_two_argument_function(hir, root_module, caller_definition,
        "call_nested_ordinary_add", u32_type, 570u,
        &fixture->nested_caller_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(590u, 591u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(591u, 592u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(hir, &expression, &constant) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(589u, 593u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left;
    expression.data.binary.right = constant;
    assert(cm_hir_add_expr(hir, &expression, &nested_left_add)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(594u, 595u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(595u, 596u);
    expression.data.integer.low_bits = 2u;
    assert(cm_hir_add_expr(hir, &expression, &constant) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(593u, 597u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = right;
    expression.data.binary.right = constant;
    assert(cm_hir_add_expr(hir, &expression, &nested_right_add)
        == CM_HIR_OK);
    arguments[0] = nested_left_add;
    arguments[1] = nested_right_add;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(588u, 598u);
    expression.data.call.callee = fixture->binary_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_inner_call)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(600u, 601u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(601u, 602u);
    expression.data.integer.low_bits = 3u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_three)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(599u, 603u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left;
    expression.data.binary.right = fixture->nested_three;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_later_add)
        == CM_HIR_OK);
    arguments[0] = fixture->nested_inner_call;
    arguments[1] = fixture->nested_later_add;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(587u, 605u);
    expression.data.call.callee = fixture->binary_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_outer_call)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->nested_caller_body;
    expression.type = u32_type;
    expression.span = test_span(587u, 605u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->nested_inner_call;
    expression.data.binary.right = fixture->nested_later_add;
    assert(cm_hir_add_expr(hir, &expression,
        &fixture->add_after_call_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir,
        fixture->nested_caller_body, fixture->nested_outer_call)
        == CM_HIR_OK);
}

typedef struct NestedAddFixture {
    CmHirBodyId body;
    CmHirExprId inner_left;
    CmHirExprId inner_right;
    CmHirExprId maximum;
    CmHirExprId root;
} NestedAddFixture;

static void add_nested_add_function(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, CmHirTypeId u32_type,
    NestedAddFixture *fixture)
{
    CmHirDefId definition;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExpr expression;
    CmHirExprId left_x;
    CmHirExprId left_y;
    CmHirExprId right_y;
    uint32_t index;

    memset(fixture, 0, sizeof(*fixture));
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(220u, 280u), &definition)
        == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    for (index = 0u; index < 2u; ++index) {
        parameters[index].name = cm_hir_intern(hir,
            index == 0u ? "x" : "y");
        parameters[index].type = u32_type;
        parameters[index].span = test_span(225u + index * 5u,
            229u + index * 5u);
        parameters[index].binding_kind = CM_HIR_BINDING_NAMED;
        locals[index].name = parameters[index].name;
        locals[index].type = u32_type;
        locals[index].span = parameters[index].span;
        locals[index].parameter_index = index;
    }
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 220u;
    body.span = test_span(220u, 280u);
    assert(cm_hir_add_body(hir, &body, &fixture->body) == CM_HIR_OK);

    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, "nested_add");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = u32_type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = fixture->body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(240u, 241u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left_x) == CM_HIR_OK);
    expression.span = test_span(242u, 243u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &left_y) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(239u, 244u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left_x;
    expression.data.binary.right = left_y;
    assert(cm_hir_add_expr(hir, &expression, &fixture->inner_left)
        == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(246u, 247u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right_y) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(248u, 258u);
    expression.data.integer.low_bits = (uint64_t)UINT32_MAX;
    assert(cm_hir_add_expr(hir, &expression, &fixture->maximum)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(245u, 259u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = right_y;
    expression.data.binary.right = fixture->maximum;
    assert(cm_hir_add_expr(hir, &expression, &fixture->inner_right)
        == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->body;
    expression.type = u32_type;
    expression.span = test_span(238u, 260u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = fixture->inner_left;
    expression.data.binary.right = fixture->inner_right;
    assert(cm_hir_add_expr(hir, &expression, &fixture->root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->body,
        fixture->root) == CM_HIR_OK);
}

typedef struct LetFlowFixture {
    CmHirDefId callee_definition;
    CmHirBodyId callee_body;
    CmHirBodyId caller_body;
    CmHirExprId caller_block;
} LetFlowFixture;

static void add_let_flow_fixture(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, CmHirTypeId u32_type,
    LetFlowFixture *fixture)
{
    CmHirDefId caller_definition;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[4];
    CmHirBody body;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExpr expression;
    CmHirExprId callee_left;
    CmHirExprId callee_right;
    CmHirExprId callee_root;
    CmHirExprId left;
    CmHirExprId one;
    CmHirExprId left_add;
    CmHirExprId right;
    CmHirExprId two;
    CmHirExprId right_add;
    CmHirExprId call;
    CmHirExprId sum;
    CmHirExprId later_left;
    CmHirExprId later;
    CmHirExprId tail;
    CmHirExprId arguments[2];
    CmHirStatement statements[2];
    uint32_t index;

    memset(fixture, 0, sizeof(*fixture));
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(630u, 680u),
        &fixture->callee_definition) == CM_HIR_OK);
    add_two_argument_function(hir, root_module, fixture->callee_definition,
        "let_add", u32_type, 630u, &fixture->callee_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->callee_body;
    expression.type = u32_type;
    expression.span = test_span(650u, 651u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &callee_left) == CM_HIR_OK);
    expression.span = test_span(652u, 653u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &callee_right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->callee_body;
    expression.type = u32_type;
    expression.span = test_span(649u, 654u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = callee_left;
    expression.data.binary.right = callee_right;
    assert(cm_hir_add_expr(hir, &expression, &callee_root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->callee_body,
        callee_root) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(640u, 700u), &caller_definition)
        == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    for (index = 0u; index < 2u; ++index) {
        parameters[index].name = cm_hir_intern(hir,
            index == 0u ? "left" : "right");
        parameters[index].type = u32_type;
        parameters[index].span = test_span(642u + index * 5u,
            646u + index * 5u);
        parameters[index].binding_kind = CM_HIR_BINDING_NAMED;
        locals[index].name = parameters[index].name;
        locals[index].type = u32_type;
        locals[index].span = parameters[index].span;
        locals[index].parameter_index = index;
    }
    locals[2].name = cm_hir_intern(hir, "sum");
    locals[2].type = u32_type;
    locals[2].span = test_span(655u, 669u);
    locals[2].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
    locals[3].name = cm_hir_intern(hir, "later");
    locals[3].type = u32_type;
    locals[3].span = test_span(673u, 680u);
    locals[3].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
    memset(&body, 0, sizeof(body));
    body.owner = caller_definition;
    body.origin = cm_hir_body_origin_item_source(caller_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.locals = locals;
    body.local_count = 4u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 640u;
    body.span = test_span(640u, 700u);
    assert(cm_hir_add_body(hir, &body, &fixture->caller_body) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = caller_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, "let_flow");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = u32_type;
    item.data.function_item.signature.abi = cm_hir_intern(hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = fixture->caller_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(658u, 659u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(660u, 661u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(hir, &expression, &one) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(657u, 662u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = left;
    expression.data.binary.right = one;
    assert(cm_hir_add_expr(hir, &expression, &left_add) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(663u, 664u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(665u, 666u);
    expression.data.integer.low_bits = 2u;
    assert(cm_hir_add_expr(hir, &expression, &two) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(662u, 667u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = right;
    expression.data.binary.right = two;
    assert(cm_hir_add_expr(hir, &expression, &right_add) == CM_HIR_OK);
    arguments[0] = left_add;
    arguments[1] = right_add;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(656u, 668u);
    expression.data.call.callee = fixture->callee_definition;
    expression.data.call.arguments = arguments;
    expression.data.call.argument_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &call) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(675u, 676u);
    expression.data.local.local_index = 2u;
    assert(cm_hir_add_expr(hir, &expression, &sum) == CM_HIR_OK);
    expression.span = test_span(677u, 678u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &later_left) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(674u, 679u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = sum;
    expression.data.binary.right = later_left;
    assert(cm_hir_add_expr(hir, &expression, &later) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(685u, 686u);
    expression.data.local.local_index = 3u;
    assert(cm_hir_add_expr(hir, &expression, &tail) == CM_HIR_OK);

    memset(statements, 0, sizeof(statements));
    statements[0].kind = CM_HIR_STATEMENT_LET;
    statements[0].span = test_span(655u, 669u);
    statements[0].data.let_statement.local_index = 2u;
    statements[0].data.let_statement.initializer = call;
    statements[1].kind = CM_HIR_STATEMENT_LET;
    statements[1].span = test_span(673u, 680u);
    statements[1].data.let_statement.local_index = 3u;
    statements[1].data.let_statement.initializer = later;
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.owner_body = fixture->caller_body;
    expression.type = u32_type;
    expression.span = test_span(654u, 690u);
    expression.data.block.statements = statements;
    expression.data.block.statement_count = 2u;
    expression.data.block.tail_expression = tail;
    assert(cm_hir_add_expr(hir, &expression, &fixture->caller_block)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->caller_body,
        fixture->caller_block) == CM_HIR_OK);
}

static CmHirExpr *mutable_expression(CmHirContext *hir, CmHirExprId id)
{
    assert(id != CM_HIR_EXPR_NONE);
    return (CmHirExpr *)cm_vec_at(&hir->expressions, (size_t)id - 1u);
}

static CmHirBody *mutable_body(CmHirContext *hir, CmHirBodyId id)
{
    assert(id != CM_HIR_BODY_NONE);
    return (CmHirBody *)cm_vec_at(&hir->bodies, (size_t)id - 1u);
}

static void assert_nested_failure(CmMirContext *mir,
    const CmHirContext *hir, CmHirBodyId body,
    CmMirLowerErrorKind expected)
{
    CmMirLowerResult result;
    size_t count;

    count = cm_mir_body_count(mir);
    result = cm_mir_lower_instance(mir, hir, body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == expected
        && cm_mir_body_count(mir) == count);
}

static void assert_nested_dump(const CmMirContext *mir)
{
    static const char expected[] =
        "mir-v9 pointer-bits=0\n"
        "body#1 instance=1:7/body=1:7 source-body#6 locals=5 blocks=1\n"
        "local body#1 _0 kind=return type=ty#2\n"
        "local body#1 _1 kind=argument type=ty#2\n"
        "local body#1 _2 kind=argument type=ty#2\n"
        "local body#1 _3 kind=temporary type=ty#2\n"
        "local body#1 _4 kind=temporary type=ty#2\n"
        "statement body#1 bb0[0] assign _3 = binary(add,"
        "move _1:ty#2,move _2:ty#2) type=ty#2\n"
        "statement body#1 bb0[1] assign _4 = binary(add,"
        "move _2:ty#2,const-u32(4294967295):ty#2) type=ty#2\n"
        "statement body#1 bb0[2] assign _0 = binary(add,"
        "move _3:ty#2,move _4:ty#2) type=ty#2\n"
        "terminator body#1 bb0 return\n";
    FILE *stream;
    char buffer[2048];
    size_t length;

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strcmp(buffer, expected) == 0);
    assert(fclose(stream) == 0);
}

static void test_let_flow_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, CmHirTypeId u32_type)
{
    LetFlowFixture fixture;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    const CmMirBasicBlock *first;
    const CmMirBasicBlock *second;
    const CmMirStatement *statements;
    CmHirExpr *block;
    uint32_t saved_local;
    size_t count;
    FILE *stream;
    char buffer[4096];
    size_t length;

    add_let_flow_fixture(hir, crate_id, root_module, u32_type, &fixture);
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.callee_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    result = cm_mir_lower_instance(&mir, hir, fixture.caller_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 2u && cm_mir_body_count(&mir) == 2u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 7u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[2].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[3].kind == CM_MIR_LOCAL_USER
        && stored->locals[4].kind == CM_MIR_LOCAL_USER
        && stored->locals[5].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[6].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->basic_block_count == 2u);
    first = &stored->basic_blocks[0];
    second = &stored->basic_blocks[1];
    assert(first->statement_count == 2u && first->statements != NULL
        && first->statements[0].data.assign.destination == 5u
        && first->statements[1].data.assign.destination == 6u
        && first->terminator.kind == CM_MIR_TERMINATOR_CALL
        && first->terminator.data.call.destination == 3u
        && first->terminator.data.call.argument_count == 2u
        && first->terminator.data.call.arguments[0].data.local == 5u
        && first->terminator.data.call.arguments[1].data.local == 6u
        && first->terminator.data.call.callee_instance == 1u
        && first->terminator.data.call.target == 1u);
    assert(second->statement_count == 2u && second->statements != NULL
        && second->terminator.kind == CM_MIR_TERMINATOR_RETURN);
    statements = second->statements;
    assert(statements[0].data.assign.destination == 4u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_BINARY
        && statements[0].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.left.data.local == 3u
        && statements[0].data.assign.value.data.binary.right.data.local == 1u
        && statements[1].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[1].data.assign.value.kind == CM_MIR_RVALUE_USE
        && statements[1].data.assign.value.data.use.kind
            == CM_MIR_OPERAND_MOVE
        && statements[1].data.assign.value.data.use.data.local == 4u);
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_OK);

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "mir-v9 pointer-bits=0\n") == buffer
        && strstr(buffer, "local body#2 _3 kind=user type=ty#2\n") != NULL
        && strstr(buffer, "local body#2 _4 kind=user type=ty#2\n") != NULL
        && strstr(buffer,
            "terminator body#2 bb0 call destination=_3") != NULL);
    assert(fclose(stream) == 0);

    block = mutable_expression(hir, fixture.caller_block);
    saved_local = block->data.block.statements[1].data.let_statement
        .local_index;
    block->data.block.statements[1].data.let_statement.local_index = 2u;
    count = cm_mir_body_count(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.caller_body,
        NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_INVALID_HIR
        && cm_mir_body_count(&mir) == count);
    block->data.block.statements[1].data.let_statement.local_index =
        saved_local;
    cm_mir_context_destroy(&mir);
}

static void test_nested_add_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, CmHirTypeId u32_type,
    CmHirTypeId alternate_u32_type, CmHirTypeId i32_type,
    CmHirBodyId foreign_body)
{
    NestedAddFixture fixture;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    const CmMirStatement *statements;
    CmHirExpr *inner;
    CmHirExpr *local;
    CmHirExpr *maximum;
    CmHirExpr saved;

    add_nested_add_function(hir, crate_id, root_module, u32_type, &fixture);
    inner = mutable_expression(hir, fixture.inner_left);
    local = mutable_expression(hir, inner->data.binary.left);
    local->type = alternate_u32_type;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.body, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 1u && cm_mir_body_count(&mir) == 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 5u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[2].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[3].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[4].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[3].type == u32_type
        && stored->locals[4].type == u32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 3u);
    statements = stored->basic_blocks[0].statements;
    assert(statements[0].data.assign.destination == 3u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_BINARY
        && statements[0].data.assign.value.data.binary.operator_kind
            == CM_MIR_BINARY_ADD
        && statements[0].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.left.type == u32_type
        && statements[0].data.assign.value.data.binary.left.data.local == 1u
        && statements[0].data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.right.type == u32_type
        && statements[0].data.assign.value.data.binary.right.data.local
            == 2u);
    assert(statements[1].data.assign.destination == 4u
        && statements[1].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[1].data.assign.value.data.binary.left.type == u32_type
        && statements[1].data.assign.value.data.binary.left.data.local == 2u
        && statements[1].data.assign.value.data.binary.right.kind
            == CM_MIR_CONSTANT_U32
        && statements[1].data.assign.value.data.binary.right.data.u32_value
            == UINT32_MAX);
    assert(statements[2].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[2].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[2].data.assign.value.data.binary.left.type == u32_type
        && statements[2].data.assign.value.data.binary.left.data.local == 3u
        && statements[2].data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statements[2].data.assign.value.data.binary.right.type == u32_type
        && statements[2].data.assign.value.data.binary.right.data.local
            == 4u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    assert_nested_dump(&mir);

    maximum = mutable_expression(hir, fixture.maximum);
    saved = *maximum;
    maximum->data.integer.high_bits = 1u;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE);
    *maximum = saved;

    inner = mutable_expression(hir, fixture.inner_right);
    saved = *inner;
    inner->owner_body = foreign_body;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_INVALID_HIR);
    *inner = saved;

    saved = *inner;
    inner->type = i32_type;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_INVALID_HIR);
    *inner = saved;

    saved = *inner;
    inner->kind = CM_HIR_EXPR_BLOCK;
    inner->data.block.tail_expression = CM_HIR_EXPR_NONE;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    *inner = saved;

    inner = mutable_expression(hir, fixture.inner_left);
    saved = *inner;
    inner->data.binary.left = CM_HIR_EXPR_NONE;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_INVALID_HIR);
    *inner = saved;

    saved = *inner;
    inner->data.binary.left = fixture.root;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_INVALID_HIR);
    *inner = saved;

    inner = mutable_expression(hir, fixture.root);
    saved = *inner;
    inner->data.binary.right = fixture.inner_left;
    assert_nested_failure(&mir, hir, fixture.body,
        CM_MIR_LOWER_INVALID_HIR);
    *inner = saved;

    cm_mir_context_destroy(&mir);
}

static void test_subtract_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type)
{
    CmHirDefId definition;
    CmHirBodyId body;
    CmHirExpr expression;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId root;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    const CmMirStatement *statement;
    FILE *stream;
    char buffer[1024];
    size_t length;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(1260u, 1310u), &definition)
        == CM_HIR_OK);
    add_two_argument_function(hir, root_module, definition, "subtract",
        u32_type, 1260u, &body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1280u, 1283u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    expression.span = test_span(1286u, 1291u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1280u, 1291u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_SUBTRACT;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    assert(cm_hir_add_expr(hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, body, root) == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, body, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 1u);
    stored = cm_mir_get_body(&mir, result.body);
    statement = stored == NULL || stored->basic_block_count != 1u
            || stored->basic_blocks[0].statement_count != 1u
        ? NULL : &stored->basic_blocks[0].statements[0];
    assert(stored != NULL && stored->local_count == 3u
        && statement != NULL
        && statement->data.assign.destination == CM_MIR_RETURN_LOCAL
        && statement->data.assign.value.kind == CM_MIR_RVALUE_BINARY
        && statement->data.assign.value.data.binary.operator_kind
            == CM_MIR_BINARY_SUBTRACT
        && statement->data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statement->data.assign.value.data.binary.left.data.local == 1u
        && statement->data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statement->data.assign.value.data.binary.right.data.local == 2u);

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "binary(subtract,") != NULL);
    assert(fclose(stream) == 0);
    cm_mir_context_destroy(&mir);
}

static void test_if_equal_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type)
{
    CmHirTypeId bool_type;
    CmHirDefId definition;
    CmHirBodyId body;
    CmHirExpr expression;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId condition;
    CmHirExprId then_value;
    CmHirExprId else_value;
    CmHirExprId then_block;
    CmHirExprId else_block;
    CmHirExprId root;
    CmMirContext mir;
    CmMirContext owned;
    CmMirContext rejected;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody candidate;
    CmMirLocal candidate_locals[4];
    CmMirBasicBlock candidate_blocks[4];
    CmMirStatement candidate_statements[3];
    CmMirBody *mutable_stored;
    CmMirRvalue saved_rvalue;
    CmMirOperand saved_operand;
    CmMirTerminator saved_terminator;
    CmHirExpr *mutable_block;
    CmHirExprId saved_tail;
    CmMirBodyId added;
    uint32_t block_index;
    uint32_t statement_index;
    size_t count;
    FILE *stream;
    char buffer[2048];
    size_t length;

    bool_type = add_bool_type(hir, 1320u);
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(1322u, 1380u), &definition)
        == CM_HIR_OK);
    add_two_argument_function(hir, root_module, definition, "if_equal",
        u32_type, 1322u, &body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1340u, 1344u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    expression.span = test_span(1348u, 1353u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = bool_type;
    expression.span = test_span(1340u, 1353u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_EQUAL;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    assert(cm_hir_add_expr(hir, &expression, &condition) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1357u, 1358u);
    expression.data.integer.low_bits = 7u;
    assert(cm_hir_add_expr(hir, &expression, &then_value) == CM_HIR_OK);
    expression.span = test_span(1367u, 1368u);
    expression.data.integer.low_bits = 9u;
    assert(cm_hir_add_expr(hir, &expression, &else_value) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1355u, 1360u);
    expression.data.block.tail_expression = then_value;
    assert(cm_hir_add_expr(hir, &expression, &then_block) == CM_HIR_OK);
    expression.span = test_span(1365u, 1370u);
    expression.data.block.tail_expression = else_value;
    assert(cm_hir_add_expr(hir, &expression, &else_block) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_IF;
    expression.owner_body = body;
    expression.type = u32_type;
    expression.span = test_span(1338u, 1370u);
    expression.data.if_expr.condition = condition;
    expression.data.if_expr.then_expression = then_block;
    expression.data.if_expr.else_expression = else_block;
    assert(cm_hir_add_expr(hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, body, root) == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, body, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->local_count == 4u
        && stored->locals[3].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[3].type == bool_type
        && stored->basic_block_count == 4u
        && stored->basic_blocks[0].statement_count == 1u
        && stored->basic_blocks[0].statements[0].data.assign.destination
            == 3u
        && stored->basic_blocks[0].statements[0].data.assign.value.kind
            == CM_MIR_RVALUE_EQUAL
        && stored->basic_blocks[0].statements[0].data.assign.value.data.equal
            .left.kind == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].statements[0].data.assign.value.data.equal
            .left.data.local == 1u
        && stored->basic_blocks[0].statements[0].data.assign.value.data.equal
            .right.kind == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].statements[0].data.assign.value.data.equal
            .right.data.local == 2u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_SWITCH_BOOL
        && stored->basic_blocks[0].terminator.data.switch_bool.condition.kind
            == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].terminator.data.switch_bool.condition.data
            .local == 3u
        && stored->basic_blocks[0].terminator.data.switch_bool.true_target
            == 1u
        && stored->basic_blocks[0].terminator.data.switch_bool.false_target
            == 2u
        && stored->basic_blocks[1].statement_count == 1u
        && stored->basic_blocks[1].statements[0].data.assign.destination
            == CM_MIR_RETURN_LOCAL
        && stored->basic_blocks[1].statements[0].data.assign.value.data.use
            .data.u32_value == 7u
        && stored->basic_blocks[1].terminator.kind
            == CM_MIR_TERMINATOR_GOTO
        && stored->basic_blocks[1].terminator.data.goto_block.target == 3u
        && stored->basic_blocks[2].statement_count == 1u
        && stored->basic_blocks[2].statements[0].data.assign.value.data.use
            .data.u32_value == 9u
        && stored->basic_blocks[2].terminator.kind
            == CM_MIR_TERMINATOR_GOTO
        && stored->basic_blocks[2].terminator.data.goto_block.target == 3u
        && stored->basic_blocks[3].statement_count == 0u
        && stored->basic_blocks[3].statements == NULL
        && stored->basic_blocks[3].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_OK);

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "equal(move _1:ty#") != NULL
        && strstr(buffer, " switch-bool move _3 type=ty#") != NULL
        && strstr(buffer, "bb1 goto bb3") != NULL
        && strstr(buffer, "bb2 goto bb3") != NULL);
    assert(fclose(stream) == 0);

    candidate = *stored;
    memcpy(candidate_locals, stored->locals, sizeof(candidate_locals));
    memcpy(candidate_blocks, stored->basic_blocks, sizeof(candidate_blocks));
    statement_index = 0u;
    for (block_index = 0u; block_index < 4u; ++block_index) {
        uint32_t index;

        candidate_blocks[block_index].statements =
            candidate_blocks[block_index].statement_count == 0u ? NULL
            : &candidate_statements[statement_index];
        for (index = 0u;
             index < stored->basic_blocks[block_index].statement_count;
             ++index) {
            candidate_statements[statement_index++] =
                stored->basic_blocks[block_index].statements[index];
        }
    }
    assert(statement_index == 3u);
    candidate.locals = candidate_locals;
    candidate.basic_blocks = candidate_blocks;
    candidate.owned_storage = NULL;
    cm_mir_context_init(&owned);
    added = CM_MIR_BODY_NONE;
    assert(cm_mir_add_monomorphized_body(&owned, hir, &candidate, &added)
        == CM_MIR_OK && added == 1u);
    candidate_statements[0].data.assign.value.data.equal.left.data.local =
        2u;
    candidate_blocks[0].terminator.data.switch_bool.false_target = 3u;
    candidate_locals[3].type = u32_type;
    assert(cm_mir_validate_monomorphized_body(&owned, hir, added)
        == CM_MIR_OK);
    cm_mir_context_destroy(&owned);

    cm_mir_context_init(&rejected);
    count = cm_mir_body_count(&rejected);
    added = 99u;
    assert(cm_mir_add_monomorphized_body(&rejected, hir, &candidate,
            &added) == CM_MIR_INVARIANT_VIOLATION
        && added == CM_MIR_BODY_NONE
        && cm_mir_body_count(&rejected) == count);
    cm_mir_context_destroy(&rejected);

    mutable_stored = (CmMirBody *)stored;
    saved_rvalue = mutable_stored->basic_blocks[0].statements[0]
        .data.assign.value;
    mutable_stored->basic_blocks[0].statements[0].data.assign.value.kind =
        CM_MIR_RVALUE_BINARY;
    mutable_stored->basic_blocks[0].statements[0].data.assign.value
        .data.binary.operator_kind = CM_MIR_BINARY_ADD;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].statements[0].data.assign.value =
        saved_rvalue;
    saved_operand = mutable_stored->basic_blocks[0].statements[0]
        .data.assign.value.data.equal.left;
    mutable_stored->basic_blocks[0].statements[0].data.assign.value
        .data.equal.left.data.local = 2u;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].statements[0].data.assign.value
        .data.equal.left = saved_operand;
    saved_terminator = mutable_stored->basic_blocks[0].terminator;
    mutable_stored->basic_blocks[0].terminator.data.switch_bool.true_target =
        2u;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].terminator = saved_terminator;
    saved_terminator = mutable_stored->basic_blocks[1].terminator;
    mutable_stored->basic_blocks[1].terminator.data.goto_block.target = 2u;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[1].terminator = saved_terminator;
    saved_operand = mutable_stored->basic_blocks[2].statements[0]
        .data.assign.value.data.use;
    mutable_stored->basic_blocks[2].statements[0].data.assign.value.data.use
        .data.u32_value = 7u;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[2].statements[0].data.assign.value.data.use =
        saved_operand;
    assert(cm_mir_validate_monomorphized_body(&mir, hir, result.body)
        == CM_MIR_OK);

    mutable_block = mutable_expression(hir, then_block);
    saved_tail = mutable_block->data.block.tail_expression;
    mutable_block->data.block.tail_expression = root;
    count = cm_mir_body_count(&mir);
    result = cm_mir_lower_instance(&mir, hir, body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_INVALID_HIR
        && cm_mir_body_count(&mir) == count);
    mutable_block->data.block.tail_expression = saved_tail;

    cm_mir_context_destroy(&mir);
}

static void test_usize_less_if_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId usize_type)
{
    CmHirTypeId bool_type;
    CmHirDefId definition;
    CmHirBodyId body;
    CmHirExpr expression;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId one;
    CmHirExprId then_left;
    CmHirExprId else_left;
    CmHirExprId else_right;
    CmHirExprId condition;
    CmHirExprId then_value;
    CmHirExprId else_value;
    CmHirExprId then_block;
    CmHirExprId else_block;
    CmHirExprId root;
    CmMirContext mir64;
    CmMirContext mir32;
    CmMirContext unset;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmHirExpr *mutable_one;
    CmMirRvalue saved;
    FILE *stream;
    char buffer[2048];
    size_t length;

    bool_type = add_bool_type(hir, 3200u);
    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(3202u, 3280u), &definition)
        == CM_HIR_OK);
    add_two_argument_function(hir, root_module, definition, "usize_choose",
        usize_type, 3202u, &body);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = usize_type;
    expression.span = test_span(3220u, 3224u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &left) == CM_HIR_OK);
    expression.span = test_span(3227u, 3230u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &right) == CM_HIR_OK);

    expression.span = test_span(3234u, 3235u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &then_left) == CM_HIR_OK);
    expression.span = test_span(3242u, 3243u);
    assert(cm_hir_add_expr(hir, &expression, &else_left) == CM_HIR_OK);
    expression.span = test_span(3246u, 3249u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &else_right) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = body;
    expression.type = usize_type;
    expression.span = test_span(3237u, 3238u);
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(hir, &expression, &one) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = bool_type;
    expression.span = test_span(3220u, 3230u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_LESS;
    expression.data.binary.left = left;
    expression.data.binary.right = right;
    assert(cm_hir_add_expr(hir, &expression, &condition) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = body;
    expression.type = usize_type;
    expression.span = test_span(3234u, 3238u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = then_left;
    expression.data.binary.right = one;
    assert(cm_hir_add_expr(hir, &expression, &then_value) == CM_HIR_OK);
    expression.span = test_span(3242u, 3249u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_SUBTRACT;
    expression.data.binary.left = else_left;
    expression.data.binary.right = else_right;
    assert(cm_hir_add_expr(hir, &expression, &else_value) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.owner_body = body;
    expression.type = usize_type;
    expression.span = test_span(3232u, 3240u);
    expression.data.block.tail_expression = then_value;
    assert(cm_hir_add_expr(hir, &expression, &then_block) == CM_HIR_OK);
    expression.span = test_span(3241u, 3250u);
    expression.data.block.tail_expression = else_value;
    assert(cm_hir_add_expr(hir, &expression, &else_block) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_IF;
    expression.owner_body = body;
    expression.type = usize_type;
    expression.span = test_span(3218u, 3250u);
    expression.data.if_expr.condition = condition;
    expression.data.if_expr.then_expression = then_block;
    expression.data.if_expr.else_expression = else_block;
    assert(cm_hir_add_expr(hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, body, root) == CM_HIR_OK);

    cm_mir_context_init(&unset);
    result = cm_mir_lower_instance(&unset, hir, body, NULL, 0u);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_UNSUPPORTED_TYPE
        && cm_mir_body_count(&unset) == 0u);
    cm_mir_context_destroy(&unset);

    cm_mir_context_init(&mir32);
    assert(cm_mir_context_set_pointer_bits(&mir32, 32u) == CM_MIR_OK);
    result = cm_mir_lower_instance(&mir32, hir, body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u
        && cm_mir_validate_monomorphized_body(&mir32, hir, result.body)
            == CM_MIR_OK);
    assert(cm_mir_context_set_pointer_bits(&mir32, 32u)
        == CM_MIR_INVARIANT_VIOLATION);
    cm_mir_context_destroy(&mir32);

    mutable_one = mutable_expression(hir, one);
    assert(mutable_one != NULL);
    mutable_one->data.integer.low_bits = UINT64_MAX;
    cm_mir_context_init(&mir32);
    assert(cm_mir_context_set_pointer_bits(&mir32, 32u) == CM_MIR_OK);
    result = cm_mir_lower_instance(&mir32, hir, body, NULL, 0u);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE
        && cm_mir_body_count(&mir32) == 0u);
    cm_mir_context_destroy(&mir32);

    cm_mir_context_init(&mir64);
    assert(cm_mir_context_set_pointer_bits(&mir64, 64u) == CM_MIR_OK);
    result = cm_mir_lower_instance(&mir64, hir, body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    stored = cm_mir_get_body(&mir64, result.body);
    assert(stored != NULL && stored->local_count == 4u
        && stored->basic_block_count == 4u
        && stored->basic_blocks[0].statements[0].data.assign.value.kind
            == CM_MIR_RVALUE_LESS
        && stored->basic_blocks[1].statements[0].data.assign.value.kind
            == CM_MIR_RVALUE_BINARY
        && stored->basic_blocks[1].statements[0].data.assign.value.data
            .binary.operator_kind == CM_MIR_BINARY_ADD
        && stored->basic_blocks[1].statements[0].data.assign.value.data
            .binary.right.kind == CM_MIR_CONSTANT_USIZE
        && stored->basic_blocks[1].statements[0].data.assign.value.data
            .binary.right.data.usize_value == UINT64_MAX
        && stored->basic_blocks[2].statements[0].data.assign.value.data
            .binary.operator_kind == CM_MIR_BINARY_SUBTRACT);
    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir64) == 0);
    assert(fflush(stream) == 0);
    rewind(stream);
    length = fread(buffer, 1u, sizeof(buffer) - 1u, stream);
    assert(!ferror(stream));
    buffer[length] = '\0';
    assert(strstr(buffer, "mir-v9 pointer-bits=64\n") == buffer
        && strstr(buffer, "less(move _1:ty#") != NULL
        && strstr(buffer,
            "const-usize(18446744073709551615):ty#") != NULL);
    assert(fclose(stream) == 0);

    mutable_stored = (CmMirBody *)stored;
    saved = mutable_stored->basic_blocks[0].statements[0]
        .data.assign.value;
    mutable_stored->basic_blocks[0].statements[0].data.assign.value.kind =
        CM_MIR_RVALUE_EQUAL;
    assert(cm_mir_validate_monomorphized_body(&mir64, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].statements[0].data.assign.value = saved;
    saved = mutable_stored->basic_blocks[1].statements[0]
        .data.assign.value;
    mutable_stored->basic_blocks[1].statements[0].data.assign.value.data
        .binary.right.kind = CM_MIR_CONSTANT_U32;
    assert(cm_mir_validate_monomorphized_body(&mir64, hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[1].statements[0].data.assign.value = saved;
    assert(cm_mir_validate_monomorphized_body(&mir64, hir, result.body)
        == CM_MIR_OK);
    cm_mir_context_destroy(&mir64);
}

static void assert_call_tree_failure(CmMirContext *mir,
    const CmHirContext *hir, CmHirBodyId body,
    CmMirLowerErrorKind expected)
{
    CmMirLowerResult result;
    size_t count;

    count = cm_mir_body_count(mir);
    result = cm_mir_lower_instance(mir, hir, body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == expected
        && cm_mir_body_count(mir) == count);
}

static void test_call_argument_tree_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type, CmHirTypeId alternate_u32_type,
    CmHirBodyId foreign_body)
{
    CallTreeFixture fixture;
    CmHirTypeId substitution;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    const CmMirStatement *statements;
    const CmMirTerminator *terminator;
    CmHirExpr *expression;
    CmHirExpr saved;
    CmHirExprId saved_argument;

    add_call_tree_fixture(hir, crate_id, root_module, u32_type, &fixture);
    substitution = u32_type;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.caller_body,
        NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.first_error.kind == CM_MIR_LOWER_MODEL_FAILURE
        && result.first_error.mir_status == CM_MIR_INVALID_ID
        && cm_mir_body_count(&mir) == 0u);
    result = cm_mir_lower_instance(&mir, hir, fixture.identity_body,
        &substitution, 1u);
    assert(result.error_count == 0u && result.body == 1u
        && cm_mir_body_count(&mir) == 1u);

    expression = mutable_expression(hir, fixture.argument_root);
    expression->type = alternate_u32_type;
    result = cm_mir_lower_instance(&mir, hir, fixture.caller_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 2u && cm_mir_body_count(&mir) == 2u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 4u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[2].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[3].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[2].type == u32_type
        && stored->locals[3].type == u32_type
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 2u
        && stored->basic_blocks[1].statement_count == 0u
        && stored->basic_blocks[1].statements == NULL
        && stored->basic_blocks[1].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    statements = stored->basic_blocks[0].statements;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 2u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_BINARY
        && statements[0].data.assign.value.data.binary.operator_kind
            == CM_MIR_BINARY_ADD
        && statements[0].data.assign.value.data.binary.left.kind
            == CM_MIR_CONSTANT_U32
        && statements[0].data.assign.value.data.binary.left.data.u32_value
            == 1u
        && statements[0].data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.right.data.local
            == 1u);
    assert(statements[1].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[1].data.assign.destination == 3u
        && statements[1].data.assign.value.kind == CM_MIR_RVALUE_BINARY
        && statements[1].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[1].data.assign.value.data.binary.left.data.local
            == 1u
        && statements[1].data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statements[1].data.assign.value.data.binary.right.data.local
            == 2u);
    terminator = &stored->basic_blocks[0].terminator;
    assert(terminator->kind == CM_MIR_TERMINATOR_CALL
        && terminator->data.call.destination == CM_MIR_RETURN_LOCAL
        && terminator->data.call.argument_count == 1u
        && terminator->data.call.arguments != NULL
        && terminator->data.call.arguments[0].kind == CM_MIR_OPERAND_MOVE
        && terminator->data.call.arguments[0].type == u32_type
        && terminator->data.call.arguments[0].data.local == 3u
        && terminator->data.call.callee_instance == 1u
        && terminator->data.call.target == 1u);
    expression->type = u32_type;

    expression = mutable_expression(hir, fixture.constant);
    saved = *expression;
    expression->data.integer.high_bits = 1u;
    assert_call_tree_failure(&mir, hir, fixture.caller_body,
        CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE);
    *expression = saved;

    expression = mutable_expression(hir, fixture.inner);
    saved = *expression;
    expression->owner_body = foreign_body;
    assert_call_tree_failure(&mir, hir, fixture.caller_body,
        CM_MIR_LOWER_INVALID_HIR);
    *expression = saved;

    saved = *expression;
    expression->kind = CM_HIR_EXPR_BLOCK;
    expression->data.block.tail_expression = CM_HIR_EXPR_NONE;
    assert_call_tree_failure(&mir, hir, fixture.caller_body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    *expression = saved;

    expression = mutable_expression(hir, fixture.argument_root);
    saved = *expression;
    expression->data.binary.left = fixture.argument_root;
    assert_call_tree_failure(&mir, hir, fixture.caller_body,
        CM_MIR_LOWER_INVALID_HIR);
    *expression = saved;

    expression = mutable_expression(hir, fixture.call);
    saved_argument = expression->data.call.arguments[0];
    expression->data.call.arguments[0] = fixture.constant;
    assert_call_tree_failure(&mir, hir, fixture.caller_body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    expression->data.call.arguments[0] = saved_argument;
    cm_mir_context_destroy(&mir);

    expression->data.call.arguments[0] = fixture.first_local;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.identity_body,
        &substitution, 1u);
    assert(result.error_count == 0u && result.body == 1u);
    result = cm_mir_lower_instance(&mir, hir, fixture.caller_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 2u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 2u
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 0u
        && stored->basic_blocks[0].statements == NULL
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && stored->basic_blocks[0].terminator.data.call.arguments[0].kind
            == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 1u);
    cm_mir_context_destroy(&mir);
    expression->data.call.arguments[0] = saved_argument;
}

static void test_ordinary_call_lowering(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type, CmHirBodyId foreign_body)
{
    OrdinaryCallFixture fixture;
    CmMirContext mir;
    CmMirContext oom_mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    const CmMirStatement *statements;
    const CmMirTerminator *terminator;
    CmHirExpr *expression;
    CmHirExpr saved;
    CmHirExprId saved_argument;
    CmHirBody *hir_body;
    CmHirExprId saved_root;
    CmHirDefId saved_callee;

    add_ordinary_call_fixture(hir, crate_id, root_module, u32_type,
        &fixture);
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir,
        fixture.binary_caller_body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_MODEL_FAILURE
        && result.first_error.mir_status == CM_MIR_INVALID_ID
        && cm_mir_body_count(&mir) == 0u);
    result = cm_mir_lower_instance(&mir, hir,
        fixture.nested_caller_body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_MODEL_FAILURE
        && result.first_error.mir_status == CM_MIR_INVALID_ID
        && cm_mir_body_count(&mir) == 0u);
    result = cm_mir_lower_instance(&mir, hir, fixture.unary_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    result = cm_mir_lower_instance(&mir, hir, fixture.binary_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 2u);
    result = cm_mir_lower_instance(&mir, hir,
        fixture.unary_caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 3u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 2u
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 0u
        && stored->basic_blocks[0].statements == NULL
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && stored->basic_blocks[0].terminator.data.call.argument_count
            == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments[0].kind
            == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 1u
        && stored->basic_blocks[0].terminator.data.call.callee_instance
            == 1u
        && stored->basic_blocks[0].terminator.data.call.callee
            .substitution_count == 0u
        && stored->basic_blocks[0].terminator.data.call.callee.substitutions
            == NULL);

    expression = mutable_expression(hir, fixture.maximum);
    saved = *expression;
    expression->data.integer.high_bits = 1u;
    assert_call_tree_failure(&mir, hir, fixture.binary_caller_body,
        CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE);
    *expression = saved;

    expression = mutable_expression(hir, fixture.first_inner);
    saved = *expression;
    expression->owner_body = foreign_body;
    assert_call_tree_failure(&mir, hir, fixture.binary_caller_body,
        CM_MIR_LOWER_INVALID_HIR);
    *expression = saved;

    expression = mutable_expression(hir, fixture.second_inner);
    saved = *expression;
    expression->kind = CM_HIR_EXPR_BLOCK;
    expression->data.block.tail_expression = CM_HIR_EXPR_NONE;
    assert_call_tree_failure(&mir, hir, fixture.binary_caller_body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    *expression = saved;

    expression = mutable_expression(hir, fixture.binary_call);
    saved_argument = expression->data.call.arguments[1];
    expression->data.call.arguments[1] = fixture.maximum;
    assert_call_tree_failure(&mir, hir, fixture.binary_caller_body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    expression->data.call.arguments[1] = saved_argument;

    result = cm_mir_lower_instance(&mir, hir,
        fixture.binary_caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 4u && cm_mir_body_count(&mir) == 4u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 7u
        && stored->locals[3].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[4].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[5].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[6].kind == CM_MIR_LOCAL_TEMPORARY
        && stored->locals[3].type == u32_type
        && stored->locals[4].type == u32_type
        && stored->locals[5].type == u32_type
        && stored->locals[6].type == u32_type
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 4u
        && stored->basic_blocks[1].statement_count == 0u
        && stored->basic_blocks[1].statements == NULL
        && stored->basic_blocks[1].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    statements = stored->basic_blocks[0].statements;
    assert(statements != NULL
        && statements[0].data.assign.destination == 3u
        && statements[0].data.assign.value.data.binary.left.kind
            == CM_MIR_CONSTANT_U32
        && statements[0].data.assign.value.data.binary.left.data.u32_value
            == 1u
        && statements[0].data.assign.value.data.binary.right.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.right.data.local
            == 2u
        && statements[1].data.assign.destination == 4u
        && statements[1].data.assign.value.data.binary.left.data.local
            == 1u
        && statements[1].data.assign.value.data.binary.right.data.local
            == 3u);
    assert(statements[2].data.assign.destination == 5u
        && statements[2].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[2].data.assign.value.data.binary.left.data.local
            == 2u
        && statements[2].data.assign.value.data.binary.right.kind
            == CM_MIR_CONSTANT_U32
        && statements[2].data.assign.value.data.binary.right.data.u32_value
            == UINT32_MAX
        && statements[3].data.assign.destination == 6u
        && statements[3].data.assign.value.data.binary.left.data.local
            == 5u
        && statements[3].data.assign.value.data.binary.right.data.local
            == 1u);
    terminator = &stored->basic_blocks[0].terminator;
    assert(terminator->kind == CM_MIR_TERMINATOR_CALL
        && terminator->data.call.argument_count == 2u
        && terminator->data.call.arguments != NULL
        && terminator->data.call.arguments[0].kind == CM_MIR_OPERAND_MOVE
        && terminator->data.call.arguments[0].type == u32_type
        && terminator->data.call.arguments[0].data.local == 4u
        && terminator->data.call.arguments[1].kind == CM_MIR_OPERAND_MOVE
        && terminator->data.call.arguments[1].type == u32_type
        && terminator->data.call.arguments[1].data.local == 6u
        && terminator->data.call.callee_instance == 2u
        && terminator->data.call.callee.substitution_count == 0u
        && terminator->data.call.callee.substitutions == NULL
        && terminator->data.call.target == 1u);

    expression = mutable_expression(hir, fixture.nested_outer_call);
    saved_argument = expression->data.call.arguments[1];
    expression->data.call.arguments[1] = fixture.nested_three;
    assert_call_tree_failure(&mir, hir, fixture.nested_caller_body,
        CM_MIR_LOWER_UNSUPPORTED_EXPRESSION);
    expression->data.call.arguments[1] = saved_argument;

    expression = mutable_expression(hir, fixture.nested_inner_call);
    saved_callee = expression->data.call.callee;
    expression->data.call.callee = fixture.unary_definition;
    assert_call_tree_failure(&mir, hir, fixture.nested_caller_body,
        CM_MIR_LOWER_INVALID_HIR);
    expression->data.call.callee = saved_callee;

    expression = mutable_expression(hir, fixture.nested_outer_call);
    saved_argument = expression->data.call.arguments[0];
    expression->data.call.arguments[0] = fixture.nested_outer_call;
    assert_call_tree_failure(&mir, hir, fixture.nested_caller_body,
        CM_MIR_LOWER_INVALID_HIR);
    expression->data.call.arguments[0] = saved_argument;

    result = cm_mir_lower_instance(&mir, hir,
        fixture.nested_caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 5u && cm_mir_body_count(&mir) == 5u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 7u
        && stored->basic_block_count == 3u
        && stored->basic_blocks[0].statement_count == 2u
        && stored->basic_blocks[1].statement_count == 1u
        && stored->basic_blocks[2].statement_count == 0u
        && stored->basic_blocks[2].statements == NULL
        && stored->basic_blocks[2].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    statements = stored->basic_blocks[0].statements;
    assert(statements != NULL
        && statements[0].data.assign.destination == 3u
        && statements[0].data.assign.value.data.binary.left.kind
            == CM_MIR_OPERAND_MOVE
        && statements[0].data.assign.value.data.binary.left.data.local
            == 1u
        && statements[0].data.assign.value.data.binary.right.kind
            == CM_MIR_CONSTANT_U32
        && statements[0].data.assign.value.data.binary.right.data.u32_value
            == 1u
        && statements[1].data.assign.destination == 4u
        && statements[1].data.assign.value.data.binary.left.data.local
            == 2u
        && statements[1].data.assign.value.data.binary.right.data.u32_value
            == 2u);
    terminator = &stored->basic_blocks[0].terminator;
    assert(terminator->kind == CM_MIR_TERMINATOR_CALL
        && terminator->data.call.destination == 5u
        && terminator->data.call.argument_count == 2u
        && terminator->data.call.arguments[0].data.local == 3u
        && terminator->data.call.arguments[1].data.local == 4u
        && terminator->data.call.callee_instance == 2u
        && terminator->data.call.target == 1u);
    statements = stored->basic_blocks[1].statements;
    assert(statements != NULL
        && statements[0].data.assign.destination == 6u
        && statements[0].data.assign.value.data.binary.left.data.local
            == 1u
        && statements[0].data.assign.value.data.binary.right.kind
            == CM_MIR_CONSTANT_U32
        && statements[0].data.assign.value.data.binary.right.data.u32_value
            == 3u);
    terminator = &stored->basic_blocks[1].terminator;
    assert(terminator->kind == CM_MIR_TERMINATOR_CALL
        && terminator->data.call.destination == CM_MIR_RETURN_LOCAL
        && terminator->data.call.argument_count == 2u
        && terminator->data.call.arguments[0].data.local == 5u
        && terminator->data.call.arguments[1].data.local == 6u
        && terminator->data.call.callee_instance == 2u
        && terminator->data.call.target == 2u);
    cm_mir_context_destroy(&mir);

    hir_body = mutable_body(hir, fixture.nested_caller_body);
    saved_root = hir_body->root_expression;
    hir_body->root_expression = fixture.add_after_call_root;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, hir, fixture.binary_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    result = cm_mir_lower_instance(&mir, hir,
        fixture.nested_caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 2u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 7u
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 2u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && stored->basic_blocks[0].terminator.data.call.destination == 5u
        && stored->basic_blocks[0].terminator.data.call.target == 1u
        && stored->basic_blocks[1].statement_count == 2u
        && stored->basic_blocks[1].statements[0].data.assign.destination
            == 6u
        && stored->basic_blocks[1].statements[1].data.assign.destination
            == CM_MIR_RETURN_LOCAL
        && stored->basic_blocks[1].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    cm_mir_context_destroy(&mir);
    hir_body->root_expression = saved_root;

    cm_mir_context_init(&oom_mir);
    result = cm_mir_lower_instance(&oom_mir, hir, fixture.binary_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u
        && cm_mir_body_count(&oom_mir) == 1u);
    cm_alloc_set_oom_handler(jump_on_lower_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(lower_oom_jump) == 0) {
        (void)cm_mir_lower_instance(&oom_mir, hir,
            fixture.nested_caller_body, NULL, 0u);
        assert(0 && "nested call lowering unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(cm_mir_body_count(&oom_mir) == 1u
        && oom_mir.hir_owner == hir);
    cm_mir_context_destroy(&oom_mir);
}

typedef struct AggregateLowerFixture {
    CmHirDefId inner_definition;
    CmHirTypeId inner_type;
    CmHirDefId outer_definition;
    CmHirTypeId outer_type;
    CmHirDefId pair_definition;
    CmHirTypeId pair_type;
    CmHirBodyId nested_body;
    CmHirExprId nested_inner;
    CmHirExprId nested_outer;
    CmHirBodyId parameter_field_body;
    CmHirExprId parameter_field;
    CmHirBodyId fresh_field_body;
    CmHirExprId fresh_aggregate;
    CmHirExprId fresh_field;
    CmHirBodyId aggregate_field_body;
    CmHirExprId aggregate_field;
} AggregateLowerFixture;

static CmHirTypeId add_named_struct_type(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module, const char *name,
    const char *first_name, CmHirTypeId first_type,
    const char *second_name, CmHirTypeId second_type, uint32_t start,
    CmHirDefId *out_definition)
{
    CmHirField fields[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId type_id;
    uint32_t field_count;

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(start, start + 20u), out_definition)
        == CM_HIR_OK);
    memset(fields, 0, sizeof(fields));
    fields[0].name = cm_hir_intern(hir, first_name);
    fields[0].type = first_type;
    fields[0].visibility.kind = CM_HIR_VIS_PRIVATE;
    fields[0].visibility.restriction = cm_hir_def_id_none();
    fields[0].span = test_span(start + 5u, start + 9u);
    field_count = 1u;
    if (second_name != NULL) {
        fields[1].name = cm_hir_intern(hir, second_name);
        fields[1].type = second_type;
        fields[1].visibility.kind = CM_HIR_VIS_PRIVATE;
        fields[1].visibility.restriction = cm_hir_def_id_none();
        fields[1].span = test_span(start + 11u, start + 16u);
        field_count = 2u;
    }
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = *out_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(hir, name);
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 20u);
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = fields;
    item.data.aggregate_item.field_count = field_count;
    assert(cm_hir_add_item(hir, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(start + 20u, start + 22u);
    type.data.named_type.definition = *out_definition;
    assert(cm_hir_add_type(hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static void add_aggregate_lower_fixture(CmHirContext *hir,
    CmHirCrateId crate_id, CmHirModuleId root_module,
    CmHirTypeId u32_type, AggregateLowerFixture *fixture)
{
    CmHirDefId function_definition;
    CmHirExpr expression;
    CmHirExprId value;
    CmHirExprId tail;
    CmHirAggregateFieldValue fields[2];

    memset(fixture, 0, sizeof(*fixture));
    fixture->inner_type = add_named_struct_type(hir, crate_id, root_module,
        "Inner", "value", u32_type, NULL, CM_HIR_TYPE_NONE, 10u,
        &fixture->inner_definition);
    fixture->outer_type = add_named_struct_type(hir, crate_id, root_module,
        "Outer", "inner", fixture->inner_type, "tail", u32_type, 40u,
        &fixture->outer_definition);
    fixture->pair_type = add_named_struct_type(hir, crate_id, root_module,
        "Pair", "first", u32_type, "second", u32_type, 70u,
        &fixture->pair_definition);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(100u, 130u),
        &function_definition) == CM_HIR_OK);
    add_one_argument_function(hir, root_module, function_definition,
        "make_nested", u32_type, fixture->outer_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 100u, &fixture->nested_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->nested_body;
    expression.type = u32_type;
    expression.span = test_span(122u, 123u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &value) == CM_HIR_OK);
    memset(fields, 0, sizeof(fields));
    fields[0].field_index = 0u;
    fields[0].value = value;
    fields[0].span = test_span(120u, 125u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.owner_body = fixture->nested_body;
    expression.type = fixture->inner_type;
    expression.span = test_span(119u, 126u);
    expression.data.aggregate.definition = fixture->inner_definition;
    expression.data.aggregate.fields = fields;
    expression.data.aggregate.field_count = 1u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_inner)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->nested_body;
    expression.type = u32_type;
    expression.span = test_span(113u, 114u);
    expression.data.integer.low_bits = 9u;
    assert(cm_hir_add_expr(hir, &expression, &tail) == CM_HIR_OK);
    memset(fields, 0, sizeof(fields));
    fields[0].field_index = 1u;
    fields[0].value = tail;
    fields[0].span = test_span(111u, 115u);
    fields[1].field_index = 0u;
    fields[1].value = fixture->nested_inner;
    fields[1].span = test_span(117u, 127u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.owner_body = fixture->nested_body;
    expression.type = fixture->outer_type;
    expression.span = test_span(109u, 128u);
    expression.data.aggregate.definition = fixture->outer_definition;
    expression.data.aggregate.fields = fields;
    expression.data.aggregate.field_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->nested_outer)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->nested_body,
        fixture->nested_outer) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(140u, 170u),
        &function_definition) == CM_HIR_OK);
    add_one_argument_function(hir, root_module, function_definition,
        "read_second", fixture->pair_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 140u,
        &fixture->parameter_field_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->parameter_field_body;
    expression.type = fixture->pair_type;
    expression.span = test_span(151u, 155u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &value) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = fixture->parameter_field_body;
    expression.type = u32_type;
    expression.span = test_span(151u, 162u);
    expression.data.field.base = value;
    expression.data.field.definition = fixture->pair_definition;
    expression.data.field.field_index = 1u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->parameter_field)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir,
        fixture->parameter_field_body, fixture->parameter_field)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(180u, 210u),
        &function_definition) == CM_HIR_OK);
    add_one_argument_function(hir, root_module, function_definition,
        "fresh_first", u32_type, u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u,
        180u, &fixture->fresh_field_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->fresh_field_body;
    expression.type = u32_type;
    expression.span = test_span(192u, 193u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &value) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = fixture->fresh_field_body;
    expression.type = u32_type;
    expression.span = test_span(197u, 198u);
    expression.data.integer.low_bits = 7u;
    assert(cm_hir_add_expr(hir, &expression, &tail) == CM_HIR_OK);
    memset(fields, 0, sizeof(fields));
    fields[0].field_index = 1u;
    fields[0].value = value;
    fields[0].span = test_span(190u, 194u);
    fields[1].field_index = 0u;
    fields[1].value = tail;
    fields[1].span = test_span(195u, 199u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.owner_body = fixture->fresh_field_body;
    expression.type = fixture->pair_type;
    expression.span = test_span(189u, 200u);
    expression.data.aggregate.definition = fixture->pair_definition;
    expression.data.aggregate.fields = fields;
    expression.data.aggregate.field_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->fresh_aggregate)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = fixture->fresh_field_body;
    expression.type = u32_type;
    expression.span = test_span(189u, 206u);
    expression.data.field.base = fixture->fresh_aggregate;
    expression.data.field.definition = fixture->pair_definition;
    expression.data.field.field_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->fresh_field)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir, fixture->fresh_field_body,
        fixture->fresh_field) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(210u, 240u),
        &function_definition) == CM_HIR_OK);
    add_one_argument_function(hir, root_module, function_definition,
        "take_inner", fixture->outer_type, fixture->inner_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 210u,
        &fixture->aggregate_field_body);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = fixture->aggregate_field_body;
    expression.type = fixture->outer_type;
    expression.span = test_span(221u, 225u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &value) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = fixture->aggregate_field_body;
    expression.type = fixture->inner_type;
    expression.span = test_span(221u, 232u);
    expression.data.field.base = value;
    expression.data.field.definition = fixture->outer_definition;
    expression.data.field.field_index = 0u;
    assert(cm_hir_add_expr(hir, &expression, &fixture->aggregate_field)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(hir,
        fixture->aggregate_field_body, fixture->aggregate_field)
        == CM_HIR_OK);
}

static void test_aggregate_and_field_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    AggregateLowerFixture fixture;
    CmMirContext mir;
    CmMirContext rejected;
    CmMirLowerResult result;
    const CmMirBody *body;
    const CmMirStatement *statements;
    const CmMirRvalue *aggregate;
    const CmMirOperand *operand;
    const CmMirPlace *place;
    CmHirExpr *expression;
    CmHirDefId saved_definition;
    uint32_t saved_field_index;
    size_t body_count;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_aggregate_lower"), CM_HIR_EDITION_2021,
        test_span(0u, 240u), &crate_id, &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    add_aggregate_lower_fixture(&hir, crate_id, root_module, u32_type,
        &fixture);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, fixture.nested_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    body = cm_mir_get_body(&mir, result.body);
    assert(body != NULL && body->local_count == 3u
        && body->locals[0].kind == CM_MIR_LOCAL_RETURN
        && body->locals[0].type == fixture.outer_type
        && body->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && body->locals[1].type == u32_type
        && body->locals[2].kind == CM_MIR_LOCAL_TEMPORARY
        && body->locals[2].type == fixture.inner_type
        && body->basic_block_count == 1u
        && body->basic_blocks[0].statement_count == 2u);
    statements = body->basic_blocks[0].statements;
    aggregate = statements == NULL ? NULL : &statements[0].data.assign.value;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 2u
        && aggregate->kind == CM_MIR_RVALUE_AGGREGATE
        && aggregate->type == fixture.inner_type
        && aggregate->span.source == 1u
        && aggregate->span.start == 119u && aggregate->span.end == 126u
        && cm_hir_def_id_equal(aggregate->data.aggregate.definition,
            fixture.inner_definition)
        && aggregate->data.aggregate.field_count == 1u
        && aggregate->data.aggregate.fields[0].field_index == 0u
        && aggregate->data.aggregate.fields[0].value.kind
            == CM_MIR_OPERAND_MOVE
        && aggregate->data.aggregate.fields[0].value.type == u32_type
        && aggregate->data.aggregate.fields[0].value.data.local == 1u);
    aggregate = &statements[1].data.assign.value;
    assert(statements[1].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[1].data.assign.destination == CM_MIR_RETURN_LOCAL
        && aggregate->kind == CM_MIR_RVALUE_AGGREGATE
        && aggregate->type == fixture.outer_type
        && aggregate->span.source == 1u
        && aggregate->span.start == 109u && aggregate->span.end == 128u
        && cm_hir_def_id_equal(aggregate->data.aggregate.definition,
            fixture.outer_definition)
        && aggregate->data.aggregate.field_count == 2u
        && aggregate->data.aggregate.fields[0].field_index == 0u
        && aggregate->data.aggregate.fields[0].value.kind
            == CM_MIR_OPERAND_MOVE
        && aggregate->data.aggregate.fields[0].value.type
            == fixture.inner_type
        && aggregate->data.aggregate.fields[0].value.data.local == 2u
        && aggregate->data.aggregate.fields[1].field_index == 1u
        && aggregate->data.aggregate.fields[1].value.kind
            == CM_MIR_CONSTANT_U32
        && aggregate->data.aggregate.fields[1].value.type == u32_type
        && aggregate->data.aggregate.fields[1].value.data.u32_value == 9u);

    result = cm_mir_lower_instance(&mir, &hir,
        fixture.parameter_field_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 2u);
    body = cm_mir_get_body(&mir, result.body);
    assert(body != NULL && body->local_count == 2u
        && body->basic_block_count == 1u
        && body->basic_blocks[0].statement_count == 1u);
    statements = body->basic_blocks[0].statements;
    operand = statements == NULL ? NULL
        : &statements[0].data.assign.value.data.use;
    place = operand == NULL ? NULL : &operand->data.place;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_USE
        && operand->kind == CM_MIR_OPERAND_COPY_PLACE
        && operand->type == u32_type
        && place->base == 1u && place->type == u32_type
        && place->projection_count == 1u && place->projections != NULL
        && cm_hir_def_id_equal(place->projections[0].definition,
            fixture.pair_definition)
        && place->projections[0].field_index == 1u
        && place->span.source == 1u
        && place->span.start == 151u && place->span.end == 162u);
    assert(cm_mir_validate_place(&hir, body, place) == CM_MIR_OK);

    result = cm_mir_lower_instance(&mir, &hir, fixture.fresh_field_body,
        NULL, 0u);
    assert(result.error_count == 0u && result.body == 3u);
    body = cm_mir_get_body(&mir, result.body);
    assert(body != NULL && body->local_count == 3u
        && body->locals[2].kind == CM_MIR_LOCAL_TEMPORARY
        && body->locals[2].type == fixture.pair_type
        && body->basic_blocks[0].statement_count == 2u);
    statements = body->basic_blocks[0].statements;
    aggregate = statements == NULL ? NULL : &statements[0].data.assign.value;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 2u
        && aggregate->kind == CM_MIR_RVALUE_AGGREGATE
        && aggregate->type == fixture.pair_type
        && aggregate->span.source == 1u
        && aggregate->span.start == 189u && aggregate->span.end == 200u
        && cm_hir_def_id_equal(aggregate->data.aggregate.definition,
            fixture.pair_definition)
        && aggregate->data.aggregate.field_count == 2u
        && aggregate->data.aggregate.fields[0].field_index == 0u
        && aggregate->data.aggregate.fields[0].value.kind
            == CM_MIR_CONSTANT_U32
        && aggregate->data.aggregate.fields[0].value.data.u32_value == 7u
        && aggregate->data.aggregate.fields[1].field_index == 1u
        && aggregate->data.aggregate.fields[1].value.kind
            == CM_MIR_OPERAND_MOVE
        && aggregate->data.aggregate.fields[1].value.data.local == 1u);
    operand = &statements[1].data.assign.value.data.use;
    place = &operand->data.place;
    assert(statements[1].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[1].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[1].data.assign.value.kind == CM_MIR_RVALUE_USE
        && operand->kind == CM_MIR_OPERAND_COPY_PLACE
        && operand->type == u32_type
        && place->base == 2u && place->type == u32_type
        && place->projection_count == 1u
        && cm_hir_def_id_equal(place->projections[0].definition,
            fixture.pair_definition)
        && place->projections[0].field_index == 0u
        && place->span.source == 1u
        && place->span.start == 189u && place->span.end == 206u);
    assert(cm_mir_validate_place(&hir, body, place) == CM_MIR_OK);

    result = cm_mir_lower_instance(&mir, &hir,
        fixture.aggregate_field_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 4u);
    body = cm_mir_get_body(&mir, result.body);
    assert(body != NULL && body->local_count == 2u
        && body->locals[0].type == fixture.inner_type
        && body->locals[1].type == fixture.outer_type
        && body->basic_block_count == 1u
        && body->basic_blocks[0].statement_count == 1u);
    statements = body->basic_blocks[0].statements;
    operand = statements == NULL ? NULL
        : &statements[0].data.assign.value.data.use;
    place = operand == NULL ? NULL : &operand->data.place;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_USE
        && operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        && operand->type == fixture.inner_type
        && place->base == 1u && place->type == fixture.inner_type
        && place->projection_count == 1u && place->projections != NULL
        && cm_hir_def_id_equal(place->projections[0].definition,
            fixture.outer_definition)
        && place->projections[0].field_index == 0u
        && place->span.source == 1u
        && place->span.start == 221u && place->span.end == 232u);
    assert(cm_mir_validate_place(&hir, body, place) == CM_MIR_OK);

    cm_mir_context_init(&rejected);
    result = cm_mir_lower_instance(&rejected, &hir,
        fixture.parameter_field_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    body_count = cm_mir_body_count(&rejected);
    expression = mutable_expression(&hir, fixture.nested_outer);
    saved_field_index = expression->data.aggregate.fields[0].field_index;
    expression->data.aggregate.fields[0].field_index = 0u;
    result = cm_mir_lower_instance(&rejected, &hir, fixture.nested_body,
        NULL, 0u);
    assert(result.error_count == 1u && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_INVALID_HIR
        && cm_mir_body_count(&rejected) == body_count);
    expression->data.aggregate.fields[0].field_index = saved_field_index;
    expression = mutable_expression(&hir, fixture.fresh_field);
    saved_definition = expression->data.field.definition;
    expression->data.field.definition = fixture.outer_definition;
    result = cm_mir_lower_instance(&rejected, &hir,
        fixture.fresh_field_body, NULL, 0u);
    assert(result.error_count == 1u && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_INVALID_HIR
        && cm_mir_body_count(&rejected) == body_count);
    expression->data.field.definition = saved_definition;

    cm_mir_context_destroy(&rejected);
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static CmHirExprId add_test_local_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirTypeId type, uint32_t local_index,
    uint32_t start, uint32_t end)
{
    CmHirExpr expression;
    CmHirExprId id;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body;
    expression.type = type;
    expression.span = test_span(start, end);
    expression.data.local.local_index = local_index;
    assert(cm_hir_add_expr(hir, &expression, &id) == CM_HIR_OK);
    return id;
}

static CmHirExprId add_test_integer_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirTypeId type, uint32_t value,
    uint32_t start, uint32_t end)
{
    CmHirExpr expression;
    CmHirExprId id;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = body;
    expression.type = type;
    expression.span = test_span(start, end);
    expression.data.integer.low_bits = value;
    assert(cm_hir_add_expr(hir, &expression, &id) == CM_HIR_OK);
    return id;
}

static CmHirExprId add_pair_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirTypeId pair_type, CmHirDefId pair_definition,
    CmHirExprId first, CmHirExprId second, uint32_t start)
{
    CmHirAggregateFieldValue fields[2];
    CmHirExpr expression;
    CmHirExprId id;

    memset(fields, 0, sizeof(fields));
    fields[0].field_index = 0u;
    fields[0].value = first;
    fields[0].span = test_span(start + 2u, start + 8u);
    fields[1].field_index = 1u;
    fields[1].value = second;
    fields[1].span = test_span(start + 9u, start + 15u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_AGGREGATE;
    expression.owner_body = body;
    expression.type = pair_type;
    expression.span = test_span(start, start + 17u);
    expression.data.aggregate.definition = pair_definition;
    expression.data.aggregate.fields = fields;
    expression.data.aggregate.field_count = 2u;
    assert(cm_hir_add_expr(hir, &expression, &id) == CM_HIR_OK);
    return id;
}

static CmHirExprId add_test_call_expression(CmHirContext *hir,
    CmHirBodyId body, CmHirTypeId result_type, CmHirDefId callee,
    const CmHirExprId *arguments, uint32_t argument_count,
    uint32_t start, uint32_t end)
{
    CmHirExpr expression;
    CmHirExprId id;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CALL;
    expression.owner_body = body;
    expression.type = result_type;
    expression.span = test_span(start, end);
    expression.data.call.callee = callee;
    expression.data.call.arguments = (CmHirExprId *)arguments;
    expression.data.call.argument_count = argument_count;
    assert(cm_hir_add_expr(hir, &expression, &id) == CM_HIR_OK);
    return id;
}

static void test_aggregate_call_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirTypeId pair_type;
    CmHirTypeId wrapper_type;
    CmHirDefId pair_definition;
    CmHirDefId wrapper_definition;
    CmHirDefId select_definition;
    CmHirDefId mixed_definition;
    CmHirDefId caller_definition;
    CmHirDefId mixed_caller_definition;
    CmHirBodyId select_body;
    CmHirBodyId mixed_body;
    CmHirBodyId caller_body;
    CmHirBodyId mixed_caller_body;
    CmHirExprId aggregate;
    CmHirExprId arguments[2];
    CmHirExprId base;
    CmHirExprId field;
    CmHirExprId offset;
    CmHirExprId root;
    CmHirExpr expression;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirOperand *stored_argument;
    CmHirTypeId saved_type;
    CmMirOperandKind saved_kind;
    uint32_t saved_field_index;
    size_t count;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_aggregate_call"), CM_HIR_EDITION_2021,
        test_span(0u, 500u), &crate_id, &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    pair_type = add_named_struct_type(&hir, crate_id, root_module, "Pair",
        "first", u32_type, "second", u32_type, 10u,
        &pair_definition);
    wrapper_type = add_named_struct_type(&hir, crate_id, root_module,
        "Wrapper", "pair", pair_type, "tag", u32_type, 32u,
        &wrapper_definition);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(50u, 90u), &select_definition)
        == CM_HIR_OK);
    add_one_argument_function(&hir, root_module, select_definition,
        "select", pair_type, u32_type, CM_HIR_GENERIC_PARAM_NONE, 0u,
        50u, &select_body);
    base = add_test_local_expression(&hir, select_body, pair_type, 0u,
        60u, 65u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = select_body;
    expression.type = u32_type;
    expression.span = test_span(60u, 75u);
    expression.data.field.base = base;
    expression.data.field.definition = pair_definition;
    expression.data.field.field_index = 1u;
    assert(cm_hir_add_expr(&hir, &expression, &field) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&hir, select_body, field)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(100u, 150u), &mixed_definition)
        == CM_HIR_OK);
    add_mixed_argument_function(&hir, root_module, mixed_definition,
        "select_offset", pair_type, u32_type, u32_type, 100u,
        &mixed_body);
    base = add_test_local_expression(&hir, mixed_body, pair_type, 0u,
        120u, 125u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = mixed_body;
    expression.type = u32_type;
    expression.span = test_span(120u, 132u);
    expression.data.field.base = base;
    expression.data.field.definition = pair_definition;
    expression.data.field.field_index = 1u;
    assert(cm_hir_add_expr(&hir, &expression, &field) == CM_HIR_OK);
    offset = add_test_local_expression(&hir, mixed_body, u32_type, 1u,
        136u, 142u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = mixed_body;
    expression.type = u32_type;
    expression.span = test_span(120u, 142u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = field;
    expression.data.binary.right = offset;
    assert(cm_hir_add_expr(&hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&hir, mixed_body, root)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(170u, 230u), &caller_definition)
        == CM_HIR_OK);
    add_one_argument_function(&hir, root_module, caller_definition,
        "call_select", wrapper_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u,
        170u, &caller_body);
    base = add_test_local_expression(&hir, caller_body, wrapper_type, 0u,
        184u, 190u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = caller_body;
    expression.type = pair_type;
    expression.span = test_span(184u, 195u);
    expression.data.field.base = base;
    expression.data.field.definition = wrapper_definition;
    expression.data.field.field_index = 0u;
    assert(cm_hir_add_expr(&hir, &expression, &field) == CM_HIR_OK);
    arguments[0] = field;
    root = add_test_call_expression(&hir, caller_body, u32_type,
        select_definition, arguments, 1u, 180u, 199u);
    assert(cm_hir_set_body_root_expression(&hir, caller_body, root)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(240u, 310u),
        &mixed_caller_definition) == CM_HIR_OK);
    add_one_argument_function(&hir, root_module, mixed_caller_definition,
        "call_select_offset", u32_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 240u, &mixed_caller_body);
    arguments[0] = add_test_local_expression(&hir, mixed_caller_body,
        u32_type, 0u, 254u, 256u);
    arguments[1] = add_test_integer_expression(&hir, mixed_caller_body,
        u32_type, 11u, 261u, 263u);
    aggregate = add_pair_expression(&hir, mixed_caller_body, pair_type,
        pair_definition, arguments[0], arguments[1], 251u);
    arguments[0] = aggregate;
    arguments[1] = add_test_local_expression(&hir, mixed_caller_body,
        u32_type, 0u, 268u, 270u);
    root = add_test_call_expression(&hir, mixed_caller_body, u32_type,
        mixed_definition, arguments, 2u, 250u, 270u);
    assert(cm_hir_set_body_root_expression(&hir, mixed_caller_body, root)
        == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, select_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 1u);
    result = cm_mir_lower_instance(&mir, &hir, mixed_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 2u);
    result = cm_mir_lower_instance(&mir, &hir, caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 3u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->local_count == 2u
        && stored->basic_block_count == 2u
        && stored->basic_blocks[0].statement_count == 0u
        && stored->basic_blocks[0].statements == NULL
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && stored->basic_blocks[0].terminator.data.call.argument_count == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments != NULL
        && stored->basic_blocks[0].terminator.data.call.arguments[0].kind
            == CM_MIR_OPERAND_MOVE_PLACE
        && stored->basic_blocks[0].terminator.data.call.arguments[0].type
            == pair_type
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .place.base == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .place.projection_count == 1u
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .place.projections != NULL
        && cm_hir_def_id_equal(
            stored->basic_blocks[0].terminator.data.call.arguments[0].data
                .place.projections[0].definition,
            wrapper_definition)
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .place.projections[0].field_index == 0u
        && stored->basic_blocks[0].terminator.data.call.destination
            == CM_MIR_RETURN_LOCAL
        && stored->basic_blocks[1].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);

    mutable_stored = (CmMirBody *)stored;
    stored_argument = &mutable_stored->basic_blocks[0].terminator.data.call
        .arguments[0];
    saved_type = stored_argument->type;
    stored_argument->type = u32_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    stored_argument->type = saved_type;
    saved_kind = stored_argument->kind;
    stored_argument->kind = CM_MIR_OPERAND_COPY_PLACE;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    stored_argument->kind = saved_kind;
    saved_field_index = stored_argument->data.place.projections[0]
        .field_index;
    stored_argument->data.place.projections[0].field_index = 1u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    stored_argument->data.place.projections[0].field_index =
        saved_field_index;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);

    result = cm_mir_lower_instance(&mir, &hir, mixed_caller_body, NULL, 0u);
    assert(result.error_count == 0u && result.body == 4u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->basic_block_count == 2u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_CALL
        && stored->basic_blocks[0].terminator.data.call.argument_count == 2u
        && stored->basic_blocks[0].terminator.data.call.arguments[0].kind
            == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].terminator.data.call.arguments[0].type
            == pair_type
        && stored->basic_blocks[0].terminator.data.call.arguments[0].data
            .local == 2u
        && stored->basic_blocks[0].terminator.data.call.arguments[1].kind
            == CM_MIR_OPERAND_MOVE
        && stored->basic_blocks[0].terminator.data.call.arguments[1].type
            == u32_type
        && stored->basic_blocks[0].terminator.data.call.arguments[1].data
            .local == 1u);
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);

    count = cm_mir_body_count(&mir);
    mutable_stored = (CmMirBody *)cm_mir_get_body(&mir, 2u);
    assert(mutable_stored != NULL);
    saved_type = mutable_stored->locals[CM_MIR_RETURN_LOCAL].type;
    mutable_stored->locals[CM_MIR_RETURN_LOCAL].type = pair_type;
    result = cm_mir_lower_instance(&mir, &hir, mixed_caller_body, NULL, 0u);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_UNSUPPORTED_EXPRESSION
        && cm_mir_body_count(&mir) == count);
    mutable_stored->locals[CM_MIR_RETURN_LOCAL].type = saved_type;

    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

/*
 * Explicit reference nodes are valid typed HIR, but they are not yet an
 * authenticated MIR recipe.  In particular, a shared borrow needs a proven
 * source place/use and a dereference needs a proven built-in place step.
 * Keep both boundaries transactional until semantic admission publishes that
 * evidence and the exact HIR/MIR matcher can replay it.
 */
static void test_reference_lowering_stays_fail_closed(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirTypeId shared_u32_type;
    CmHirDefId borrow_definition;
    CmHirDefId roundtrip_definition;
    CmHirBodyId borrow_body;
    CmHirBodyId roundtrip_body;
    CmHirExpr expression;
    CmHirExprId local;
    CmHirExprId borrow;
    CmHirExprId explicit_borrow;
    CmHirExprId dereference;
    CmMirContext mir;
    CmMirLowerResult result;
    size_t count;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_reference_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 200u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    shared_u32_type = add_shared_reference_type(&hir, u32_type, 3u);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(40u, 80u), &borrow_definition)
        == CM_HIR_OK);
    add_one_argument_function(&hir, root_module, borrow_definition,
        "borrow", u32_type, shared_u32_type, CM_HIR_GENERIC_PARAM_NONE,
        0u, 40u, &borrow_body);
    local = add_test_local_expression(&hir, borrow_body, u32_type, 0u,
        60u, 64u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BORROW_SHARED;
    expression.owner_body = borrow_body;
    expression.type = shared_u32_type;
    expression.span = test_span(59u, 64u);
    expression.data.borrow_shared.operand = local;
    assert(cm_hir_add_expr(&hir, &expression, &borrow) == CM_HIR_OK);
    explicit_borrow = borrow;
    assert(cm_hir_set_body_root_expression(&hir, borrow_body, borrow)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(90u, 130u),
        &roundtrip_definition) == CM_HIR_OK);
    add_one_argument_function(&hir, root_module, roundtrip_definition,
        "borrow_then_dereference", u32_type, u32_type,
        CM_HIR_GENERIC_PARAM_NONE, 0u, 90u, &roundtrip_body);
    local = add_test_local_expression(&hir, roundtrip_body, u32_type, 0u,
        110u, 114u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BORROW_SHARED;
    expression.owner_body = roundtrip_body;
    expression.type = shared_u32_type;
    expression.span = test_span(109u, 114u);
    expression.data.borrow_shared.operand = local;
    assert(cm_hir_add_expr(&hir, &expression, &borrow) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_DEREFERENCE;
    expression.owner_body = roundtrip_body;
    expression.type = u32_type;
    expression.span = test_span(108u, 114u);
    expression.data.dereference.operand = borrow;
    assert(cm_hir_add_expr(&hir, &expression, &dereference) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&hir, roundtrip_body,
        dereference) == CM_HIR_OK);

    cm_mir_context_init(&mir);
    count = cm_mir_body_count(&mir);
    result = cm_mir_lower_instance(&mir, &hir, borrow_body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_UNSUPPORTED_EXPRESSION
        && result.first_error.hir_body == borrow_body
        && result.first_error.hir_expression == explicit_borrow
        && cm_mir_body_count(&mir) == count);
    result = cm_mir_lower_instance(&mir, &hir, roundtrip_body, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE
        && result.first_error.kind == CM_MIR_LOWER_UNSUPPORTED_EXPRESSION
        && result.first_error.hir_body == roundtrip_body
        && result.first_error.hir_expression == dereference
        && cm_mir_body_count(&mir) == count);

    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static void test_discard_parameter_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirDefId definition;
    CmHirFunctionParameter parameter;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId root;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirLocalKind saved_kind;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_discard_parameter_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 80u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.type = u32_type;
    parameter.span = test_span(20u, 25u);
    parameter.binding_kind = CM_HIR_BINDING_DISCARD;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(10u, 80u);
    assert(cm_hir_add_body(&hir, &body, &body_id) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&hir, "discard");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = u32_type;
    item.data.function_item.signature.abi = cm_hir_intern(&hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
    root = add_test_integer_expression(&hir, body_id, u32_type, 7u,
        40u, 41u);
    assert(cm_hir_set_body_root_expression(&hir, body_id, root)
        == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, NULL, 0u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && stored != NULL && stored->local_count == 2u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[0].type == u32_type
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[1].type == u32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 1u
        && stored->basic_blocks[0].statements[0].kind
            == CM_MIR_STATEMENT_ASSIGN
        && stored->basic_blocks[0].statements[0].data.assign.destination
            == CM_MIR_RETURN_LOCAL
        && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
            == CM_MIR_OK);
    mutable_stored = (CmMirBody *)stored;
    saved_kind = mutable_stored->locals[1].kind;
    mutable_stored->locals[1].kind = CM_MIR_LOCAL_USER;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[1].kind = saved_kind;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static void test_tuple_parameter_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId i32_type;
    CmHirTypeId u32_type;
    CmHirTypeId tuple_type;
    CmHirTypeId reference_type;
    CmHirType tuple_value;
    CmHirType reference_value;
    CmHirTypeId tuple_elements[2];
    CmHirDefId definition;
    CmHirFunctionParameter parameter;
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExprId root;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirStatement *statements;
    CmMirPlaceProjection *first_projection;
    CmMirPlaceProjection *second_projection;
    CmMirStatement extra_statements[4];
    CmMirLocalKind saved_kind;
    CmHirTypeId saved_type;
    CmHirDefId saved_definition;
    uint32_t saved_destination;
    uint32_t saved_field_index;
    uint32_t saved_statement_count;
    CmMirStatement *saved_statements;
    CmHirBody *mutable_source_body;
    CmHirType *mutable_tuple_type;
    CmHirTypeId saved_element_type;
    uint32_t saved_element_count;
    uint32_t saved_binding_index;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirType generic_value;
    CmHirTypeId generic_type;
    CmHirItem *mutable_item;
    FILE *stream;
    char dump[2048];
    size_t length;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_tuple_parameter_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    i32_type = add_integer_type(&hir, CM_HIR_INT_I32, 2u);
    tuple_elements[0] = u32_type;
    tuple_elements[1] = i32_type;
    memset(&tuple_value, 0, sizeof(tuple_value));
    tuple_value.kind = CM_HIR_TYPE_TUPLE_KIND;
    tuple_value.span = test_span(3u, 8u);
    tuple_value.data.tuple_type.elements = tuple_elements;
    tuple_value.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&hir, &tuple_value, &tuple_type) == CM_HIR_OK);
    memset(&reference_value, 0, sizeof(reference_value));
    reference_value.kind = CM_HIR_TYPE_REFERENCE_KIND;
    reference_value.span = test_span(3u, 8u);
    reference_value.data.reference_type.pointee = u32_type;
    reference_value.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    reference_value.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&hir, &reference_value, &reference_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 80u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.type = tuple_type;
    parameter.span = test_span(20u, 40u);
    parameter.binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.tuple_bindings[0].name = cm_hir_intern(&hir, "left");
    parameter.tuple_bindings[0].span = test_span(21u, 25u);
    parameter.tuple_bindings[1].name = cm_hir_intern(&hir, "right");
    parameter.tuple_bindings[1].span = test_span(27u, 32u);
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameter.tuple_bindings[0].name;
    locals[0].type = u32_type;
    locals[0].span = parameter.tuple_bindings[0].span;
    locals[0].parameter_index = 0u;
    locals[0].parameter_binding_index = 0u;
    locals[1].name = parameter.tuple_bindings[1].name;
    locals[1].type = i32_type;
    locals[1].span = parameter.tuple_bindings[1].span;
    locals[1].parameter_index = 0u;
    locals[1].parameter_binding_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(10u, 80u);
    assert(cm_hir_add_body(&hir, &body, &body_id) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&hir, "first");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = u32_type;
    item.data.function_item.signature.abi = cm_hir_intern(&hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
    root = add_test_local_expression(&hir, body_id, u32_type, 0u,
        50u, 54u);
    assert(cm_hir_set_body_root_expression(&hir, body_id, root)
        == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, NULL, 0u);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 1u && cm_mir_body_count(&mir) == 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->owned_storage != NULL
        && stored->local_count == 4u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[0].type == u32_type
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[1].type == tuple_type
        && stored->locals[2].kind == CM_MIR_LOCAL_USER
        && stored->locals[2].type == u32_type
        && stored->locals[3].kind == CM_MIR_LOCAL_USER
        && stored->locals[3].type == i32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 3u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    statements = (CmMirStatement *)stored->basic_blocks[0].statements;
    assert(statements != NULL
        && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 2u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_USE
        && statements[0].data.assign.value.data.use.kind
            == CM_MIR_OPERAND_COPY_PLACE
        && statements[0].data.assign.value.data.use.data.place.base == 1u
        && statements[0].data.assign.value.data.use.data.place.type
            == u32_type
        && statements[0].data.assign.value.data.use.data.place
            .projection_count == 1u
        && statements[1].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[1].data.assign.destination == 3u
        && statements[1].data.assign.value.kind == CM_MIR_RVALUE_USE
        && statements[1].data.assign.value.data.use.kind
            == CM_MIR_OPERAND_COPY_PLACE
        && statements[1].data.assign.value.data.use.data.place.base == 1u
        && statements[1].data.assign.value.data.use.data.place.type
            == i32_type
        && statements[1].data.assign.value.data.use.data.place
            .projection_count == 1u
        && statements[2].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[2].data.assign.value.kind == CM_MIR_RVALUE_USE
        && statements[2].data.assign.value.data.use.kind
            == CM_MIR_OPERAND_MOVE
        && statements[2].data.assign.value.data.use.data.local == 2u);
    first_projection = statements[0].data.assign.value.data.use.data.place
        .projections;
    second_projection = statements[1].data.assign.value.data.use.data.place
        .projections;
    assert(first_projection != NULL && second_projection != NULL
        && first_projection->kind == CM_MIR_PROJECTION_FIELD
        && cm_hir_def_id_is_none(first_projection->definition)
        && first_projection->field_index == 0u
        && second_projection->kind == CM_MIR_PROJECTION_FIELD
        && cm_hir_def_id_is_none(second_projection->definition)
        && second_projection->field_index == 1u
        && cm_mir_validate_place(&hir, stored,
            &statements[0].data.assign.value.data.use.data.place)
                == CM_MIR_OK
        && cm_mir_validate_place(&hir, stored,
            &statements[1].data.assign.value.data.use.data.place)
                == CM_MIR_OK
        && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
                == CM_MIR_OK);

    stream = tmpfile();
    assert(stream != NULL && cm_mir_dump(stream, &mir) == 0
        && fflush(stream) == 0);
    rewind(stream);
    length = fread(dump, 1u, sizeof(dump) - 1u, stream);
    assert(!ferror(stream));
    dump[length] = '\0';
    assert(strstr(dump, ".tuple-field(#0)") != NULL
        && strstr(dump, ".tuple-field(#1)") != NULL);
    assert(fclose(stream) == 0);

    mutable_stored = (CmMirBody *)stored;
    saved_kind = mutable_stored->locals[1].kind;
    mutable_stored->locals[1].kind = CM_MIR_LOCAL_USER;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[1].kind = saved_kind;
    saved_type = mutable_stored->locals[1].type;
    mutable_stored->locals[1].type = u32_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[1].type = saved_type;

    saved_destination = statements[0].data.assign.destination;
    statements[0].data.assign.destination =
        statements[1].data.assign.destination;
    statements[1].data.assign.destination = saved_destination;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    saved_destination = statements[0].data.assign.destination;
    statements[0].data.assign.destination =
        statements[1].data.assign.destination;
    statements[1].data.assign.destination = saved_destination;

    saved_field_index = second_projection->field_index;
    second_projection->field_index = 0u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    second_projection->field_index = saved_field_index;
    saved_definition = first_projection->definition;
    first_projection->definition = definition;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    first_projection->definition = saved_definition;

    saved_statement_count = mutable_stored->basic_blocks[0].statement_count;
    mutable_stored->basic_blocks[0].statement_count = 2u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].statement_count = saved_statement_count;
    memcpy(extra_statements, statements, sizeof(*statements) * 3u);
    extra_statements[3] = statements[2];
    saved_statements = mutable_stored->basic_blocks[0].statements;
    mutable_stored->basic_blocks[0].statements = extra_statements;
    mutable_stored->basic_blocks[0].statement_count = 4u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->basic_blocks[0].statements = saved_statements;
    mutable_stored->basic_blocks[0].statement_count = saved_statement_count;

    saved_type = mutable_stored->locals[2].type;
    mutable_stored->locals[2].type = i32_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[2].type = saved_type;

    mutable_tuple_type = (CmHirType *)cm_hir_get_type(&hir, tuple_type);
    mutable_source_body = (CmHirBody *)cm_hir_get_body(&hir, body_id);
    assert(mutable_tuple_type != NULL && mutable_source_body != NULL);
    saved_element_count = mutable_tuple_type->data.tuple_type.element_count;
    mutable_tuple_type->data.tuple_type.element_count = 1u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_tuple_type->data.tuple_type.element_count = saved_element_count;
    saved_element_type = mutable_tuple_type->data.tuple_type.elements[1];
    mutable_tuple_type->data.tuple_type.elements[1] = tuple_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_tuple_type->data.tuple_type.elements[1] = reference_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_tuple_type->data.tuple_type.elements[1] = saved_element_type;
    saved_binding_index = mutable_source_body->locals[1]
        .parameter_binding_index;
    mutable_source_body->locals[1].parameter_binding_index = 0u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_source_body->locals[1].parameter_binding_index =
        saved_binding_index;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);
    cm_mir_context_destroy(&mir);

    mutable_tuple_type->data.tuple_type.elements[1] = reference_type;
    mutable_source_body->locals[1].type = reference_type;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, NULL, 0u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE);
    cm_mir_context_destroy(&mir);
    mutable_tuple_type->data.tuple_type.elements[1] = saved_element_type;
    mutable_source_body->locals[1].type = saved_element_type;

    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = definition;
    generic.name = cm_hir_intern(&hir, "T");
    generic.span = test_span(8u, 9u);
    assert(cm_hir_add_generic_param(&hir, &generic, &generic_id)
        == CM_HIR_OK);
    memset(&generic_value, 0, sizeof(generic_value));
    generic_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    generic_value.span = generic.span;
    generic_value.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&hir, &generic_value, &generic_type)
        == CM_HIR_OK);
    mutable_item = (CmHirItem *)cm_hir_get_item(&hir, item_id);
    assert(mutable_item != NULL);
    mutable_item->generic_parameter_start = generic_id;
    mutable_item->generic_parameter_count = 1u;
    mutable_tuple_type->data.tuple_type.elements[0] = generic_type;
    mutable_tuple_type->data.tuple_type.elements[1] = generic_type;
    mutable_source_body->locals[0].type = generic_type;
    mutable_source_body->locals[1].type = generic_type;
    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, &u32_type, 1u);
    assert(result.error_count == 1u && result.lowered_body_count == 0u
        && result.body == CM_MIR_BODY_NONE);
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static void test_unary_tuple_parameter_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirDefId impl_definition;
    CmHirDefId definition;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirType type;
    CmHirTypeId generic_type;
    CmHirTypeId self_type;
    CmHirTypeId tuple_element;
    CmHirTypeId tuple_type;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirExpr expression;
    CmHirExprId root;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirStatement *statements;
    CmMirOperand *operand;
    CmMirPlaceProjection *projection;
    CmHirItem *stored_item;
    CmMirOperandKind saved_operand_kind;
    CmHirTypeId saved_type;
    uint32_t saved_field_index;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_unary_tuple_parameter_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 120u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_IMPL, test_span(5u, 115u), &impl_definition)
        == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_IMPL;
    item.definition = impl_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(5u, 115u);
    item.data.impl_item.self_type = u32_type;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(8u, 12u);
    type.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&hir, &type, &self_type) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 110u), &definition)
        == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = definition;
    generic.name = cm_hir_intern(&hir, "T");
    generic.span = test_span(14u, 15u);
    assert(cm_hir_add_generic_param(&hir, &generic, &generic_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = generic.span;
    type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&hir, &type, &generic_type) == CM_HIR_OK);
    tuple_element = generic_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(30u, 34u);
    type.data.tuple_type.elements = &tuple_element;
    type.data.tuple_type.element_count = 1u;
    assert(cm_hir_add_type(&hir, &type, &tuple_type) == CM_HIR_OK);

    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&hir, "self");
    parameters[0].type = self_type;
    parameters[0].span = test_span(22u, 26u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[0].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameters[1].type = tuple_type;
    parameters[1].span = test_span(28u, 45u);
    parameters[1].binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    parameters[1].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameters[1].tuple_bindings[0].name = cm_hir_intern(&hir, "value");
    parameters[1].tuple_bindings[0].span = test_span(29u, 34u);
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameters[0].name;
    locals[0].type = self_type;
    locals[0].span = parameters[0].span;
    locals[0].parameter_index = 0u;
    locals[1].name = parameters[1].tuple_bindings[0].name;
    locals[1].type = generic_type;
    locals[1].span = parameters[1].tuple_bindings[0].span;
    locals[1].parameter_index = 1u;
    locals[1].parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = generic_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 10u;
    body.span = test_span(10u, 110u);
    assert(cm_hir_add_body(&hir, &body, &body_id) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = impl_definition;
    item.name = cm_hir_intern(&hir, "call_once");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = generic_id;
    item.generic_parameter_count = 1u;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = generic_type;
    item.data.function_item.signature.abi = cm_hir_intern(&hir, "rust-call");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = generic_type;
    expression.span = test_span(70u, 75u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&hir, body_id, root)
        == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, &u32_type, 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && stored != NULL && stored->local_count == 4u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[0].type == u32_type
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[1].type == u32_type
        && stored->locals[2].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[2].type == tuple_type
        && stored->locals[3].kind == CM_MIR_LOCAL_USER
        && stored->locals[3].type == u32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 2u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    mutable_stored = (CmMirBody *)stored;
    statements = mutable_stored->basic_blocks[0].statements;
    operand = &statements[0].data.assign.value.data.use;
    projection = operand->data.place.projections;
    assert(statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 3u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_USE
        && operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        && operand->type == u32_type
        && operand->data.place.base == 2u
        && operand->data.place.type == u32_type
        && operand->data.place.projection_count == 1u
        && projection != NULL
        && projection->kind == CM_MIR_PROJECTION_FIELD
        && cm_hir_def_id_is_none(projection->definition)
        && projection->field_index == 0u
        && statements[1].data.assign.destination == CM_MIR_RETURN_LOCAL
        && statements[1].data.assign.value.data.use.kind
            == CM_MIR_OPERAND_MOVE
        && statements[1].data.assign.value.data.use.data.local == 3u
        && cm_mir_validate_place(&hir, stored, &operand->data.place)
            == CM_MIR_OK
        && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
            == CM_MIR_OK);

    saved_operand_kind = operand->kind;
    operand->kind = CM_MIR_OPERAND_COPY_PLACE;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->kind = saved_operand_kind;
    saved_type = operand->data.place.type;
    operand->data.place.type = generic_type;
    assert(cm_mir_validate_place(&hir, stored, &operand->data.place)
        == CM_MIR_INVARIANT_VIOLATION
        && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
            == CM_MIR_INVARIANT_VIOLATION);
    operand->data.place.type = saved_type;
    saved_type = operand->type;
    operand->type = generic_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->type = saved_type;
    saved_field_index = projection->field_index;
    projection->field_index = 1u;
    assert(cm_mir_validate_place(&hir, stored, &operand->data.place)
        == CM_MIR_INVARIANT_VIOLATION);
    projection->field_index = saved_field_index;
    stored_item = (CmHirItem *)cm_hir_get_item(&hir, item_id);
    assert(stored_item != NULL
        && stored_item->data.function_item.signature.parameters != NULL);
    stored_item->data.function_item.signature.parameters[1]
        .tuple_bindings[1].name = parameters[0].name;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    stored_item->data.function_item.signature.parameters[1]
        .tuple_bindings[1].name = CM_INTERN_ID_NONE;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static void test_reference_tuple_parameter_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirTypeId usize_type;
    CmHirDefId impl_definition;
    CmHirTypeId self_type;
    CmHirTypeId reference_types[2];
    CmHirTypeId tuple_types[2];
    CmHirExprId roots[2];
    CmHirItemId item_ids[2];
    CmHirTypeId alternate_erased_type;
    CmHirTypeId rejected_static_type;
    CmHirType reference_value;
    CmHirType self_value;
    CmHirItem impl_item;
    CmHirItemId impl_item_id;
    CmMirContext mir;
    uint32_t index;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_reference_tuple_parameter_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 240u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    usize_type = add_integer_type(&hir, CM_HIR_INT_USIZE, 2u);
    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_IMPL, test_span(5u, 230u), &impl_definition)
        == CM_HIR_OK);
    memset(&impl_item, 0, sizeof(impl_item));
    impl_item.kind = CM_HIR_ITEM_IMPL;
    impl_item.definition = impl_definition;
    impl_item.owner_module = root_module;
    impl_item.parent_definition = cm_hir_def_id_none();
    impl_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    impl_item.visibility.restriction = cm_hir_def_id_none();
    impl_item.span = test_span(5u, 230u);
    impl_item.data.impl_item.self_type = u32_type;
    impl_item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&hir, &impl_item, &impl_item_id) == CM_HIR_OK);
    memset(&self_value, 0, sizeof(self_value));
    self_value.kind = CM_HIR_TYPE_SELF_KIND;
    self_value.span = test_span(6u, 10u);
    self_value.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&hir, &self_value, &self_type) == CM_HIR_OK);
    for (index = 0u; index < 2u; ++index) {
        CmHirType type;
        CmHirTypeId tuple_element;
        CmHirDefId definition;
        CmHirFunctionParameter parameters[2];
        CmHirLocal locals[2];
        CmHirBody body;
        CmHirBodyId body_id;
        CmHirItem item;
        CmHirItemId item_id;
        CmHirExpr expression;
        CmHirExprId root;
        uint32_t start;

        start = 10u + index * 70u;
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_REFERENCE_KIND;
        type.span = test_span(start, start + 3u);
        type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
        type.data.reference_type.pointee = index == 1u
            ? usize_type : u32_type;
        type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
        assert(cm_hir_add_type(&hir, &type, &reference_types[index])
            == CM_HIR_OK);
        tuple_element = reference_types[index];
        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_TUPLE_KIND;
        type.span = test_span(start + 4u, start + 9u);
        type.data.tuple_type.elements = &tuple_element;
        type.data.tuple_type.element_count = 1u;
        assert(cm_hir_add_type(&hir, &type, &tuple_types[index])
            == CM_HIR_OK);
        assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
            CM_HIR_ITEM_FUNCTION, test_span(start, start + 80u),
            &definition) == CM_HIR_OK);
        memset(parameters, 0, sizeof(parameters));
        parameters[0].name = cm_hir_intern(&hir, "self");
        parameters[0].type = self_type;
        parameters[0].span = test_span(start + 11u, start + 15u);
        parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
        parameters[0].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
        parameters[1].type = tuple_types[index];
        parameters[1].span = test_span(start + 15u, start + 35u);
        parameters[1].binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
        parameters[1].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
        parameters[1].tuple_bindings[0].name = cm_hir_intern(&hir,
            index == 0u ? "erased" : "word_ref");
        parameters[1].tuple_bindings[0].span =
            test_span(start + 17u, start + 27u);
        memset(locals, 0, sizeof(locals));
        locals[0].name = parameters[0].name;
        locals[0].type = self_type;
        locals[0].span = parameters[0].span;
        locals[0].parameter_index = 0u;
        locals[1].name = parameters[1].tuple_bindings[0].name;
        locals[1].type = reference_types[index];
        locals[1].span = parameters[1].tuple_bindings[0].span;
        locals[1].parameter_index = 1u;
        memset(&body, 0, sizeof(body));
        body.owner = definition;
        body.origin = cm_hir_body_origin_item_source(definition);
        body.state = CM_HIR_BODY_UNLOWERED;
        body.expected_type = reference_types[index];
        body.locals = locals;
        body.local_count = 2u;
        body.parameter_count = 2u;
        body.source = 1u;
        body.source_expression_id = start;
        body.span = test_span(start, start + 80u);
        assert(cm_hir_add_body(&hir, &body, &body_id) == CM_HIR_OK);
        memset(&item, 0, sizeof(item));
        item.kind = CM_HIR_ITEM_FUNCTION;
        item.definition = definition;
        item.owner_module = root_module;
        item.parent_definition = impl_definition;
        item.name = cm_hir_intern(&hir,
            index == 0u ? "take_erased" : "take_word");
        item.visibility.kind = CM_HIR_VIS_PRIVATE;
        item.visibility.restriction = cm_hir_def_id_none();
        item.span = body.span;
        item.data.function_item.signature.parameters = parameters;
        item.data.function_item.signature.parameter_count = 2u;
        item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
        item.data.function_item.signature.return_type =
            reference_types[index];
        item.data.function_item.signature.abi =
            cm_hir_intern(&hir, "rust-call");
        item.data.function_item.signature.safety = CM_HIR_SAFE;
        item.data.function_item.body = body_id;
        item.data.function_item.trait_item_definition = cm_hir_def_id_none();
        assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
        item_ids[index] = item_id;
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_LOCAL;
        expression.owner_body = body_id;
        expression.type = reference_types[index];
        expression.span = test_span(start + 50u, start + 55u);
        expression.data.local.local_index = 1u;
        assert(cm_hir_add_expr(&hir, &expression, &root) == CM_HIR_OK);
        assert(cm_hir_set_body_root_expression(&hir, body_id, root)
            == CM_HIR_OK);
        roots[index] = root;
    }

    memset(&reference_value, 0, sizeof(reference_value));
    reference_value.kind = CM_HIR_TYPE_REFERENCE_KIND;
    reference_value.span = test_span(154u, 158u);
    reference_value.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    reference_value.data.reference_type.pointee = u32_type;
    reference_value.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&hir, &reference_value,
        &alternate_erased_type) == CM_HIR_OK);
    ((CmHirExpr *)cm_hir_get_expr(&hir, roots[0]))->type =
        alternate_erased_type;
    reference_value.span = test_span(159u, 163u);
    reference_value.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_add_type(&hir, &reference_value,
        &rejected_static_type) == CM_HIR_OK);

    cm_mir_context_init(&mir);
    assert(cm_mir_context_set_pointer_bits(&mir, 64u) == CM_MIR_OK);
    for (index = 0u; index < 2u; ++index) {
        CmMirLowerResult result;
        const CmMirBody *stored;
        const CmMirStatement *statements;
        const CmMirOperand *operand;

        result = cm_mir_lower_instance(&mir, &hir,
            (CmHirBodyId)(index + 1u), NULL, 0u);
        stored = cm_mir_get_body(&mir, result.body);
        assert(result.error_count == 0u && result.lowered_body_count == 1u
            && stored != NULL && stored->local_count == 4u
            && stored->locals[0].type == reference_types[index]
            && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
            && stored->locals[1].type == u32_type
            && stored->locals[2].kind == CM_MIR_LOCAL_ARGUMENT
            && stored->locals[2].type == tuple_types[index]
            && stored->locals[3].kind == CM_MIR_LOCAL_USER
            && stored->locals[3].type == reference_types[index]
            && stored->basic_block_count == 1u
            && stored->basic_blocks[0].statement_count == 2u);
        statements = stored->basic_blocks[0].statements;
        operand = &statements[0].data.assign.value.data.use;
        assert(statements[0].data.assign.destination == 3u
            && operand->kind == CM_MIR_OPERAND_MOVE_PLACE
            && operand->type == reference_types[index]
            && operand->data.place.base == 2u
            && operand->data.place.type == reference_types[index]
            && cm_mir_validate_place(&hir, stored, &operand->data.place)
                == CM_MIR_OK
            && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
                == CM_MIR_OK);
    }
    cm_mir_context_destroy(&mir);
    {
        CmHirItem *ordinary;
        CmHirFunctionParameter *ordinary_parameter;
        CmMirLowerResult ordinary_result;

        ordinary = (CmHirItem *)cm_hir_get_item(&hir, item_ids[0]);
        assert(ordinary != NULL
            && ordinary->data.function_item.signature.parameters != NULL);
        ordinary_parameter =
            &ordinary->data.function_item.signature.parameters[1];
        ordinary_parameter->binding_kind = CM_HIR_BINDING_NAMED;
        ordinary_parameter->name = ordinary_parameter->tuple_bindings[0].name;
        cm_mir_context_init(&mir);
        assert(cm_mir_context_set_pointer_bits(&mir, 64u) == CM_MIR_OK);
        ordinary_result = cm_mir_lower_instance(&mir, &hir, 1u, NULL, 0u);
        assert(ordinary_result.error_count == 1u
            && ordinary_result.lowered_body_count == 0u
            && ordinary_result.body == CM_MIR_BODY_NONE);
        cm_mir_context_destroy(&mir);
        ordinary_parameter->binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
        ordinary_parameter->name = CM_INTERN_ID_NONE;
    }
    cm_mir_context_init(&mir);
    assert(cm_mir_context_set_pointer_bits(&mir, 32u) == CM_MIR_OK);
    {
        CmMirLowerResult word_result;

        word_result = cm_mir_lower_instance(&mir, &hir, 2u, NULL, 0u);
        assert(word_result.error_count == 0u
            && word_result.lowered_body_count == 1u
            && cm_mir_validate_monomorphized_body(&mir, &hir,
                word_result.body) == CM_MIR_OK);
    }
    cm_mir_context_destroy(&mir);
    ((CmHirExpr *)cm_hir_get_expr(&hir, roots[0]))->type =
        rejected_static_type;
    cm_mir_context_init(&mir);
    {
        CmMirLowerResult rejected;

        rejected = cm_mir_lower_instance(&mir, &hir, 1u, NULL, 0u);
        assert(rejected.error_count == 1u
            && rejected.lowered_body_count == 0u
            && rejected.body == CM_MIR_BODY_NONE);
    }
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

static void test_newtype_parameter_lowering(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirTypeId i32_type;
    CmHirDefId newtype_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId newtype_generic;
    CmHirGenericParamId function_generic;
    CmHirType type;
    CmHirTypeId declared_field_type;
    CmHirTypeId function_generic_type;
    CmHirGenericArg argument;
    CmHirTypeId applied_type;
    CmHirTypeId wrong_applied_type;
    CmHirField field;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirDefId function_definition;
    CmHirFunctionParameter parameter;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirExpr expression;
    CmHirExprId root;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    CmMirBody *mutable_stored;
    CmMirStatement *statements;
    CmMirOperand *operand;
    CmMirPlaceProjection *projection;
    CmMirPlaceProjectionKind saved_projection_kind;
    CmMirOperandKind saved_operand_kind;
    CmHirTypeId saved_type;
    CmHirDefId saved_definition;
    CmSpan saved_span;
    uint32_t saved_value;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir,
        cm_hir_intern(&hir, "mir_newtype_parameter_boundary"),
        CM_HIR_EDITION_2021, test_span(0u, 140u), &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 1u);
    i32_type = add_integer_type(&hir, CM_HIR_INT_I32, 2u);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(10u, 40u), &newtype_definition)
        == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = newtype_definition;
    generic.name = cm_hir_intern(&hir, "T");
    generic.span = test_span(18u, 19u);
    assert(cm_hir_add_generic_param(&hir, &generic, &newtype_generic)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = generic.span;
    type.data.parameter_type.parameter = newtype_generic;
    assert(cm_hir_add_type(&hir, &type, &declared_field_type) == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.type = declared_field_type;
    field.visibility.kind = CM_HIR_VIS_PUBLIC;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(24u, 28u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = newtype_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&hir, "Newtype");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(10u, 40u);
    item.generic_parameter_start = newtype_generic;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(50u, 120u), &function_definition)
        == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = function_definition;
    generic.name = cm_hir_intern(&hir, "E");
    generic.span = test_span(54u, 55u);
    assert(cm_hir_add_generic_param(&hir, &generic, &function_generic)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = generic.span;
    type.data.parameter_type.parameter = function_generic;
    assert(cm_hir_add_type(&hir, &type, &function_generic_type)
        == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = function_generic_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(60u, 70u);
    type.data.named_type.definition = newtype_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&hir, &type, &applied_type) == CM_HIR_OK);
    argument.data.type = i32_type;
    assert(cm_hir_add_type(&hir, &type, &wrong_applied_type) == CM_HIR_OK);

    memset(&parameter, 0, sizeof(parameter));
    parameter.type = applied_type;
    parameter.span = test_span(60u, 80u);
    parameter.binding_kind = CM_HIR_BINDING_NEWTYPE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.newtype_binding.name = cm_hir_intern(&hir, "value");
    parameter.newtype_binding.span = test_span(68u, 73u);
    memset(&local, 0, sizeof(local));
    local.name = parameter.newtype_binding.name;
    local.type = function_generic_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = parameter.newtype_binding.span;
    local.parameter_index = 0u;
    local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = function_definition;
    body.origin = cm_hir_body_origin_item_source(function_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = function_generic_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 50u;
    body.span = test_span(50u, 120u);
    assert(cm_hir_add_body(&hir, &body, &body_id) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = function_definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&hir, "unwrap_newtype");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    item.generic_parameter_start = function_generic;
    item.generic_parameter_count = 1u;
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = function_generic_type;
    item.data.function_item.signature.abi = cm_hir_intern(&hir, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = function_generic_type;
    expression.span = test_span(90u, 95u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&hir, body_id, root)
        == CM_HIR_OK);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_instance(&mir, &hir, body_id, &u32_type, 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && stored != NULL && stored->owned_storage != NULL
        && stored->instance.substitution_count == 1u
        && stored->instance.substitutions[0] == u32_type
        && stored->local_count == 3u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[0].type == u32_type
        && stored->locals[1].kind == CM_MIR_LOCAL_ARGUMENT
        && stored->locals[1].type == applied_type
        && stored->locals[2].kind == CM_MIR_LOCAL_USER
        && stored->locals[2].type == u32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 2u
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    mutable_stored = (CmMirBody *)stored;
    statements = mutable_stored->basic_blocks[0].statements;
    assert(statements != NULL && statements[0].kind == CM_MIR_STATEMENT_ASSIGN
        && statements[0].data.assign.destination == 2u
        && statements[0].data.assign.value.kind == CM_MIR_RVALUE_USE
        && statements[1].data.assign.destination == CM_MIR_RETURN_LOCAL);
    operand = &statements[0].data.assign.value.data.use;
    projection = operand->data.place.projections;
    assert(operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        && operand->type == u32_type
        && operand->data.place.base == 1u
        && operand->data.place.type == u32_type
        && operand->data.place.projection_count == 1u
        && operand->data.place.span.source
            == parameter.newtype_binding.span.source
        && operand->data.place.span.start
            == parameter.newtype_binding.span.start
        && operand->data.place.span.end == parameter.newtype_binding.span.end
        && projection != NULL
        && projection->kind == CM_MIR_PROJECTION_FIELD
        && cm_hir_def_id_equal(projection->definition,
            newtype_definition)
        && projection->field_index == 0u
        && cm_mir_validate_place(&hir, stored, &operand->data.place)
            == CM_MIR_OK
        && cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
            == CM_MIR_OK);

    saved_type = mutable_stored->locals[1].type;
    mutable_stored->locals[1].type = wrong_applied_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    mutable_stored->locals[1].type = saved_type;

    saved_operand_kind = operand->kind;
    operand->kind = CM_MIR_OPERAND_COPY_PLACE;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->kind = saved_operand_kind;
    saved_projection_kind = projection->kind;
    projection->kind = CM_MIR_PROJECTION_DEREFERENCE;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    projection->kind = saved_projection_kind;
    saved_definition = projection->definition;
    projection->definition = function_definition;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    projection->definition = saved_definition;
    saved_value = projection->field_index;
    projection->field_index = 1u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    projection->field_index = saved_value;
    saved_value = operand->data.place.base;
    operand->data.place.base = CM_MIR_RETURN_LOCAL;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->data.place.base = saved_value;
    saved_type = operand->type;
    operand->type = i32_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->type = saved_type;
    saved_type = operand->data.place.type;
    operand->data.place.type = i32_type;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->data.place.type = saved_type;
    saved_span = operand->data.place.span;
    operand->data.place.span.start += 1u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->data.place.span = saved_span;
    saved_value = operand->data.place.projection_count;
    operand->data.place.projection_count = 0u;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    operand->data.place.projection_count = saved_value;
    saved_value = statements[0].data.assign.destination;
    statements[0].data.assign.destination = CM_MIR_RETURN_LOCAL;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_INVARIANT_VIOLATION);
    statements[0].data.assign.destination = saved_value;
    assert(cm_mir_validate_monomorphized_body(&mir, &hir, result.body)
        == CM_MIR_OK);

    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
}

int main(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId i32_type;
    CmHirTypeId u32_type;
    CmHirTypeId alternate_u32_type;
    CmHirTypeId usize_type;
    CmHirBodyId good_body;
    CmHirBodyId unlowered_body;
    CmHirBodyId wrong_type_body;
    CmHirBodyId large_body;
    CmHirBodyId negative_body;
    CmMirContext mir;
    CmMirLowerResult result;
    const CmMirBody *stored;
    size_t count_before;
    CmMirLocal invalid_local;
    CmMirStatement invalid_statement;
    CmMirBasicBlock invalid_block;
    CmMirBody invalid_body;
    CmMirBodyId invalid_id;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir, cm_hir_intern(&hir, "mir_test"),
        CM_HIR_EDITION_2021, test_span(0u, 700u), &crate_id, &root_module)
        == CM_HIR_OK);
    i32_type = add_integer_type(&hir, CM_HIR_INT_I32, 1u);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 3u);
    alternate_u32_type = add_integer_type(&hir, CM_HIR_INT_U32, 5u);
    usize_type = add_integer_type(&hir, CM_HIR_INT_USIZE, 6u);
    good_body = add_function_body(&hir, crate_id, root_module, "main",
        i32_type, CM_HIR_BODY_TYPED, 7u, 0u, 20u);
    unlowered_body = add_function_body(&hir, crate_id, root_module,
        "pending", i32_type, CM_HIR_BODY_UNLOWERED, 0u, 0u, 60u);
    wrong_type_body = add_function_body(&hir, crate_id, root_module,
        "wrong_type", u32_type, CM_HIR_BODY_TYPED, 7u, 0u, 100u);
    large_body = add_function_body(&hir, crate_id, root_module, "large",
        i32_type, CM_HIR_BODY_TYPED, 2147483648ULL, 0u, 140u);
    negative_body = add_function_body(&hir, crate_id, root_module,
        "negative", i32_type, CM_HIR_BODY_TYPED,
        (uint64_t)(uint32_t)-17, UINT64_MAX, 180u);

    cm_mir_context_init(&mir);
    result = cm_mir_lower_body(&mir, &hir, good_body);
    assert(result.error_count == 0u && result.lowered_body_count == 1u
        && result.body == 1u && cm_mir_body_count(&mir) == 1u);
    stored = cm_mir_get_body(&mir, result.body);
    assert(stored != NULL && stored->local_count == 1u
        && stored->locals[0].kind == CM_MIR_LOCAL_RETURN
        && stored->locals[0].type == i32_type
        && stored->basic_block_count == 1u
        && stored->basic_blocks[0].statement_count == 1u
        && stored->basic_blocks[0].statements[0].kind
            == CM_MIR_STATEMENT_ASSIGN
        && stored->basic_blocks[0].statements[0].data.assign.destination
            == CM_MIR_RETURN_LOCAL
        && stored->basic_blocks[0].statements[0].data.assign.value.kind
            == CM_MIR_RVALUE_USE
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .kind == CM_MIR_CONSTANT_I32
        && stored->basic_blocks[0].statements[0].data.assign.value.data.use
            .data.i32_value == 7
        && stored->basic_blocks[0].terminator.kind
            == CM_MIR_TERMINATOR_RETURN);
    assert_dump(&mir);
    assert_dump(&mir);

    result = cm_mir_lower_body(&mir, &hir, negative_body);
    assert(result.error_count == 0u && result.body == 2u
        && cm_mir_get_body(&mir, result.body) != NULL
        && cm_mir_get_body(&mir, result.body)->basic_blocks[0]
            .statements[0].data.assign.value.data.use.data.i32_value == -17);

    count_before = cm_mir_body_count(&mir);
    result = cm_mir_lower_body(&mir, &hir, good_body);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_MODEL_FAILURE
        && result.first_error.mir_status == CM_MIR_INVARIANT_VIOLATION
        && result.body == CM_MIR_BODY_NONE
        && cm_mir_body_count(&mir) == count_before);
    result = cm_mir_lower_body(&mir, &hir, unlowered_body);
    assert(result.error_count == 1u
        && result.first_error.kind
            == CM_MIR_LOWER_UNSUPPORTED_BODY_STATE
        && cm_mir_body_count(&mir) == count_before);
    result = cm_mir_lower_body(&mir, &hir, wrong_type_body);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_UNSUPPORTED_TYPE
        && cm_mir_body_count(&mir) == count_before);
    result = cm_mir_lower_body(&mir, &hir, large_body);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_CONSTANT_OUT_OF_RANGE
        && cm_mir_body_count(&mir) == count_before);
    result = cm_mir_lower_body(&mir, &hir, (CmHirBodyId)9999u);
    assert(result.error_count == 1u
        && result.first_error.kind == CM_MIR_LOWER_INVALID_HIR
        && cm_mir_body_count(&mir) == count_before);

    memset(&invalid_local, 0, sizeof(invalid_local));
    invalid_local.kind = CM_MIR_LOCAL_RETURN;
    invalid_local.type = i32_type;
    memset(&invalid_statement, 0, sizeof(invalid_statement));
    invalid_statement.kind = CM_MIR_STATEMENT_ASSIGN;
    invalid_statement.data.assign.destination = 1u;
    invalid_statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    invalid_statement.data.assign.value.type = i32_type;
    invalid_statement.data.assign.value.data.use.kind = CM_MIR_CONSTANT_I32;
    invalid_statement.data.assign.value.data.use.type = i32_type;
    memset(&invalid_block, 0, sizeof(invalid_block));
    invalid_block.statements = &invalid_statement;
    invalid_block.statement_count = 1u;
    invalid_block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(&invalid_body, 0, sizeof(invalid_body));
    invalid_body.owner.crate_id = crate_id;
    invalid_body.owner.index = 99u;
    invalid_body.source_body = 99u;
    invalid_body.locals = &invalid_local;
    invalid_body.local_count = 1u;
    invalid_body.basic_blocks = &invalid_block;
    invalid_body.basic_block_count = 1u;
    invalid_id = 88u;
    assert(cm_mir_add_body(&mir, &invalid_body, &invalid_id)
        == CM_MIR_INVARIANT_VIOLATION);
    assert(invalid_id == CM_MIR_BODY_NONE
        && cm_mir_body_count(&mir) == count_before);

    assert(cm_mir_get_body(&mir, CM_MIR_BODY_NONE) == NULL);
    assert(cm_mir_get_body(&mir, 3u) == NULL);
    assert(strcmp(cm_mir_status_name(CM_MIR_OK), "ok") == 0);
    assert(strcmp(cm_mir_lower_error_kind_name(
        CM_MIR_LOWER_UNSUPPORTED_TYPE), "unsupported type") == 0);

    test_nested_add_lowering(&hir, crate_id, root_module, u32_type,
        alternate_u32_type, i32_type, good_body);
    test_subtract_lowering(&hir, crate_id, root_module, u32_type);
    test_if_equal_lowering(&hir, crate_id, root_module, u32_type);
    test_usize_less_if_lowering(&hir, crate_id, root_module, usize_type);
    test_call_argument_tree_lowering(&hir, crate_id, root_module,
        u32_type, alternate_u32_type, good_body);
    test_ordinary_call_lowering(&hir, crate_id, root_module, u32_type,
        good_body);
    test_let_flow_lowering(&hir, crate_id, root_module, u32_type);
    test_aggregate_and_field_lowering();
    test_aggregate_call_lowering();
    test_reference_lowering_stays_fail_closed();
    test_discard_parameter_lowering();
    test_tuple_parameter_lowering();
    test_unary_tuple_parameter_lowering();
    test_reference_tuple_parameter_lowering();
    test_newtype_parameter_lowering();
    cm_mir_context_destroy(&mir);
    cm_hir_context_destroy(&hir);
    puts("mir lowering tests: ok");
    return 0;
}
