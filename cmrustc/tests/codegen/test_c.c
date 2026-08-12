#include "cm/codegen/c.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct TestProgram {
    CmHirContext hir;
    CmHirItemId entry_item;
    CmHirTypeId i32_type;
    CmMirLocal local;
    CmMirStatement statement;
    CmMirBasicBlock block;
    CmMirBody body;
} TestProgram;

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

int main(void)
{
    test_exact_output_and_determinism();
    test_rejection_and_rollback();
    return 0;
}
