#define _POSIX_C_SOURCE 200809L

#include "cm/codegen/c.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Borrow and dereference HIR are not admitted to exact MIR yet.  Compile one
 * test-local copy of the backend so its private, deliberately unreachable
 * formatters can still be checked without weakening that public boundary.
 */
CmCEmitStatus cm_c_emit_program_reference_test_copy(CmStrBuf *output,
    const CmHirContext *hir, const CmMirBody *body,
    CmHirItemId entry_item, const CmTargetDesc *target);
CmCEmitStatus cm_c_emit_reachable_program_reference_test_copy(
    CmStrBuf *output, const CmHirContext *hir, const CmMirContext *mir,
    const CmSemanticAdmission *admission,
    const CmMirBodyId *roots, uint32_t root_count,
    const CmTargetDesc *target);
const char *cm_c_emit_status_name_reference_test_copy(CmCEmitStatus status);

#define cm_c_emit_program cm_c_emit_program_reference_test_copy
#define cm_c_emit_reachable_program \
    cm_c_emit_reachable_program_reference_test_copy
#define cm_c_emit_status_name cm_c_emit_status_name_reference_test_copy
#include "../../src/codegen/c.c"
#undef cm_c_emit_status_name
#undef cm_c_emit_reachable_program
#undef cm_c_emit_program

typedef struct TestProgram {
    CmHirContext hir;
    CmHirItemId entry_item;
    CmHirTypeId i32_type;
    CmMirLocal local;
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirBody body;
} TestProgram;

typedef struct TestReferenceProgram {
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u8_type;
    CmHirTypeId u32_type;
    CmHirTypeId pair_type;
    CmHirTypeId shared_u32_type;
    CmHirTypeId shared_pair_type;
    CmHirTypeId mutable_u32_type;
    CmHirTypeId static_u32_type;
    CmHirTypeId raw_u32_type;
    CmHirTypeId dyn_trait_type;
    CmHirTypeId shared_dyn_trait_type;
    CmHirDefId pair_definition;
    CmHirDefId trait_definition;
    CmHirDefId export_definition;
    CmHirDefId field_definition;
    CmHirBodyId export_body;
    CmHirBodyId field_body;
} TestReferenceProgram;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static const CmTargetDesc test_target = {
    "x86_64-unknown-linux-gnu", "x86_64", "linux", "gnu", "", "unknown",
    "unix", 64u, CM_ENDIAN_LITTLE, NULL, 0u, NULL, 0u
};

static CmHirTypeId add_reference_test_type(TestReferenceProgram *program,
    CmHirTypeId pointee, CmHirMutability mutability, CmHirRegionKind region,
    uint32_t start)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(start, start + 1u);
    type.data.reference_type.pointee = pointee;
    type.data.reference_type.mutability = mutability;
    type.data.reference_type.region.kind = region;
    assert(cm_hir_add_type(&program->hir, &type, &id) == CM_HIR_OK);
    return id;
}

static void add_reference_test_function(TestReferenceProgram *program,
    CmHirDefId definition, const char *name, CmHirTypeId parameter_type,
    int exported, uint32_t start, CmHirBodyId *out_body)
{
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirExpr expression;
    CmHirExprId root;
    CmHirAttribute attribute;
    CmHirItem item;
    CmHirItemId item_id;
    uint32_t parameter_count;

    parameter_count = exported ? 2u : 1u;
    memset(parameters, 0, sizeof(parameters));
    memset(locals, 0, sizeof(locals));
    parameters[0].name = cm_hir_intern(&program->hir, "reference");
    parameters[0].type = parameter_type;
    parameters[0].span = test_span(start + 2u, start + 3u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    locals[0].name = parameters[0].name;
    locals[0].type = parameter_type;
    locals[0].span = parameters[0].span;
    locals[0].parameter_index = 0u;
    if (exported) {
        parameters[1].name = cm_hir_intern(&program->hir, "value");
        parameters[1].type = program->u32_type;
        parameters[1].span = test_span(start + 4u, start + 5u);
        parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
        locals[1].name = parameters[1].name;
        locals[1].type = program->u32_type;
        locals[1].span = parameters[1].span;
        locals[1].parameter_index = 1u;
    }
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = program->u32_type;
    body.locals = locals;
    body.local_count = parameter_count;
    body.parameter_count = parameter_count;
    body.source = 1u;
    body.source_expression_id = start;
    body.span = test_span(start, start + 20u);
    assert(cm_hir_add_body(&program->hir, &body, out_body) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = exported ? CM_HIR_EXPR_LOCAL : CM_HIR_EXPR_INTEGER;
    expression.owner_body = *out_body;
    expression.type = program->u32_type;
    expression.span = test_span(start + 10u, start + 11u);
    if (exported) expression.data.local.local_index = 1u;
    else expression.data.integer.low_bits = 0u;
    assert(cm_hir_add_expr(&program->hir, &expression, &root) == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&program->hir, *out_body, root)
        == CM_HIR_OK);

    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&program->hir, "no_mangle");
    attribute.span = test_span(start, start + 1u);
    attribute.source_attribute = 1u;
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = program->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&program->hir, name);
    item.visibility.kind = exported ? CM_HIR_VIS_PUBLIC : CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = body.span;
    if (exported) {
        item.attributes = &attribute;
        item.attribute_count = 1u;
    }
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = parameter_count;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = program->u32_type;
    item.data.function_item.signature.abi = cm_hir_intern(&program->hir,
        exported ? "C" : "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = *out_body;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&program->hir, &item, &item_id) == CM_HIR_OK);
}

static void reference_program_init(TestReferenceProgram *program)
{
    CmHirAttribute attributes[3];
    CmHirType type;
    CmHirField field;
    CmHirItem item;
    CmHirItemId item_id;

    memset(program, 0, sizeof(*program));
    cm_hir_context_init(&program->hir);
    assert(cm_hir_create_crate(&program->hir,
        cm_hir_intern(&program->hir, "reference_codegen"),
        CM_HIR_EDITION_2021, test_span(0u, 500u), &program->crate_id,
        &program->root_module) == CM_HIR_OK);
    memset(attributes, 0, sizeof(attributes));
    attributes[0].metadata = cm_hir_intern(&program->hir,
        "feature(no_core)");
    attributes[1].metadata = cm_hir_intern(&program->hir, "no_core");
    attributes[2].metadata = cm_hir_intern(&program->hir, "no_main");
    attributes[0].span = test_span(1u, 2u);
    attributes[1].span = test_span(3u, 4u);
    attributes[2].span = test_span(5u, 6u);
    attributes[0].source_attribute = 1u;
    attributes[1].source_attribute = 2u;
    attributes[2].source_attribute = 3u;
    assert(cm_hir_set_crate_inner_attributes(&program->hir,
        program->crate_id, attributes, 3u) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(10u, 11u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&program->hir, &type, &program->u8_type)
        == CM_HIR_OK);
    type.span = test_span(12u, 13u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&program->hir, &type, &program->u32_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&program->hir,
        program->crate_id, CM_HIR_ITEM_STRUCT, test_span(20u, 40u),
        &program->pair_definition) == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.name = cm_hir_intern(&program->hir, "value");
    field.type = program->u32_type;
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(25u, 30u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = program->pair_definition;
    item.owner_module = program->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&program->hir, "Pair");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(20u, 40u);
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&program->hir, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(20u, 40u);
    type.data.named_type.definition = program->pair_definition;
    assert(cm_hir_add_type(&program->hir, &type, &program->pair_type)
        == CM_HIR_OK);

    program->shared_u32_type = add_reference_test_type(program,
        program->u32_type, CM_HIR_IMMUTABLE, CM_HIR_REGION_ERASED, 50u);
    program->shared_pair_type = add_reference_test_type(program,
        program->pair_type, CM_HIR_IMMUTABLE, CM_HIR_REGION_ERASED, 52u);
    program->mutable_u32_type = add_reference_test_type(program,
        program->u32_type, CM_HIR_MUTABLE, CM_HIR_REGION_ERASED, 54u);
    program->static_u32_type = add_reference_test_type(program,
        program->u32_type, CM_HIR_IMMUTABLE, CM_HIR_REGION_STATIC, 56u);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = test_span(58u, 59u);
    type.data.raw_pointer_type.pointee = program->u32_type;
    type.data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&program->hir, &type, &program->raw_u32_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&program->hir,
        program->crate_id, CM_HIR_ITEM_TRAIT, test_span(60u, 80u),
        &program->trait_definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TRAIT;
    item.definition = program->trait_definition;
    item.owner_module = program->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&program->hir, "Trait");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(60u, 80u);
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&program->hir, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_DYN_TRAIT_KIND;
    type.span = test_span(82u, 84u);
    type.data.dyn_trait_type.principal_trait.definition =
        program->trait_definition;
    type.data.dyn_trait_type.region.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_type(&program->hir, &type, &program->dyn_trait_type)
        == CM_HIR_OK);
    program->shared_dyn_trait_type = add_reference_test_type(program,
        program->dyn_trait_type, CM_HIR_IMMUTABLE, CM_HIR_REGION_ERASED,
        85u);

    assert(cm_hir_reserve_item_definition_as(&program->hir,
        program->crate_id, CM_HIR_ITEM_FUNCTION, test_span(100u, 140u),
        &program->export_definition) == CM_HIR_OK);
    add_reference_test_function(program, program->export_definition,
        "reference_entry", program->shared_u32_type, 1, 100u,
        &program->export_body);
    assert(cm_hir_reserve_item_definition_as(&program->hir,
        program->crate_id, CM_HIR_ITEM_FUNCTION, test_span(160u, 200u),
        &program->field_definition) == CM_HIR_OK);
    add_reference_test_function(program, program->field_definition,
        "field_probe", program->shared_pair_type, 0, 160u,
        &program->field_body);
}

static void reference_program_destroy(TestReferenceProgram *program)
{
    cm_hir_context_destroy(&program->hir);
}

static void test_program_init(TestProgram *program, int32_t value)
{
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId definition;
    CmHirType type;
    CmHirExpr expression;
    CmHirExprId integer_expression;
    CmHirExprId block_expression;
    CmHirBody hir_body;
    CmHirBodyId hir_body_id;
    CmHirAttribute crate_attributes[3];
    CmHirAttribute attribute;
    CmHirItem item;

    memset(program, 0, sizeof(*program));
    cm_hir_context_init(&program->hir);
    assert(cm_hir_create_crate(&program->hir,
        cm_hir_intern(&program->hir, "no_core_exit"),
        CM_HIR_EDITION_2021, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    memset(crate_attributes, 0, sizeof(crate_attributes));
    crate_attributes[0].metadata = cm_hir_intern(&program->hir,
        "feature(no_core)");
    crate_attributes[1].metadata = cm_hir_intern(&program->hir,
        "no_core");
    crate_attributes[2].metadata = cm_hir_intern(&program->hir,
        "no_main");
    crate_attributes[0].span = test_span(1u, 2u);
    crate_attributes[1].span = test_span(3u, 4u);
    crate_attributes[2].span = test_span(5u, 6u);
    crate_attributes[0].source_attribute = 1u;
    crate_attributes[1].source_attribute = 2u;
    crate_attributes[2].source_attribute = 3u;
    assert(cm_hir_set_crate_inner_attributes(&program->hir, crate_id,
        crate_attributes, 3u) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(20u, 23u);
    type.data.integer_type.kind = CM_HIR_INT_I32;
    assert(cm_hir_add_type(&program->hir, &type, &program->i32_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&program->hir, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 40u), &definition)
        == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.type = program->i32_type;
    expression.span = test_span(30u, 34u);
    expression.data.integer.low_bits = (uint32_t)value;
    expression.data.integer.high_bits = value < 0 ? UINT64_MAX : 0u;
    assert(cm_hir_add_expr(&program->hir, &expression,
        &integer_expression) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BLOCK;
    expression.type = program->i32_type;
    expression.span = test_span(29u, 35u);
    expression.data.block.tail_expression = integer_expression;
    assert(cm_hir_add_expr(&program->hir, &expression,
        &block_expression) == CM_HIR_OK);

    memset(&hir_body, 0, sizeof(hir_body));
    hir_body.owner = definition;
    hir_body.origin = cm_hir_body_origin_item_source(definition);
    hir_body.state = CM_HIR_BODY_TYPED;
    hir_body.expected_type = program->i32_type;
    hir_body.source = 1u;
    hir_body.source_expression_id = 1u;
    hir_body.root_expression = block_expression;
    hir_body.span = test_span(29u, 35u);
    assert(cm_hir_add_body(&program->hir, &hir_body, &hir_body_id)
        == CM_HIR_OK);

    memset(&attribute, 0, sizeof(attribute));
    attribute.metadata = cm_hir_intern(&program->hir, "no_mangle");
    attribute.span = test_span(1u, 9u);
    attribute.source_attribute = 1u;
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = definition;
    item.owner_module = root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&program->hir, "main");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(10u, 40u);
    item.attributes = &attribute;
    item.attribute_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = program->i32_type;
    item.data.function_item.signature.abi = cm_hir_intern(&program->hir,
        "C");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = hir_body_id;
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&program->hir, &item, &program->entry_item)
        == CM_HIR_OK);

    program->local.kind = CM_MIR_LOCAL_RETURN;
    program->local.type = program->i32_type;
    program->statement.kind = CM_MIR_STATEMENT_ASSIGN;
    program->statement.data.assign.destination = CM_MIR_RETURN_LOCAL;
    program->statement.data.assign.value.kind = CM_MIR_RVALUE_USE;
    program->statement.data.assign.value.type = program->i32_type;
    program->statement.data.assign.value.data.use.kind =
        CM_MIR_CONSTANT_I32;
    program->statement.data.assign.value.data.use.type = program->i32_type;
    program->statement.data.assign.value.data.use.data.i32_value = value;
    program->block.statements = &program->statement;
    program->block.statement_count = 1u;
    program->block.terminator.kind = CM_MIR_TERMINATOR_RETURN;
    program->body.owner = definition;
    program->body.source_body = hir_body_id;
    program->body.locals = &program->local;
    program->body.local_count = 1u;
    program->body.basic_blocks = &program->block;
    program->body.basic_block_count = 1u;
}

static void test_program_destroy(TestProgram *program)
{
    cm_hir_context_destroy(&program->hir);
}

static void assert_unchanged_failure(CmCEmitStatus expected,
    CmStrBuf *output, const TestProgram *program, const CmMirBody *body,
    CmHirItemId entry_item, const CmTargetDesc *target)
{
    size_t length;

    length = output->len;
    assert(cm_c_emit_program(output, &program->hir, body, entry_item,
        target) == expected);
    assert(output->len == length);
    assert(strcmp(cm_str_buf_c_str(output), "sentinel") == 0);
}

static void test_exact_output_and_determinism(void)
{
    static const char expected[] =
        "#include <limits.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "#if CHAR_BIT != 8\n"
        "# error \"cmrustc requires 8-bit bytes for Rust i32\"\n"
        "#endif\n"
        "#if !defined(INT32_MAX) || INT32_MAX != 2147483647\n"
        "# error \"cmrustc requires an exact 32-bit int32_t\"\n"
        "#endif\n"
        "#if !defined(INT32_MIN) || INT32_MIN != (-2147483647 - 1)\n"
        "# error \"cmrustc requires two's-complement Rust i32\"\n"
        "#endif\n"
        "#if INT_MAX < INT32_MAX || INT_MIN > INT32_MIN\n"
        "# error \"C int cannot represent a Rust i32 exit value\"\n"
        "#endif\n"
        "\n"
        "int main(void)\n"
        "{\n"
        "    int32_t _0;\n"
        "    _0 = (int32_t)(7);\n"
        "    return (int)_0;\n"
        "}\n";
    TestProgram program;
    CmStrBuf first;
    CmStrBuf second;

    test_program_init(&program, 7);
    cm_str_buf_init(&first);
    cm_str_buf_init(&second);
    assert(cm_c_emit_program(&first, &program.hir, &program.body,
        program.entry_item, &test_target) == CM_C_EMIT_OK);
    assert(cm_c_emit_program(&second, &program.hir, &program.body,
        program.entry_item, &test_target) == CM_C_EMIT_OK);
    assert(strcmp(cm_str_buf_c_str(&first), expected) == 0);
    assert(strcmp(cm_str_buf_c_str(&first),
        cm_str_buf_c_str(&second)) == 0);
    cm_str_buf_destroy(&second);
    cm_str_buf_destroy(&first);
    test_program_destroy(&program);
}

static void test_rejection_and_rollback(void)
{
    TestProgram program;
    CmStrBuf output;
    CmTargetDesc target;
    CmMirBody body;
    CmHirCrate *crate_value;
    CmHirItem *entry;
    CmHirItem extra_item;
    CmHirBody *source_body;
    CmInternId unknown_metadata;
    CmInternId old_metadata;
    CmHirDefId old_owner;
    CmMirTerminatorKind old_terminator;
    uint32_t old_attribute_count;

    test_program_init(&program, -17);
    unknown_metadata = cm_hir_intern(&program.hir, "allow(dead_code)");
    assert(unknown_metadata != CM_INTERN_ID_NONE);
    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "sentinel");

    assert(cm_c_emit_program(NULL, &program.hir, &program.body,
        program.entry_item, &test_target) == CM_C_EMIT_INVALID_ARGUMENT);
    assert_unchanged_failure(CM_C_EMIT_INVALID_ENTRY, &output, &program,
        &program.body, CM_HIR_ITEM_NONE, &test_target);

    target = test_target;
    target.endian = CM_ENDIAN_BIG;
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_TARGET, &output,
        &program, &program.body, program.entry_item, &target);

    crate_value = (CmHirCrate *)cm_vec_at(&program.hir.crates, 0u);
    assert(crate_value != NULL);
    old_attribute_count = crate_value->inner_attribute_count;
    crate_value->inner_attribute_count = 2u;
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_ENTRY, &output,
        &program, &program.body, program.entry_item, &test_target);
    crate_value->inner_attribute_count = old_attribute_count;
    old_metadata = crate_value->inner_attributes[2].metadata;
    crate_value->inner_attributes[2].metadata =
        crate_value->inner_attributes[1].metadata;
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_ENTRY, &output,
        &program, &program.body, program.entry_item, &test_target);
    crate_value->inner_attributes[2].metadata = old_metadata;
    crate_value->inner_attributes[2].metadata = unknown_metadata;
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_ENTRY, &output,
        &program, &program.body, program.entry_item, &test_target);
    crate_value->inner_attributes[2].metadata = old_metadata;

    entry = (CmHirItem *)cm_vec_at(&program.hir.items,
        (size_t)program.entry_item - 1u);
    assert(entry != NULL);
    old_attribute_count = entry->attribute_count;
    entry->attribute_count = 0u;
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_ENTRY, &output,
        &program, &program.body, program.entry_item, &test_target);
    entry->attribute_count = old_attribute_count;

    memset(&extra_item, 0, sizeof(extra_item));
    assert(cm_vec_push(&program.hir.items, &extra_item) != NULL);
    assert_unchanged_failure(CM_C_EMIT_UNSUPPORTED_ENTRY, &output,
        &program, &program.body, program.entry_item, &test_target);
    cm_vec_resize(&program.hir.items, 1u);

    body = program.body;
    old_owner = body.owner;
    body.owner = cm_hir_def_id_none();
    assert_unchanged_failure(CM_C_EMIT_INVALID_MIR, &output, &program,
        &body, program.entry_item, &test_target);
    body.owner = old_owner;
    body.local_count = 0u;
    assert_unchanged_failure(CM_C_EMIT_INVALID_MIR, &output, &program,
        &body, program.entry_item, &test_target);

    body = program.body;
    old_terminator = program.block.terminator.kind;
    program.block.terminator.kind = (CmMirTerminatorKind)99;
    assert_unchanged_failure(CM_C_EMIT_INVALID_MIR, &output, &program,
        &body, program.entry_item, &test_target);
    program.block.terminator.kind = old_terminator;

    source_body = (CmHirBody *)cm_vec_at(&program.hir.bodies,
        (size_t)program.body.source_body - 1u);
    assert(source_body != NULL);
    source_body->state = CM_HIR_BODY_UNLOWERED;
    assert_unchanged_failure(CM_C_EMIT_INVALID_MIR, &output, &program,
        &program.body, program.entry_item, &test_target);
    source_body->state = CM_HIR_BODY_TYPED;

    assert(strcmp(cm_c_emit_status_name(CM_C_EMIT_OK), "ok") == 0);
    assert(strcmp(cm_c_emit_status_name((CmCEmitStatus)99),
        "unknown C emit status") == 0);
    cm_str_buf_destroy(&output);
    test_program_destroy(&program);
}

static void compile_and_run_c(const CmStrBuf *source)
{
    char source_path[] = "/tmp/cmrustc-reference-source-XXXXXX";
    char executable_path[] = "/tmp/cmrustc-reference-exe-XXXXXX";
    char command[1024];
    const char *compiler;
    FILE *stream;
    int source_fd;
    int executable_fd;
    int length;
    int status;

    source_fd = mkstemp(source_path);
    assert(source_fd >= 0);
    stream = fdopen(source_fd, "w");
    assert(stream != NULL);
    assert(fwrite(source->data, 1u, source->len, stream) == source->len);
    assert(fclose(stream) == 0);
    executable_fd = mkstemp(executable_path);
    assert(executable_fd >= 0);
    assert(close(executable_fd) == 0);
    assert(unlink(executable_path) == 0);
    compiler = getenv("CC");
    if (compiler == NULL || compiler[0] == '\0') compiler = "cc";
    length = snprintf(command, sizeof(command),
        "%s -std=c99 -pedantic -Wall -Wextra -Werror -x c '%s' -o '%s'",
        compiler, source_path, executable_path);
    assert(length > 0 && (size_t)length < sizeof(command));
    status = system(command);
    assert(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    length = snprintf(command, sizeof(command), "'%s'", executable_path);
    assert(length > 0 && (size_t)length < sizeof(command));
    status = system(command);
    assert(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(unlink(executable_path) == 0);
    assert(unlink(source_path) == 0);
}

static void init_reference_export_mir(const TestReferenceProgram *program,
    CmMirBody *body, CmMirLocal locals[3], CmMirStatement *statement,
    CmMirBasicBlock *block)
{
    memset(locals, 0, 3u * sizeof(*locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = program->u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = program->shared_u32_type;
    locals[2].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[2].type = program->u32_type;
    memset(statement, 0, sizeof(*statement));
    statement->kind = CM_MIR_STATEMENT_ASSIGN;
    statement->data.assign.destination = CM_MIR_RETURN_LOCAL;
    statement->data.assign.value.kind = CM_MIR_RVALUE_USE;
    statement->data.assign.value.type = program->u32_type;
    statement->data.assign.value.data.use.kind = CM_MIR_OPERAND_MOVE;
    statement->data.assign.value.data.use.type = program->u32_type;
    statement->data.assign.value.data.use.data.local = 2u;
    memset(block, 0, sizeof(*block));
    block->statements = statement;
    block->statement_count = 1u;
    block->terminator.kind = CM_MIR_TERMINATOR_RETURN;
    memset(body, 0, sizeof(*body));
    body->instance.definition = program->export_definition;
    body->instance.body_definition = program->export_definition;
    body->owner = program->export_definition;
    body->source_body = program->export_body;
    body->locals = locals;
    body->local_count = 3u;
    body->basic_blocks = block;
    body->basic_block_count = 1u;
}

static void test_reference_export_and_rejection(void)
{
    TestReferenceProgram program;
    CmMirContext mir;
    CmMirBody body;
    CmMirLocal locals[3];
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirBodyId root;
    CmMirBodyId rejected;
    CmMirBodyId roots[1];
    CmStrBuf output;
    CmHirItem *item;
    CmHirBody *source_body;
    CmHirTypeId old_parameter;
    CmHirTypeId old_local;
    CmHirTypeId rejected_types[4];
    uint32_t index;

    reference_program_init(&program);
    init_reference_export_mir(&program, &body, locals, &statement, &block);
    cm_mir_context_init(&mir);
    root = CM_MIR_BODY_NONE;
    assert(cm_mir_add_monomorphized_body(&mir, &program.hir, &body, &root)
        == CM_MIR_INVARIANT_VIOLATION);
    assert(root == CM_MIR_BODY_NONE);
    roots[0] = 1u;
    cm_str_buf_init(&output);
    cm_str_buf_append(&output, "sentinel");
    assert(cm_c_emit_reachable_program(&output, &program.hir, &mir, NULL,
        roots,
        1u, &test_target) == CM_C_EMIT_INVALID_MIR);
    assert(strcmp(cm_str_buf_c_str(&output), "sentinel") == 0);

    item = (CmHirItem *)cm_hir_get_item(&program.hir, 3u);
    source_body = (CmHirBody *)cm_hir_get_body(&program.hir,
        program.export_body);
    assert(item != NULL && item->definition.index
        == program.export_definition.index && source_body != NULL);
    old_parameter = item->data.function_item.signature.parameters[0].type;
    old_local = source_body->locals[0].type;
    rejected_types[0] = program.mutable_u32_type;
    rejected_types[1] = program.static_u32_type;
    rejected_types[2] = program.raw_u32_type;
    rejected_types[3] = program.shared_dyn_trait_type;
    for (index = 0u; index < 4u; ++index) {
        assert(!cm_c_type_is_supported(&program.hir,
            rejected_types[index]));
        item->data.function_item.signature.parameters[0].type =
            rejected_types[index];
        source_body->locals[0].type = rejected_types[index];
        body.locals[1].type = rejected_types[index];
        assert(cm_mir_add_monomorphized_body(&mir, &program.hir, &body,
            &rejected) == CM_MIR_INVARIANT_VIOLATION);
        assert(rejected == CM_MIR_BODY_NONE);
    }
    item->data.function_item.signature.parameters[0].type = old_parameter;
    source_body->locals[0].type = old_local;
    body.locals[1].type = old_local;

    body.basic_blocks[0].statements[0].data.assign.destination = 1u;
    body.basic_blocks[0].statements[0].data.assign.value.kind =
        CM_MIR_RVALUE_BORROW;
    body.basic_blocks[0].statements[0].data.assign.value.type =
        program.shared_u32_type;
    body.basic_blocks[0].statements[0].data.assign.value.span =
        test_span(108u, 112u);
    body.basic_blocks[0].statements[0].data.assign.value.data.borrow.kind =
        CM_MIR_BORROW_SHARED;
    body.basic_blocks[0].statements[0].data.assign.value.data.borrow.source
        .base = 2u;
    body.basic_blocks[0].statements[0].data.assign.value.data.borrow.source
        .type = program.u32_type;
    body.basic_blocks[0].statements[0].data.assign.value.data.borrow.source
        .span = test_span(109u, 110u);
    assert(cm_mir_validate_rvalue(&program.hir, &body,
        &body.basic_blocks[0].statements[0].data.assign.value, 64u)
        == CM_MIR_OK);
    assert(cm_mir_add_monomorphized_body(&mir, &program.hir, &body,
        &rejected) == CM_MIR_INVARIANT_VIOLATION);
    assert(rejected == CM_MIR_BODY_NONE);

    cm_str_buf_destroy(&output);
    cm_mir_context_destroy(&mir);
    reference_program_destroy(&program);
}

static void test_reference_formatter_shapes(void)
{
    TestReferenceProgram program;
    CmMirBody body;
    CmMirLocal locals[2];
    CmMirPlaceProjection projections[2];
    CmMirPlace field_place;
    CmMirPlace dereference_place;
    CmMirRvalue borrow;
    CmStrBuf text;
    CmStrBuf source;
    char struct_name[CM_C_EXACT_NAME_CAPACITY];

    reference_program_init(&program);
    memset(locals, 0, sizeof(locals));
    locals[0].kind = CM_MIR_LOCAL_RETURN;
    locals[0].type = program.u32_type;
    locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
    locals[1].type = program.shared_pair_type;
    memset(&body, 0, sizeof(body));
    body.instance.definition = program.field_definition;
    body.instance.body_definition = program.field_definition;
    body.owner = program.field_definition;
    body.source_body = program.field_body;
    body.locals = locals;
    body.local_count = 2u;
    cm_str_buf_init(&text);
    cm_c_append_parameters(&text, &program.hir, &body, 1u);
    assert(strstr(cm_str_buf_c_str(&text), "struct cmrustc_struct_c")
            == cm_str_buf_c_str(&text)
        && strstr(cm_str_buf_c_str(&text), " const * _1") != NULL);
    cm_str_buf_clear(&text);
    memset(projections, 0, sizeof(projections));
    projections[0].kind = CM_MIR_PROJECTION_DEREFERENCE;
    projections[1].kind = CM_MIR_PROJECTION_FIELD;
    projections[1].definition = program.pair_definition;
    projections[1].field_index = 0u;
    memset(&field_place, 0, sizeof(field_place));
    field_place.base = 1u;
    field_place.type = program.u32_type;
    field_place.projections = projections;
    field_place.projection_count = 2u;
    field_place.span = test_span(168u, 172u);
    assert(cm_mir_validate_place(&program.hir, &body, &field_place)
        == CM_MIR_OK);
    dereference_place = field_place;
    dereference_place.base = 3u;
    dereference_place.projections = projections;
    dereference_place.projection_count = 1u;

    memset(&borrow, 0, sizeof(borrow));
    borrow.kind = CM_MIR_RVALUE_BORROW;
    borrow.type = program.shared_u32_type;
    borrow.span = test_span(167u, 174u);
    borrow.data.borrow.kind = CM_MIR_BORROW_SHARED;
    borrow.data.borrow.source = field_place;
    assert(cm_mir_validate_rvalue(&program.hir, &body, &borrow, 64u)
        == CM_MIR_OK);
    cm_c_append_type(&text, &program.hir, program.shared_u32_type);
    assert(strcmp(cm_str_buf_c_str(&text), "uint32_t const *") == 0);
    cm_str_buf_clear(&text);
    cm_c_append_place(&text, &field_place);
    assert(strcmp(cm_str_buf_c_str(&text), "(*(_1))._f0") == 0);
    cm_str_buf_clear(&text);
    cm_c_append_rvalue(&text, &program.hir, &borrow, 64u);
    assert(strcmp(cm_str_buf_c_str(&text), "&((*(_1))._f0)") == 0);

    assert(cm_c_struct_name(program.pair_definition, struct_name,
        sizeof(struct_name)));
    cm_str_buf_init(&source);
    cm_str_buf_append(&source, "#include <stdint.h>\nstruct ");
    cm_str_buf_append(&source, struct_name);
    cm_str_buf_append(&source, " { uint32_t _f0; };\nstatic uint32_t probe(");
    cm_c_append_type(&source, &program.hir, program.shared_u32_type);
    cm_str_buf_append(&source, " _unused, ");
    cm_c_append_type(&source, &program.hir, program.shared_pair_type);
    cm_str_buf_append(&source,
        " _1)\n{\n    uint32_t const * _3;\n    (void)_unused;\n    _3 = ");
    cm_c_append_rvalue(&source, &program.hir, &borrow, 64u);
    cm_str_buf_append(&source, ";\n    return ");
    cm_c_append_place(&source, &dereference_place);
    cm_str_buf_append(&source,
        ";\n}\nint main(void)\n{\n    uint32_t value = 3u;\n    struct ");
    cm_str_buf_append(&source, struct_name);
    cm_str_buf_append(&source,
        " pair = { 9u };\n    return probe(&value, &pair) == 9u ? 0 : 1;\n}\n");
    compile_and_run_c(&source);
    cm_str_buf_destroy(&source);
    cm_str_buf_destroy(&text);
    reference_program_destroy(&program);
}

static void test_canonical_instance_identity_and_name(void)
{
    TestReferenceProgram program;
    CmMirInstance first;
    CmMirInstance same;
    CmMirInstance distinct;
    CmMirInstance flat;
    CmMirBody body;
    CmMirLocal materialized_local;
    CmMirLocal scalar_locals[2];
    CmMirRvalue u8_binary;
    const CmHirItem *item;
    unsigned char first_bytes[3] = { 1u, 2u, 3u };
    unsigned char same_bytes[3] = { 1u, 2u, 3u };
    unsigned char distinct_bytes[3] = { 1u, 2u, 4u };
    CmHirTypeId first_substitution;
    CmHirTypeId same_substitution;
    char first_name[CM_C_EXACT_NAME_CAPACITY];
    char same_name[CM_C_EXACT_NAME_CAPACITY];
    char distinct_name[CM_C_EXACT_NAME_CAPACITY];

    reference_program_init(&program);
    memset(&first, 0, sizeof(first));
    first.definition = program.field_definition;
    first.body_definition = program.field_definition;
    first.body = program.field_body;
    first.identity_bytes = first_bytes;
    first.identity_size = sizeof(first_bytes);
    same = first;
    same.identity_bytes = same_bytes;
    first_substitution = program.u32_type;
    same_substitution = program.u8_type;
    first.substitutions = &first_substitution;
    first.substitution_count = 1u;
    same.substitutions = &same_substitution;
    same.substitution_count = 1u;
    distinct = first;
    distinct.identity_bytes = distinct_bytes;
    flat.definition = first.definition;
    assert(cm_c_instance_equal(&first, &same)
        && !cm_c_instance_equal(&first, &distinct)
        && !cm_c_instance_equal(&first, &flat));
    distinct = first;
    distinct.body += 1u;
    assert(!cm_c_instance_equal(&first, &distinct));

    memset(&body, 0, sizeof(body));
    memset(&materialized_local, 0, sizeof(materialized_local));
    materialized_local.kind = CM_MIR_LOCAL_RETURN;
    materialized_local.type = program.u32_type;
    body.instance = first;
    body.owner = first.definition;
    body.source_body = first.body;
    body.locals = &materialized_local;
    body.local_count = 1u;
    item = cm_c_instance_item(&program.hir, &body);
    assert(item != NULL
        && cm_c_instance_materialization_valid(&program.hir, item, &body, 1)
        && !cm_c_instance_materialization_valid(&program.hir, item, &body, 0));
    body.instance.substitutions = &same_substitution;
    assert(!cm_c_instance_materialization_valid(&program.hir, item, &body, 1));
    materialized_local.type = program.u8_type;
    assert(cm_c_instance_materialization_valid(&program.hir, item, &body, 1));
    same_substitution = program.pair_type;
    assert(!cm_c_instance_materialization_valid(&program.hir, item, &body, 1));
    same_substitution = program.u8_type;
    body.instance.identity_bytes = NULL;
    assert(!cm_c_instance_materialization_valid(&program.hir, item, &body, 1));
    body.instance = first;
    assert(cm_c_exact_name(&program.hir, &body, 0, first_name,
        sizeof(first_name)));
    body.instance = same;
    assert(cm_c_exact_name(&program.hir, &body, 0, same_name,
        sizeof(same_name)));
    body.instance.identity_bytes = distinct_bytes;
    assert(cm_c_exact_name(&program.hir, &body, 0, distinct_name,
        sizeof(distinct_name))
        && strcmp(first_name, same_name) == 0
        && strcmp(first_name, distinct_name) != 0
        && strncmp(first_name, "cmrustc_h", 9u) == 0
        && strstr(first_name, "_t") == NULL
        && strlen(first_name) < CM_C_EXACT_NAME_CAPACITY);
    {
        CmStrBuf type_text;

        cm_str_buf_init(&type_text);
        cm_c_append_type(&type_text, &program.hir, program.u8_type);
        assert(strcmp(cm_str_buf_c_str(&type_text), "uint8_t") == 0);
        cm_str_buf_clear(&type_text);
        memset(scalar_locals, 0, sizeof(scalar_locals));
        scalar_locals[0].kind = CM_MIR_LOCAL_RETURN;
        scalar_locals[0].type = program.u8_type;
        scalar_locals[1].kind = CM_MIR_LOCAL_ARGUMENT;
        scalar_locals[1].type = program.u8_type;
        memset(&body, 0, sizeof(body));
        body.locals = scalar_locals;
        body.local_count = 2u;
        memset(&u8_binary, 0, sizeof(u8_binary));
        u8_binary.kind = CM_MIR_RVALUE_BINARY;
        u8_binary.type = program.u8_type;
        u8_binary.data.binary.operator_kind = CM_MIR_BINARY_ADD;
        u8_binary.data.binary.left.kind = CM_MIR_OPERAND_MOVE;
        u8_binary.data.binary.left.type = program.u8_type;
        u8_binary.data.binary.left.data.local = 1u;
        u8_binary.data.binary.right = u8_binary.data.binary.left;
        assert(cm_c_exact_rvalue_shape(&program.hir, &body, &u8_binary,
                64u));
        cm_c_append_rvalue(&type_text, &program.hir, &u8_binary, 64u);
        assert(strcmp(cm_str_buf_c_str(&type_text),
            "(uint8_t)(_1 + _1)") == 0);
        cm_str_buf_destroy(&type_text);
    }
    reference_program_destroy(&program);
}

int main(void)
{
    test_exact_output_and_determinism();
    test_rejection_and_rollback();
    test_reference_export_and_rejection();
    test_reference_formatter_shapes();
    test_canonical_instance_identity_and_name();
    return 0;
}
