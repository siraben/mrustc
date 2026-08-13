#include "cm/codegen/c.h"

#include "cm/alloc.h"
#include "cm/hir/layout.h"
#include "cm/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int cm_c_text_equal(const CmHirContext *hir, CmInternId id,
    const char *expected)
{
    const CmInternedString *text;
    size_t length;

    if (hir == NULL || expected == NULL) return 0;
    text = cm_interner_get(&hir->strings, id);
    length = strlen(expected);
    return text != NULL && text->len == length
        && memcmp(text->bytes, expected, length) == 0;
}

static int cm_c_target_is_supported(const CmTargetDesc *target)
{
    if (target == NULL || target->triple == NULL
        || target->architecture == NULL
        || target->operating_system == NULL
        || target->environment == NULL
        || target->endian != CM_ENDIAN_LITTLE) {
        return 0;
    }
    if (strcmp(target->operating_system, "linux") != 0) return 0;
    if (strcmp(target->triple, "i386-unknown-linux-musl") == 0
        || strcmp(target->triple, "i686-unknown-linux-musl") == 0) {
        return strcmp(target->architecture, "x86") == 0
            && strcmp(target->environment, "musl") == 0
            && target->pointer_bits == 32u;
    }
    if (strcmp(target->triple, "x86_64-unknown-linux-gnu") == 0) {
        return strcmp(target->architecture, "x86_64") == 0
            && strcmp(target->environment, "gnu") == 0
            && target->pointer_bits == 64u;
    }
    if (strcmp(target->triple, "x86_64-unknown-linux-musl") == 0) {
        return strcmp(target->architecture, "x86_64") == 0
            && strcmp(target->environment, "musl") == 0
            && target->pointer_bits == 64u;
    }
    return 0;
}

static int cm_c_crate_envelope_valid(const CmHirContext *hir,
    const CmHirCrate *crate_value)
{
    int saw_feature;
    int saw_no_core;
    int saw_no_main;
    uint32_t index;

    if (crate_value == NULL || crate_value->inner_attribute_count != 3u
        || crate_value->inner_attributes == NULL) {
        return 0;
    }
    saw_feature = 0;
    saw_no_core = 0;
    saw_no_main = 0;
    for (index = 0u; index < crate_value->inner_attribute_count; ++index) {
        CmInternId metadata;

        metadata = crate_value->inner_attributes[index].metadata;
        if (cm_c_text_equal(hir, metadata, "feature(no_core)")) {
            if (saw_feature) return 0;
            saw_feature = 1;
        } else if (cm_c_text_equal(hir, metadata, "no_core")) {
            if (saw_no_core) return 0;
            saw_no_core = 1;
        } else if (cm_c_text_equal(hir, metadata, "no_main")) {
            if (saw_no_main) return 0;
            saw_no_main = 1;
        } else {
            return 0;
        }
    }
    return saw_feature && saw_no_core && saw_no_main;
}

static CmCEmitStatus cm_c_preflight_entry(const CmHirContext *hir,
    CmHirItemId entry_item, const CmHirItem **out_item,
    CmHirTypeId *out_i32_type)
{
    const CmHirItem *item;
    const CmHirDefinition *definition;
    const CmHirModule *module;
    const CmHirCrate *crate_value;
    const CmHirFunctionSignature *signature;
    const CmHirType *return_type;

    item = cm_hir_get_item(hir, entry_item);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || cm_hir_def_id_is_none(item->definition)) {
        return CM_C_EMIT_INVALID_ENTRY;
    }
    definition = cm_hir_lookup_definition(hir, item->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->entity.item_id != entry_item) {
        return CM_C_EMIT_INVALID_ENTRY;
    }
    module = cm_hir_get_module(hir, item->owner_module);
    crate_value = module == NULL ? NULL
        : cm_hir_get_crate(hir, module->crate_id);
    if (module == NULL || crate_value == NULL
        || hir->crates.len != 1u || hir->modules.len != 1u
        || hir->items.len != 1u || hir->bodies.len != 1u
        || hir->expressions.len != 2u || entry_item != 1u
        || crate_value->root_module != item->owner_module
        || module->parent != CM_HIR_MODULE_NONE
        || module->import_count != 0u
        || module->crate_id != item->definition.crate_id
        || !cm_c_crate_envelope_valid(hir, crate_value)) {
        return CM_C_EMIT_UNSUPPORTED_ENTRY;
    }
    signature = &item->data.function_item.signature;
    return_type = cm_hir_get_type(hir, signature->return_type);
    if (return_type == NULL
        || return_type->kind != CM_HIR_TYPE_INTEGER_KIND
        || return_type->data.integer_type.kind != CM_HIR_INT_I32) {
        return CM_C_EMIT_UNSUPPORTED_ENTRY;
    }
    if (!cm_hir_def_id_is_none(item->parent_definition)
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || item->generic_parameter_count != 0u
        || item->predicate_count != 0u
        || signature->parameter_count != 0u
        || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE
        || signature->is_const
        || signature->is_async
        || signature->is_variadic
        || !cm_c_text_equal(hir, signature->abi, "C")
        || !cm_c_text_equal(hir, item->name, "main")
        || item->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || item->attribute_count != 1u
        || item->attributes == NULL
        || !cm_c_text_equal(hir, item->attributes[0].metadata,
            "no_mangle")
        || item->data.function_item.body == CM_HIR_BODY_NONE) {
        return CM_C_EMIT_UNSUPPORTED_ENTRY;
    }
    *out_item = item;
    *out_i32_type = signature->return_type;
    return CM_C_EMIT_OK;
}

static CmCEmitStatus cm_c_preflight_mir(const CmHirItem *item,
    const CmHirContext *hir, CmHirTypeId i32_type,
    const CmMirBody *body, int32_t *out_value)
{
    const CmHirBody *source_body;
    const CmMirLocal *return_local;
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;

    if (body == NULL) return CM_C_EMIT_INVALID_ARGUMENT;
    source_body = cm_hir_get_body(hir, body->source_body);
    if (!cm_hir_def_id_equal(body->owner, item->definition)
        || body->source_body != item->data.function_item.body
        || source_body == NULL
        || !cm_hir_def_id_equal(source_body->owner, item->definition)
        || source_body->state != CM_HIR_BODY_TYPED
        || source_body->expected_type != i32_type
        || source_body->parameter_count != 0u
        || source_body->local_count != 0u
        || body->local_count != 1u || body->locals == NULL
        || body->basic_block_count != 1u
        || body->basic_blocks == NULL) {
        return CM_C_EMIT_INVALID_MIR;
    }
    return_local = &body->locals[CM_MIR_RETURN_LOCAL];
    block = &body->basic_blocks[0];
    if (return_local->kind != CM_MIR_LOCAL_RETURN
        || return_local->type != i32_type
        || block->statement_count != 1u || block->statements == NULL
        || block->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
        return CM_C_EMIT_INVALID_MIR;
    }
    statement = &block->statements[0];
    if (statement->kind != CM_MIR_STATEMENT_ASSIGN
        || statement->data.assign.destination != CM_MIR_RETURN_LOCAL
        || statement->data.assign.value.kind != CM_MIR_RVALUE_USE
        || statement->data.assign.value.type != i32_type
        || statement->data.assign.value.data.use.kind
            != CM_MIR_CONSTANT_I32
        || statement->data.assign.value.data.use.type != i32_type) {
        return CM_C_EMIT_INVALID_MIR;
    }
    *out_value = statement->data.assign.value.data.use.data.i32_value;
    return CM_C_EMIT_OK;
}

CmCEmitStatus cm_c_emit_program(CmStrBuf *output,
    const CmHirContext *hir, const CmMirBody *body,
    CmHirItemId entry_item, const CmTargetDesc *target)
{
    static const char prefix[] =
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
        "    _0 = (int32_t)(";
    static const char suffix[] =
        ");\n"
        "    return (int)_0;\n"
        "}\n";
    const CmHirItem *item;
    CmHirTypeId i32_type;
    CmCEmitStatus status;
    int32_t value;
    char literal[32];
    int length;

    if (output == NULL || hir == NULL || body == NULL || target == NULL) {
        return CM_C_EMIT_INVALID_ARGUMENT;
    }
    if (!cm_c_target_is_supported(target)) {
        return CM_C_EMIT_UNSUPPORTED_TARGET;
    }
    status = cm_c_preflight_entry(hir, entry_item, &item, &i32_type);
    if (status != CM_C_EMIT_OK) return status;
    status = cm_c_preflight_mir(item, hir, i32_type, body, &value);
    if (status != CM_C_EMIT_OK) return status;
    length = snprintf(literal, sizeof(literal), "%lld",
        (long long)value);
    if (length <= 0 || (size_t)length >= sizeof(literal)) {
        return CM_C_EMIT_INVALID_MIR;
    }

    /* All fallible validation precedes the first append. */
    cm_str_buf_append_n(output, prefix, sizeof(prefix) - 1u);
    cm_str_buf_append_n(output, literal, (size_t)length);
    cm_str_buf_append_n(output, suffix, sizeof(suffix) - 1u);
    return CM_C_EMIT_OK;
}

#define CM_C_EXACT_NAME_CAPACITY ((size_t)256u)

static int cm_c_item_has_attribute(const CmHirContext *hir,
    const CmHirItem *item, const char *name)
{
    uint32_t index;

    for (index = 0u; index < item->attribute_count; ++index) {
        if (cm_c_text_equal(hir, item->attributes[index].metadata, name)) {
            return 1;
        }
    }
    return 0;
}

static int cm_c_type_is_u32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_c_type_is_i32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32;
}

static int cm_c_type_is_usize(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int cm_c_type_is_codegen_scalar(const CmHirContext *hir,
    CmHirTypeId id)
{
    return cm_c_type_is_u32(hir, id) || cm_c_type_is_usize(hir, id);
}

static int cm_c_type_is_bool(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static const CmHirItem *cm_c_named_struct(const CmHirContext *hir,
    CmHirDefId definition, size_t *out_item_index)
{
    const CmHirDefinition *record;
    const CmHirItem *item;
    size_t item_index;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_equal(item->definition, definition)
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 0u
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || item->data.aggregate_item.field_count == 0u
        || item->data.aggregate_item.fields == NULL
        || item->data.aggregate_item.field_count
            > CM_MIR_MAX_AGGREGATE_FIELDS) {
        return NULL;
    }
    item_index = (size_t)record->entity.item_id - 1u;
    if (item_index >= hir->items.len
        || cm_vec_at_const(&hir->items, item_index) != item) {
        return NULL;
    }
    if (out_item_index != NULL) *out_item_index = item_index;
    return item;
}

static int cm_c_type_is_aggregate(const CmHirContext *hir,
    CmHirTypeId id, CmHirDefId *out_definition)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    if (type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.argument_count != 0u
        || type->data.named_type.arguments != NULL
        || cm_c_named_struct(hir, type->data.named_type.definition,
            NULL) == NULL) {
        return 0;
    }
    if (out_definition != NULL) {
        *out_definition = type->data.named_type.definition;
    }
    return 1;
}

static int cm_c_type_is_supported_inner(const CmHirContext *hir,
    CmHirTypeId id, size_t depth)
{
    const CmHirType *type;

    if (hir == NULL || depth > hir->types.len) return 0;
    type = cm_hir_get_type(hir, id);
    return cm_c_type_is_i32(hir, id) || cm_c_type_is_u32(hir, id)
        || cm_c_type_is_usize(hir, id)
        || cm_c_type_is_aggregate(hir, id, NULL)
        || (type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
            && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && type->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && cm_c_type_is_supported_inner(hir,
                type->data.reference_type.pointee, depth + 1u));
}

static int cm_c_type_is_supported(const CmHirContext *hir, CmHirTypeId id)
{
    return cm_c_type_is_supported_inner(hir, id, 0u);
}

static int cm_c_type_is_reference(const CmHirContext *hir,
    CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && type->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && cm_c_type_is_supported(hir, id);
}

static int cm_c_type_equal_inner(const CmHirContext *hir,
    CmHirTypeId left_id, CmHirTypeId right_id, size_t depth)
{
    const CmHirType *left;
    const CmHirType *right;

    if (hir == NULL || depth > hir->types.len) return 0;
    if (left_id == right_id) {
        return cm_c_type_is_supported(hir, left_id)
            || cm_c_type_is_bool(hir, left_id);
    }
    left = cm_hir_get_type(hir, left_id);
    right = cm_hir_get_type(hir, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    if (left->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return left->data.integer_type.kind == right->data.integer_type.kind;
    }
    if (left->kind == CM_HIR_TYPE_BOOL_KIND) return 1;
    if (left->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return left->data.reference_type.mutability
                == right->data.reference_type.mutability
            && left->data.reference_type.region.kind
                == right->data.reference_type.region.kind
            && left->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && left->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && cm_c_type_equal_inner(hir,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee, depth + 1u);
    }
    return left->kind == CM_HIR_TYPE_ADT_KIND
        && left->data.named_type.argument_count == 0u
        && left->data.named_type.arguments == NULL
        && right->data.named_type.argument_count == 0u
        && right->data.named_type.arguments == NULL
        && cm_hir_def_id_equal(left->data.named_type.definition,
            right->data.named_type.definition);
}

static int cm_c_type_equal(const CmHirContext *hir, CmHirTypeId left_id,
    CmHirTypeId right_id)
{
    return cm_c_type_equal_inner(hir, left_id, right_id, 0u);
}

static int cm_c_place_present(const CmMirPlace *place)
{
    return place != NULL && (place->type != CM_HIR_TYPE_NONE
        || place->projections != NULL || place->projection_count != 0u
        || place->span.source != 0u || place->span.start != 0u
        || place->span.end != 0u);
}

static int cm_c_instance_type_is_u32(const CmHirContext *hir,
    const CmHirItem *item, const CmMirBody *body, CmHirTypeId id)
{
    const CmHirGenericParam *parameter;
    const CmHirType *type;

    if (cm_c_type_is_u32(hir, id)) return 1;
    type = cm_hir_get_type(hir, id);
    if (item == NULL || body == NULL
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || body->instance.substitution_count != 1u
        || body->instance.substitutions == NULL
        || !cm_c_type_is_u32(hir, body->instance.substitutions[0])
        || type == NULL || type->kind != CM_HIR_TYPE_PARAMETER_KIND) {
        return 0;
    }
    parameter = cm_hir_get_generic_param(hir,
        type->data.parameter_type.parameter);
    return parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && type->data.parameter_type.parameter
            == item->generic_parameter_start
        && cm_hir_def_id_equal(parameter->owner, item->definition);
}

static int cm_c_instance_parameter_matches(const CmHirContext *hir,
    const CmHirItem *item, const CmMirBody *body,
    CmHirTypeId actual, CmHirTypeId declared)
{
    if (cm_c_type_is_u32(hir, actual)) {
        return cm_c_instance_type_is_u32(hir, item, body, declared);
    }
    if (cm_c_type_is_usize(hir, actual)) {
        return item != NULL && body != NULL
            && item->generic_parameter_count == 0u
            && body->instance.substitution_count == 0u
            && body->instance.substitutions == NULL
            && cm_c_type_is_usize(hir, declared);
    }
    if (cm_c_type_is_reference(hir, actual)) {
        return item != NULL && body != NULL
            && item->generic_parameter_count == 0u
            && body->instance.substitution_count == 0u
            && body->instance.substitutions == NULL
            && cm_c_type_is_reference(hir, declared)
            && cm_c_type_equal(hir, actual, declared);
    }
    return item != NULL && body != NULL
        && item->generic_parameter_count == 0u
        && cm_c_text_equal(hir,
            item->data.function_item.signature.abi, "Rust")
        && body->instance.substitution_count == 0u
        && body->instance.substitutions == NULL
        && cm_c_type_is_supported(hir, actual)
        && cm_c_type_is_supported(hir, declared)
        && cm_c_type_equal(hir, actual, declared);
}

static const CmHirItem *cm_c_instance_item(const CmHirContext *hir,
    const CmMirBody *body)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    if (body == NULL || cm_hir_def_id_is_none(body->instance.definition)
        || !cm_hir_def_id_equal(body->owner,
            body->instance.definition)) {
        return NULL;
    }
    definition = cm_hir_lookup_definition(hir, body->instance.definition);
    item = definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND ? NULL
        : cm_hir_get_item(hir, definition->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_equal(item->definition, body->owner)
        && item->data.function_item.body == body->source_body
        ? item : NULL;
}

static int cm_c_body_parameter_count(const CmHirContext *hir,
    const CmMirContext *mir, const CmSemanticAdmission *admission,
    CmMirBodyId body_id, uint32_t *out_count)
{
    const CmMirBody *body;
    const CmHirItem *item;
    CmSemanticFunctionSignatureView semantic_signature;

    if (out_count == NULL) return 0;
    *out_count = 0u;
    body = cm_mir_get_body(mir, body_id);
    if (body == NULL) return 0;
    if (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        memset(&semantic_signature, 0, sizeof(semantic_signature));
        if (cm_mir_admitted_signature(mir, admission, body_id,
                &semantic_signature) != CM_MIR_OK) return 0;
        *out_count = semantic_signature.parameter_count;
        return 1;
    }
    if (body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_NONE) return 0;
    item = cm_c_instance_item(hir, body);
    if (item == NULL) return 0;
    *out_count = item->data.function_item.signature.parameter_count;
    return 1;
}

static int cm_c_interned_identifier(const CmHirContext *hir, CmInternId id,
    const CmInternedString **out_text)
{
    const CmInternedString *text;
    size_t index;

    text = cm_interner_get(&hir->strings, id);
    if (text == NULL || text->len == 0u || text->len > 128u) return 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned char byte;

        byte = (unsigned char)text->bytes[index];
        if (!((byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
                || (byte >= (unsigned char)'A'
                    && byte <= (unsigned char)'Z')
                || byte == (unsigned char)'_'
                || (index != 0u && byte >= (unsigned char)'0'
                    && byte <= (unsigned char)'9'))) {
            return 0;
        }
    }
    *out_text = text;
    return 1;
}

static int cm_c_identifier_is_keyword(const char *name)
{
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue",
        "default", "do", "double", "else", "enum", "extern", "float",
        "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "_Bool", "_Complex", "_Imaginary"
    };
    size_t index;

    for (index = 0u; index < sizeof(keywords) / sizeof(keywords[0]);
         ++index) {
        if (strcmp(name, keywords[index]) == 0) return 1;
    }
    return 0;
}

static int cm_c_exact_operand_shape(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand)
{
    CmMirLocalId local;

    if (operand == NULL
        || (!cm_c_type_is_supported(hir, operand->type)
            && !cm_c_type_is_bool(hir, operand->type))) {
        return 0;
    }
    if (operand->kind == CM_MIR_CONSTANT_I32) {
        return cm_c_type_is_i32(hir, operand->type);
    }
    if (operand->kind == CM_MIR_CONSTANT_U32) {
        return cm_c_type_is_u32(hir, operand->type);
    }
    if (operand->kind == CM_MIR_CONSTANT_USIZE) {
        return cm_c_type_is_usize(hir, operand->type);
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        local = operand->data.local;
        if (local == CM_MIR_RETURN_LOCAL || local >= body->local_count
            || !cm_c_type_equal(hir, body->locals[local].type,
                operand->type)) {
            return 0;
        }
        return body->locals[local].kind == CM_MIR_LOCAL_ARGUMENT
            || body->locals[local].kind == CM_MIR_LOCAL_USER
            || body->locals[local].kind == CM_MIR_LOCAL_TEMPORARY;
    }
    if (operand->kind != CM_MIR_OPERAND_COPY_PLACE
        && operand->kind != CM_MIR_OPERAND_MOVE_PLACE) {
        return 0;
    }
    if (cm_mir_validate_place(hir, body, &operand->data.place)
            != CM_MIR_OK
        || !cm_c_type_equal(hir, operand->type,
            operand->data.place.type)) {
        return 0;
    }
    return operand->kind == CM_MIR_OPERAND_COPY_PLACE
        ? (cm_c_type_is_i32(hir, operand->type)
            || cm_c_type_is_u32(hir, operand->type)
            || cm_c_type_is_usize(hir, operand->type)
            || cm_c_type_is_reference(hir, operand->type))
        : (cm_c_type_is_aggregate(hir, operand->type, NULL)
            || cm_c_type_is_reference(hir, operand->type));
}

static int cm_c_exact_rvalue_shape(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    uint32_t index;

    if (rvalue == NULL) {
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        return cm_c_type_is_bool(hir, rvalue->type)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.equal.left)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.equal.right)
            && cm_c_type_is_u32(hir, rvalue->data.equal.left.type)
            && cm_c_type_is_u32(hir, rvalue->data.equal.right.type);
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        return cm_c_type_is_bool(hir, rvalue->type)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.less.left)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.less.right)
            && cm_c_type_is_usize(hir, rvalue->data.less.left.type)
            && cm_c_type_is_usize(hir, rvalue->data.less.right.type);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BORROW) {
        return cm_c_type_is_reference(hir, rvalue->type)
            && cm_mir_validate_rvalue(hir, body, rvalue,
                pointer_bits) == CM_MIR_OK;
    }
    if (!cm_c_type_is_supported(hir, rvalue->type)) return 0;
    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        return cm_c_exact_operand_shape(hir, body, &rvalue->data.use)
            && cm_c_type_equal(hir, rvalue->type,
                rvalue->data.use.type);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY) {
        return cm_c_type_is_codegen_scalar(hir, rvalue->type)
            && (rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
                || rvalue->data.binary.operator_kind
                    == CM_MIR_BINARY_SUBTRACT)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.binary.left)
            && cm_c_exact_operand_shape(hir, body,
                &rvalue->data.binary.right)
            && cm_c_type_equal(hir, rvalue->type,
                rvalue->data.binary.left.type)
            && cm_c_type_equal(hir, rvalue->type,
                rvalue->data.binary.right.type);
    }
    if (rvalue->kind != CM_MIR_RVALUE_AGGREGATE
        || !cm_c_type_is_aggregate(hir, rvalue->type, NULL)
        || rvalue->data.aggregate.field_count == 0u
        || rvalue->data.aggregate.field_count
            > CM_MIR_MAX_AGGREGATE_FIELDS
        || rvalue->data.aggregate.fields == NULL) {
        return 0;
    }
    for (index = 0u; index < rvalue->data.aggregate.field_count; ++index) {
        if (rvalue->data.aggregate.fields[index].field_index != index
            || !cm_c_exact_operand_shape(hir, body,
                &rvalue->data.aggregate.fields[index].value)) {
            return 0;
        }
    }
    return 1;
}

/*
 * The first control-flow boundary deliberately admits one graph, not a
 * general CFG: one exact comparison ends bb0, true and false assignments end
 * bb1/bb2, and one empty return join is bb3. Earlier statements in those
 * three blocks retain authenticated arithmetic-tree evaluation order. The
 * bool exists only as the exact temporary consumed by the switch, and both
 * arms assign the scalar return destination.
 */
static int cm_c_exact_if_diamond(const CmHirContext *hir,
    const CmMirBody *body, CmMirLocalId *out_bool_local)
{
    const CmMirBasicBlock *entry;
    const CmMirBasicBlock *then_block;
    const CmMirBasicBlock *else_block;
    const CmMirBasicBlock *join;
    const CmMirStatement *condition;
    const CmMirStatement *then_statement;
    const CmMirStatement *else_statement;
    CmMirLocalId bool_local;

    if (body == NULL || body->basic_block_count != 4u
        || body->basic_blocks == NULL) {
        return 0;
    }
    entry = &body->basic_blocks[0];
    then_block = &body->basic_blocks[1];
    else_block = &body->basic_blocks[2];
    join = &body->basic_blocks[3];
    if (entry->statement_count == 0u || entry->statements == NULL
        || then_block->statement_count == 0u
        || then_block->statements == NULL
        || else_block->statement_count == 0u
        || else_block->statements == NULL
        || join->statement_count != 0u || join->statements != NULL
        || entry->terminator.kind != CM_MIR_TERMINATOR_SWITCH_BOOL
        || entry->terminator.data.switch_bool.true_target != 1u
        || entry->terminator.data.switch_bool.false_target != 2u
        || then_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
        || then_block->terminator.data.goto_block.target != 3u
        || else_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
        || else_block->terminator.data.goto_block.target != 3u
        || join->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
        return 0;
    }

    condition = &entry->statements[entry->statement_count - 1u];
    if (condition->kind != CM_MIR_STATEMENT_ASSIGN
        || (condition->data.assign.value.kind != CM_MIR_RVALUE_EQUAL
            && condition->data.assign.value.kind != CM_MIR_RVALUE_LESS)
        || cm_c_place_present(&condition->data.assign.destination_place)) {
        return 0;
    }
    bool_local = condition->data.assign.destination;
    if (bool_local == CM_MIR_RETURN_LOCAL || bool_local >= body->local_count
        || body->locals[bool_local].kind != CM_MIR_LOCAL_TEMPORARY
        || !cm_c_type_is_bool(hir, body->locals[bool_local].type)
        || !cm_c_type_is_bool(hir, condition->data.assign.value.type)
        || entry->terminator.data.switch_bool.condition.kind
            != CM_MIR_OPERAND_MOVE
        || entry->terminator.data.switch_bool.condition.data.local
            != bool_local
        || !cm_c_type_is_bool(hir,
            entry->terminator.data.switch_bool.condition.type)) {
        return 0;
    }

    then_statement = &then_block->statements[
        then_block->statement_count - 1u];
    else_statement = &else_block->statements[
        else_block->statement_count - 1u];
    if (then_statement->kind != CM_MIR_STATEMENT_ASSIGN
        || else_statement->kind != CM_MIR_STATEMENT_ASSIGN
        || then_statement->data.assign.destination != CM_MIR_RETURN_LOCAL
        || else_statement->data.assign.destination != CM_MIR_RETURN_LOCAL
        || cm_c_place_present(
            &then_statement->data.assign.destination_place)
        || cm_c_place_present(
            &else_statement->data.assign.destination_place)
        || !cm_c_type_is_codegen_scalar(hir,
            body->locals[CM_MIR_RETURN_LOCAL].type)
        || !cm_c_type_equal(hir,
            body->locals[CM_MIR_RETURN_LOCAL].type,
            then_statement->data.assign.value.type)
        || !cm_c_type_equal(hir,
            body->locals[CM_MIR_RETURN_LOCAL].type,
            else_statement->data.assign.value.type)
        || then_statement->data.assign.value.kind == CM_MIR_RVALUE_EQUAL
        || then_statement->data.assign.value.kind == CM_MIR_RVALUE_LESS
        || else_statement->data.assign.value.kind == CM_MIR_RVALUE_EQUAL
        || else_statement->data.assign.value.kind == CM_MIR_RVALUE_LESS) {
        return 0;
    }
    if (out_bool_local != NULL) *out_bool_local = bool_local;
    return 1;
}

static int cm_c_instance_equal(const CmMirInstance *left,
    const CmMirInstance *right)
{
    uint32_t index;
    int left_canonical;
    int right_canonical;

    left_canonical = left->body != CM_HIR_BODY_NONE
        && left->identity_bytes != NULL && left->identity_size != 0u;
    right_canonical = right->body != CM_HIR_BODY_NONE
        && right->identity_bytes != NULL && right->identity_size != 0u;
    if (!cm_hir_def_id_equal(left->definition, right->definition)) {
        return 0;
    }
    if (left_canonical || right_canonical) {
        return left_canonical && right_canonical
            && left->body == right->body
            && left->identity_size == right->identity_size
            && memcmp(left->identity_bytes, right->identity_bytes,
                left->identity_size) == 0;
    }
    if (left->substitution_count != right->substitution_count
        || (left->substitution_count == 0u
            && (left->substitutions != NULL
                || right->substitutions != NULL))
        || (left->substitution_count != 0u
            && (left->substitutions == NULL
                || right->substitutions == NULL))) {
        return 0;
    }
    for (index = 0u; index < left->substitution_count; ++index) {
        if (left->substitutions[index] != right->substitutions[index]) {
            return 0;
        }
    }
    return 1;
}

static void cm_c_sha256_u32(CmSha256 *context, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8u);
    bytes[2] = (unsigned char)(value >> 16u);
    bytes[3] = (unsigned char)(value >> 24u);
    cm_sha256_update(context, bytes, sizeof(bytes));
}

static void cm_c_sha256_u64(CmSha256 *context, uint64_t value)
{
    unsigned char bytes[8];
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (unsigned char)(value >> (index * 8u));
    }
    cm_sha256_update(context, bytes, sizeof(bytes));
}

static void cm_c_instance_digest(const CmMirInstance *instance,
    unsigned char digest[CM_SHA256_DIGEST_SIZE])
{
    static const unsigned char domain[] = "cmrustc-c-symbol-v1";
    CmSha256 context;

    cm_sha256_init(&context);
    cm_sha256_update(&context, domain, sizeof(domain));
    cm_c_sha256_u32(&context, instance->definition.crate_id);
    cm_c_sha256_u32(&context, instance->definition.index);
    cm_c_sha256_u32(&context, instance->body);
    cm_c_sha256_u64(&context, (uint64_t)instance->identity_size);
    cm_sha256_update(&context, instance->identity_bytes,
        instance->identity_size);
    cm_sha256_final(&context, digest);
}

static int cm_c_exact_body_shape(const CmHirContext *hir,
    const CmMirContext *mir, const CmSemanticAdmission *admission,
    CmMirBodyId body_id)
{
    const CmSemanticResults *semantic_results;
    const CmMirBody *body;
    const CmHirBody *source_body;
    const CmHirItem *item;
    const CmHirFunctionSignature *signature;
    CmSemanticFunctionSignatureView semantic_signature;
    CmSemanticTypeView semantic_parameter;
    uint32_t block_index;
    uint32_t first_temporary;
    uint32_t local_index;
    uint32_t parameter_count;
    uint32_t parameter_index;
    CmMirLocalId diamond_bool_local;
    int admitted;
    int type_matches;
    int is_if_diamond;

    body = cm_mir_get_body(mir, body_id);
    admitted = body != NULL
        && body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE;
    memset(&semantic_signature, 0, sizeof(semantic_signature));
    semantic_signature.definition = cm_hir_def_id_none();
    semantic_results = admitted && admission != NULL
        ? cm_semantic_admission_results(admission) : NULL;
    if (body == NULL
        || (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_NONE
            ? cm_mir_validate_monomorphized_body(mir, hir, body_id)
                != CM_MIR_OK
            : !admitted
                || admission == NULL
                || cm_mir_validate_admitted_monomorphized_body(mir,
                    admission, body_id) != CM_MIR_OK
                || cm_mir_admitted_signature(mir, admission, body_id,
                    &semantic_signature) != CM_MIR_OK)) {
        return 0;
    }
    item = cm_c_instance_item(hir, body);
    signature = item == NULL ? NULL : &item->data.function_item.signature;
    source_body = body == NULL ? NULL
        : cm_hir_get_body(hir, body->source_body);
    parameter_count = admitted ? semantic_signature.parameter_count
                               : signature == NULL ? 0u
                                                   : signature->parameter_count;
    type_matches = 0;
    if (admitted && (semantic_results == NULL
        || cm_semantic_type_view_matches_monomorphic_hir(semantic_results,
            admission, &semantic_signature.return_type,
            body->locals == NULL ? CM_HIR_TYPE_NONE : body->locals[0].type,
            &type_matches) != CM_SEMANTIC_RESULTS_OK)) {
        return 0;
    }
    if (signature == NULL
        || source_body == NULL
        || body->instance.substitution_count
            != item->generic_parameter_count
        || body->instance.substitution_count > 1u
        || (body->instance.substitution_count == 0u
            && body->instance.substitutions != NULL)
        || (body->instance.substitution_count != 0u
            && (body->instance.substitutions == NULL
                || !cm_c_type_is_u32(hir,
                    body->instance.substitutions[0])))
        || (!admitted && (item->generic_parameter_count == 0u
            ? !cm_c_type_is_codegen_scalar(hir, signature->return_type)
            : !cm_c_instance_type_is_u32(hir, item, body,
                signature->return_type)))
        || (admitted && !type_matches)
        || parameter_count == 0u
        || parameter_count > 2u
        || (!admitted && signature->parameters == NULL)
        || parameter_count == UINT32_MAX
        || source_body->parameter_count != parameter_count
        || source_body->local_count < parameter_count
        || source_body->local_count == UINT32_MAX
        || body->local_count < source_body->local_count + 1u
        || body->locals == NULL
        || body->locals[0].kind != CM_MIR_LOCAL_RETURN
        || !cm_c_type_is_codegen_scalar(hir, body->locals[0].type)
        || (!admitted && item->generic_parameter_count == 0u
            && !cm_c_type_equal(hir, body->locals[0].type,
                signature->return_type))
        || body->basic_block_count == 0u
        || body->basic_blocks == NULL) {
        return 0;
    }
    for (parameter_index = 0u;
         parameter_index < parameter_count; ++parameter_index) {
        type_matches = 0;
        memset(&semantic_parameter, 0, sizeof(semantic_parameter));
        if (admitted
            && (cm_mir_admitted_signature_parameter(mir, admission, body_id,
                    parameter_index, &semantic_parameter) != CM_MIR_OK
                || cm_semantic_type_view_matches_monomorphic_hir(
                    semantic_results, admission, &semantic_parameter,
                    body->locals[parameter_index + 1u].type,
                    &type_matches) != CM_SEMANTIC_RESULTS_OK)) {
            return 0;
        }
        if (body->locals[parameter_index + 1u].kind
                != CM_MIR_LOCAL_ARGUMENT
            || (admitted ? !type_matches
                : !cm_c_instance_parameter_matches(hir, item, body,
                body->locals[parameter_index + 1u].type,
                signature->parameters[parameter_index].type))
            || (cm_c_type_is_codegen_scalar(hir,
                    body->locals[parameter_index + 1u].type)
                && !cm_c_type_equal(hir, body->locals[0].type,
                    body->locals[parameter_index + 1u].type))) {
            return 0;
        }
    }
    first_temporary = source_body->local_count + 1u;
    diamond_bool_local = CM_MIR_RETURN_LOCAL;
    is_if_diamond = cm_c_exact_if_diamond(hir, body,
        &diamond_bool_local);
    for (local_index = parameter_count + 1u;
         local_index < first_temporary; ++local_index) {
        if (body->locals[local_index].kind != CM_MIR_LOCAL_USER
            || !cm_c_type_is_supported(hir,
                body->locals[local_index].type)
            || (cm_c_type_is_codegen_scalar(hir,
                    body->locals[local_index].type)
                && !cm_c_type_equal(hir, body->locals[0].type,
                    body->locals[local_index].type))) {
            return 0;
        }
    }
    for (local_index = first_temporary; local_index < body->local_count;
         ++local_index) {
        if (body->locals[local_index].kind != CM_MIR_LOCAL_TEMPORARY
            || (!cm_c_type_is_supported(hir,
                    body->locals[local_index].type)
                && !(is_if_diamond && local_index == diamond_bool_local
                    && cm_c_type_is_bool(hir,
                        body->locals[local_index].type)))
            || (cm_c_type_is_codegen_scalar(hir,
                    body->locals[local_index].type)
                && !cm_c_type_equal(hir, body->locals[0].type,
                    body->locals[local_index].type))) {
            return 0;
        }
    }
    for (block_index = 0u; block_index < body->basic_block_count;
         ++block_index) {
        const CmMirBasicBlock *block;
        const CmMirTerminator *terminator;
        uint32_t statement_index;

        block = &body->basic_blocks[block_index];
        terminator = &block->terminator;
        if ((block->statement_count == 0u) != (block->statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index < block->statement_count; ++statement_index) {
            const CmMirStatement *statement;
            CmMirLocalId destination;

            statement = &block->statements[statement_index];
            if (statement->kind != CM_MIR_STATEMENT_ASSIGN) return 0;
            destination = statement->data.assign.destination;
            if (destination >= body->local_count
                || body->locals[destination].kind == CM_MIR_LOCAL_ARGUMENT
                || cm_c_place_present(
                    &statement->data.assign.destination_place)
                || !cm_c_exact_rvalue_shape(hir, body,
                    &statement->data.assign.value,
                    cm_mir_context_pointer_bits(mir))
                || !cm_c_type_equal(hir,
                    body->locals[destination].type,
                    statement->data.assign.value.type)) {
                return 0;
            }
        }
        if (is_if_diamond) {
            continue;
        }
        if (block_index + 1u == body->basic_block_count) {
            if (terminator->kind != CM_MIR_TERMINATOR_RETURN) return 0;
        } else {
            const CmMirBody *callee_body;
            const CmHirItem *callee_item;
            const CmHirFunctionSignature *callee_signature;
            CmSemanticFunctionSignatureView callee_semantic_signature;
            CmSemanticTypeView callee_semantic_parameter;
            CmMirBodyId callee_id;
            uint32_t callee_parameter_count;
            CmMirLocalId destination;
            uint32_t argument_index;

            if (terminator->kind != CM_MIR_TERMINATOR_CALL
                || terminator->data.call.target != block_index + 1u
                || terminator->data.call.argument_count == 0u
                || terminator->data.call.argument_count > 2u
                || terminator->data.call.arguments == NULL
                || cm_c_place_present(
                    &terminator->data.call.destination_place)) {
                return 0;
            }
            destination = terminator->data.call.destination;
            callee_id = terminator->data.call.callee_instance;
            callee_body = cm_mir_get_body(mir, callee_id);
            callee_item = cm_c_instance_item(hir, callee_body);
            callee_signature = callee_item == NULL ? NULL
                : &callee_item->data.function_item.signature;
            memset(&callee_semantic_signature, 0,
                sizeof(callee_semantic_signature));
            callee_parameter_count = callee_signature == NULL ? 0u
                : callee_signature->parameter_count;
            if (admitted) {
                if (cm_mir_admitted_signature(mir, admission, callee_id,
                        &callee_semantic_signature) != CM_MIR_OK) {
                    return 0;
                }
                callee_parameter_count =
                    callee_semantic_signature.parameter_count;
            }
            if (destination >= body->local_count
                || body->locals[destination].kind == CM_MIR_LOCAL_ARGUMENT
                || !cm_c_type_is_codegen_scalar(hir,
                    body->locals[destination].type)
                || callee_signature == NULL
                || !cm_c_instance_equal(&terminator->data.call.callee,
                    &callee_body->instance)
                || callee_parameter_count
                    != terminator->data.call.argument_count
                || !cm_c_type_equal(hir,
                    body->locals[destination].type,
                    callee_body->locals[CM_MIR_RETURN_LOCAL].type)
                || !cm_c_type_equal(hir, body->locals[0].type,
                    body->locals[destination].type)) {
                return 0;
            }
            for (argument_index = 0u;
                 argument_index < terminator->data.call.argument_count;
                 ++argument_index) {
                type_matches = 0;
                memset(&callee_semantic_parameter, 0,
                    sizeof(callee_semantic_parameter));
                if (admitted
                    && (cm_mir_admitted_signature_parameter(mir, admission,
                            callee_id, argument_index,
                            &callee_semantic_parameter) != CM_MIR_OK
                        || cm_semantic_type_view_matches_monomorphic_hir(
                            semantic_results, admission,
                            &callee_semantic_parameter,
                            terminator->data.call.arguments[argument_index]
                                .type,
                            &type_matches) != CM_SEMANTIC_RESULTS_OK)) {
                    return 0;
                }
                if (!cm_c_exact_operand_shape(hir, body,
                        &terminator->data.call.arguments[argument_index])
                    || (admitted ? !type_matches
                        : !cm_c_instance_parameter_matches(hir,
                        callee_item, callee_body,
                        terminator->data.call.arguments[argument_index]
                            .type,
                        callee_signature->parameters[argument_index]
                            .type))) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int cm_c_exact_export(const CmHirContext *hir,
    const CmMirBody *body)
{
    const CmHirItem *item;
    const CmHirFunctionSignature *signature;
    uint32_t parameter_index;

    item = cm_c_instance_item(hir, body);
    if (item == NULL) return 0;
    signature = &item->data.function_item.signature;
    if (!(body->instance.substitution_count == 0u
        && body->instance.substitutions == NULL
        && item->generic_parameter_count == 0u
        && cm_hir_def_id_is_none(item->parent_definition)
        && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction)
        && item->attribute_count == 1u && item->attributes != NULL
        && cm_c_item_has_attribute(hir, item, "no_mangle")
        && (signature->parameter_count == 1u
            || signature->parameter_count == 2u)
        && signature->parameters != NULL
        && signature->receiver == CM_HIR_RECEIVER_NONE
        && signature->safety == CM_HIR_SAFE
        && !signature->is_const && !signature->is_async
        && !signature->is_variadic
        && cm_c_text_equal(hir, signature->abi, "C")
        && cm_c_type_is_codegen_scalar(hir, signature->return_type))) {
        return 0;
    }
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count;
         ++parameter_index) {
        const CmHirType *parameter;

        if (cm_c_type_equal(hir,
                signature->parameters[parameter_index].type,
                signature->return_type)) {
            continue;
        }
        parameter = cm_hir_get_type(hir,
            signature->parameters[parameter_index].type);
        if (parameter == NULL
            || parameter->kind != CM_HIR_TYPE_REFERENCE_KIND
            || !cm_c_type_is_reference(hir,
                signature->parameters[parameter_index].type)
            || !cm_c_type_equal(hir,
                parameter->data.reference_type.pointee,
                signature->return_type)) {
            return 0;
        }
    }
    return 1;
}

static int cm_c_exact_name(const CmHirContext *hir, const CmMirBody *body,
    int exported, char *output, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    const CmHirItem *item;
    const CmInternedString *name;
    unsigned char digest[CM_SHA256_DIGEST_SIZE];
    size_t prefix_size;
    size_t index;
    int length;

    item = cm_c_instance_item(hir, body);
    if (item == NULL || !cm_c_interned_identifier(hir, item->name, &name)) {
        return 0;
    }
    if (exported) {
        length = snprintf(output, capacity, "%.*s", (int)name->len,
            name->bytes);
    } else if (body->instance.body != CM_HIR_BODY_NONE
        && body->instance.identity_bytes != NULL
        && body->instance.identity_size != 0u) {
        cm_c_instance_digest(&body->instance, digest);
        length = snprintf(output, capacity, "cmrustc_h");
        if (length <= 0 || (size_t)length >= capacity) return 0;
        prefix_size = (size_t)length;
        if (capacity - prefix_size <= CM_SHA256_DIGEST_SIZE * 2u + 1u) {
            return 0;
        }
        for (index = 0u; index < CM_SHA256_DIGEST_SIZE; ++index) {
            output[prefix_size + index * 2u] = hex[digest[index] >> 4u];
            output[prefix_size + index * 2u + 1u] = hex[digest[index] & 15u];
        }
        prefix_size += CM_SHA256_DIGEST_SIZE * 2u;
        output[prefix_size] = '\0';
        length = snprintf(output + prefix_size, capacity - prefix_size,
            "_%.*s", (int)(name->len < 24u ? name->len : 24u),
            name->bytes);
        if (length <= 0 || (size_t)length >= capacity - prefix_size) {
            return 0;
        }
        length += (int)prefix_size;
    } else if (body->instance.substitution_count == 1u
        && body->instance.substitutions != NULL) {
        length = snprintf(output, capacity,
            "cmrustc_%.*s_c%u_d%u_t%u", (int)name->len, name->bytes,
            (unsigned int)body->instance.definition.crate_id,
            (unsigned int)body->instance.definition.index,
            (unsigned int)body->instance.substitutions[0]);
    } else {
        length = snprintf(output, capacity, "cmrustc_%.*s_c%u_d%u",
            (int)name->len, name->bytes,
            (unsigned int)body->instance.definition.crate_id,
            (unsigned int)body->instance.definition.index);
    }
    if (length <= 0 || (size_t)length >= capacity) return 0;
    if (exported && (output[0] == '_'
            || cm_c_identifier_is_keyword(output))) {
        return 0;
    }
    return 1;
}

typedef struct CmCAggregatePlan {
    const CmHirContext *hir;
    unsigned int pointer_bits;
    unsigned char *visits;
    size_t *order;
    size_t order_count;
    CmHirNamedStructLayout *layouts;
    CmHirFieldLayout **fields;
} CmCAggregatePlan;

static void cm_c_aggregate_plan_destroy(CmCAggregatePlan *plan)
{
    size_t index;

    if (plan == NULL) return;
    if (plan->fields != NULL && plan->hir != NULL) {
        for (index = 0u; index < plan->hir->items.len; ++index) {
            cm_free(plan->fields[index]);
        }
    }
    cm_free(plan->fields);
    cm_free(plan->layouts);
    cm_free(plan->order);
    cm_free(plan->visits);
    memset(plan, 0, sizeof(*plan));
}

static int cm_c_aggregate_plan_definition(CmCAggregatePlan *plan,
    CmHirDefId definition);

static int cm_c_aggregate_plan_reference_type(CmCAggregatePlan *plan,
    CmHirTypeId type_id, size_t depth)
{
    CmHirDefId definition;
    const CmHirType *type;
    size_t item_index;

    if (depth > plan->hir->types.len) return 0;
    type = cm_hir_get_type(plan->hir, type_id);
    if (type == NULL || type->kind != CM_HIR_TYPE_REFERENCE_KIND
        || !cm_c_type_is_reference(plan->hir, type_id)) return 0;
    type_id = type->data.reference_type.pointee;
    type = cm_hir_get_type(plan->hir, type_id);
    if (type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return cm_c_aggregate_plan_reference_type(plan, type_id,
            depth + 1u);
    }
    if (!cm_c_type_is_aggregate(plan->hir, type_id, &definition)) {
        return cm_c_type_is_i32(plan->hir, type_id)
            || cm_c_type_is_u32(plan->hir, type_id)
            || cm_c_type_is_usize(plan->hir, type_id)
            || cm_c_type_is_bool(plan->hir, type_id);
    }
    if (cm_c_named_struct(plan->hir, definition, &item_index) == NULL
        || item_index >= plan->hir->items.len) return 0;
    if (plan->visits[item_index] == 1u) return 1;
    return cm_c_aggregate_plan_definition(plan, definition);
}

static int cm_c_type_contains_usize(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth)
{
    CmHirDefId definition;
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t field_index;

    if (cm_c_type_is_usize(hir, type_id)) return 1;
    type = cm_hir_get_type(hir, type_id);
    if (type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return 1;
    }
    if (depth > hir->items.len
        || !cm_c_type_is_aggregate(hir, type_id, &definition)) {
        return 0;
    }
    item = cm_c_named_struct(hir, definition, NULL);
    if (item == NULL) return 0;
    for (field_index = 0u;
         field_index < item->data.aggregate_item.field_count;
         ++field_index) {
        if (cm_c_type_contains_usize(hir,
                item->data.aggregate_item.fields[field_index].type,
                depth + 1u)) {
            return 1;
        }
    }
    return 0;
}

static int cm_c_aggregate_plan_type(CmCAggregatePlan *plan,
    CmHirTypeId type_id)
{
    CmHirDefId definition;

    if (cm_c_type_is_i32(plan->hir, type_id)
        || cm_c_type_is_u32(plan->hir, type_id)
        || cm_c_type_is_usize(plan->hir, type_id)
        || cm_c_type_is_bool(plan->hir, type_id)) {
        return 1;
    }
    if (cm_c_type_is_reference(plan->hir, type_id)) {
        return cm_c_aggregate_plan_reference_type(plan, type_id, 0u);
    }
    if (!cm_c_type_is_aggregate(plan->hir, type_id, &definition)) return 0;
    return cm_c_aggregate_plan_definition(plan, definition);
}

static int cm_c_aggregate_plan_definition(CmCAggregatePlan *plan,
    CmHirDefId definition)
{
    const CmHirItem *item;
    size_t item_index;
    uint32_t field_index;

    item = cm_c_named_struct(plan->hir, definition, &item_index);
    if (item == NULL || item_index >= plan->hir->items.len) return 0;
    if (plan->visits[item_index] == 2u) return 1;
    if (plan->visits[item_index] != 0u) return 0;
    plan->visits[item_index] = 1u;
    for (field_index = 0u;
         field_index < item->data.aggregate_item.field_count;
         ++field_index) {
        if (!cm_c_aggregate_plan_type(plan,
                item->data.aggregate_item.fields[field_index].type)) {
            return 0;
        }
    }
    plan->fields[item_index] = (CmHirFieldLayout *)cm_alloc_zeroed(
        (size_t)item->data.aggregate_item.field_count,
        sizeof(CmHirFieldLayout));
    if (cm_hir_layout_named_struct(plan->hir, plan->pointer_bits,
            definition, &plan->layouts[item_index],
            plan->fields[item_index],
            item->data.aggregate_item.field_count) != CM_HIR_LAYOUT_OK) {
        return 0;
    }
    if (plan->order_count >= plan->hir->items.len) return 0;
    plan->visits[item_index] = 2u;
    plan->order[plan->order_count] = item_index;
    plan->order_count += 1u;
    return 1;
}

static int cm_c_aggregate_plan_build(CmCAggregatePlan *plan,
    const CmHirContext *hir, const CmMirContext *mir,
    const unsigned char *reachable, size_t body_count,
    unsigned int pointer_bits)
{
    size_t body_index;

    memset(plan, 0, sizeof(*plan));
    plan->hir = hir;
    plan->pointer_bits = pointer_bits;
    plan->visits = (unsigned char *)cm_alloc_zeroed(hir->items.len,
        sizeof(unsigned char));
    plan->order = (size_t *)cm_alloc_zeroed(hir->items.len,
        sizeof(size_t));
    plan->layouts = (CmHirNamedStructLayout *)cm_alloc_zeroed(
        hir->items.len, sizeof(CmHirNamedStructLayout));
    plan->fields = (CmHirFieldLayout **)cm_alloc_zeroed(hir->items.len,
        sizeof(CmHirFieldLayout *));
    for (body_index = 0u; body_index < body_count; ++body_index) {
        const CmMirBody *body;
        uint32_t local_index;

        if (reachable[body_index] == 0u) continue;
        body = cm_mir_get_body(mir, (CmMirBodyId)(body_index + 1u));
        if (body == NULL) return 0;
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            if (!cm_c_aggregate_plan_type(plan,
                    body->locals[local_index].type)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_c_struct_name(CmHirDefId definition, char *output,
    size_t capacity)
{
    int length;

    length = snprintf(output, capacity, "cmrustc_struct_c%u_d%u",
        (unsigned int)definition.crate_id,
        (unsigned int)definition.index);
    return length > 0 && (size_t)length < capacity;
}

static void cm_c_append_type(CmStrBuf *output, const CmHirContext *hir,
    CmHirTypeId type_id)
{
    CmHirDefId definition;
    const CmHirType *type;
    char name[CM_C_EXACT_NAME_CAPACITY];

    type = cm_hir_get_type(hir, type_id);
    if (cm_c_type_is_i32(hir, type_id)) {
        cm_str_buf_append(output, "int32_t");
    } else if (cm_c_type_is_u32(hir, type_id)) {
        cm_str_buf_append(output, "uint32_t");
    } else if (cm_c_type_is_usize(hir, type_id)) {
        cm_str_buf_append(output, "uintptr_t");
    } else if (cm_c_type_is_bool(hir, type_id)) {
        cm_str_buf_append(output, "uint8_t");
    } else if (type != NULL
        && type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        cm_c_append_type(output, hir,
            type->data.reference_type.pointee);
        cm_str_buf_append(output, " const *");
    } else {
        (void)cm_c_type_is_aggregate(hir, type_id, &definition);
        (void)cm_c_struct_name(definition, name, sizeof(name));
        cm_str_buf_append(output, "struct ");
        cm_str_buf_append(output, name);
    }
}

static void cm_c_append_aggregate_definitions(CmStrBuf *output,
    const CmCAggregatePlan *plan)
{
    size_t order_index;

    for (order_index = 0u; order_index < plan->order_count;
         ++order_index) {
        const CmHirItem *item;
        char name[CM_C_EXACT_NAME_CAPACITY];

        item = (const CmHirItem *)cm_vec_at_const(&plan->hir->items,
            plan->order[order_index]);
        (void)cm_c_struct_name(item->definition, name, sizeof(name));
        cm_str_buf_append(output, "struct ");
        cm_str_buf_append(output, name);
        cm_str_buf_append(output, ";\n");
    }
    if (plan->order_count != 0u) cm_str_buf_append(output, "\n");
    for (order_index = 0u; order_index < plan->order_count;
         ++order_index) {
        size_t item_index;
        const CmHirItem *item;
        const CmHirNamedStructLayout *layout;
        const CmHirFieldLayout *fields;
        char name[CM_C_EXACT_NAME_CAPACITY];
        char line[768];
        int length;
        uint32_t field_index;

        item_index = plan->order[order_index];
        item = (const CmHirItem *)cm_vec_at_const(&plan->hir->items,
            item_index);
        layout = &plan->layouts[item_index];
        fields = plan->fields[item_index];
        (void)cm_c_struct_name(item->definition, name, sizeof(name));
        cm_str_buf_append(output, "struct ");
        cm_str_buf_append(output, name);
        cm_str_buf_append(output, " {\n");
        for (field_index = 0u;
             field_index < item->data.aggregate_item.field_count;
             ++field_index) {
            cm_str_buf_append(output, "    ");
            cm_c_append_type(output, plan->hir,
                item->data.aggregate_item.fields[field_index].type);
            length = snprintf(line, sizeof(line), " _f%u;\n",
                (unsigned int)field_index);
            cm_str_buf_append_n(output, line, (size_t)length);
        }
        cm_str_buf_append(output, "};\n");
        length = snprintf(line, sizeof(line),
            "struct cmrustc_align_c%u_d%u { char _pad; struct %s _value; };\n"
            "typedef char cmrustc_size_c%u_d%u[(sizeof(struct %s) == %luu) ? 1 : -1];\n"
            "typedef char cmrustc_alignof_c%u_d%u[(offsetof(struct cmrustc_align_c%u_d%u, _value) == %luu) ? 1 : -1];\n",
            (unsigned int)item->definition.crate_id,
            (unsigned int)item->definition.index, name,
            (unsigned int)item->definition.crate_id,
            (unsigned int)item->definition.index, name,
            (unsigned long)layout->size,
            (unsigned int)item->definition.crate_id,
            (unsigned int)item->definition.index,
            (unsigned int)item->definition.crate_id,
            (unsigned int)item->definition.index,
            (unsigned long)layout->alignment);
        cm_str_buf_append_n(output, line, (size_t)length);
        for (field_index = 0u; field_index < layout->field_count;
             ++field_index) {
            length = snprintf(line, sizeof(line),
                "typedef char cmrustc_offset_c%u_d%u_f%u[(offsetof(struct %s, _f%u) == %luu) ? 1 : -1];\n",
                (unsigned int)item->definition.crate_id,
                (unsigned int)item->definition.index,
                (unsigned int)field_index, name,
                (unsigned int)field_index,
                (unsigned long)fields[field_index].offset);
            cm_str_buf_append_n(output, line, (size_t)length);
        }
        cm_str_buf_append(output, "\n");
    }
}

static void cm_c_append_parameters(CmStrBuf *output,
    const CmHirContext *hir, const CmMirBody *body,
    uint32_t parameter_count)
{
    uint32_t index;

    for (index = 0u; index < parameter_count; ++index) {
        char parameter[32];
        int length;

        if (index != 0u) cm_str_buf_append(output, ", ");
        cm_c_append_type(output, hir, body->locals[index + 1u].type);
        length = snprintf(parameter, sizeof(parameter), " _%u",
            (unsigned int)(index + 1u));
        cm_str_buf_append_n(output, parameter, (size_t)length);
    }
}

static void cm_c_append_place(CmStrBuf *output, const CmMirPlace *place)
{
    CmStrBuf expression;
    char text[64];
    int length;
    uint32_t projection_index;

    cm_str_buf_init(&expression);
    length = snprintf(text, sizeof(text), "_%u",
        (unsigned int)place->base);
    cm_str_buf_append_n(&expression, text, (size_t)length);
    for (projection_index = 0u;
         projection_index < place->projection_count;
         ++projection_index) {
        const CmMirPlaceProjection *projection;

        projection = &place->projections[projection_index];
        if (projection->kind == CM_MIR_PROJECTION_DEREFERENCE) {
            CmStrBuf dereference;

            cm_str_buf_init(&dereference);
            cm_str_buf_append(&dereference, "(*(");
            cm_str_buf_append_n(&dereference, expression.data,
                expression.len);
            cm_str_buf_append(&dereference, "))");
            cm_str_buf_destroy(&expression);
            expression = dereference;
        } else {
            length = snprintf(text, sizeof(text), "._f%u",
                (unsigned int)projection->field_index);
            cm_str_buf_append_n(&expression, text, (size_t)length);
        }
    }
    cm_str_buf_append_n(output, expression.data, expression.len);
    cm_str_buf_destroy(&expression);
}

static void cm_c_append_operand(CmStrBuf *output,
    const CmHirContext *hir, const CmMirOperand *operand,
    unsigned int pointer_bits)
{
    char text[64];
    int length;

    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        length = snprintf(text, sizeof(text), "_%u",
            (unsigned int)operand->data.local);
        cm_str_buf_append_n(output, text, (size_t)length);
        return;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        cm_c_append_place(output, &operand->data.place);
        return;
    }
    if (operand->kind == CM_MIR_CONSTANT_U32) {
        length = snprintf(text, sizeof(text), "UINT32_C(%lu)",
            (unsigned long)operand->data.u32_value);
    } else if (operand->kind == CM_MIR_CONSTANT_USIZE
        && pointer_bits == 32u) {
        length = snprintf(text, sizeof(text), "(uintptr_t)UINT32_C(%lu)",
            (unsigned long)operand->data.usize_value);
    } else if (operand->kind == CM_MIR_CONSTANT_USIZE
        && pointer_bits == 64u) {
        length = snprintf(text, sizeof(text), "(uintptr_t)UINT64_C(%llu)",
            (unsigned long long)operand->data.usize_value);
    } else if (operand->data.i32_value == INT32_MIN) {
        length = snprintf(text, sizeof(text),
            "(int32_t)(-2147483647 - 1)");
    } else {
        length = snprintf(text, sizeof(text), "(int32_t)(%ld)",
            (long)operand->data.i32_value);
    }
    cm_str_buf_append_n(output, text, (size_t)length);
    (void)hir;
}

static void cm_c_append_rvalue(CmStrBuf *output, const CmHirContext *hir,
    const CmMirRvalue *rvalue, unsigned int pointer_bits)
{
    uint32_t field_index;

    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        cm_c_append_operand(output, hir, &rvalue->data.use, pointer_bits);
        return;
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        cm_str_buf_append(output, "(uint8_t)((");
        cm_c_append_operand(output, hir, &rvalue->data.equal.left,
            pointer_bits);
        cm_str_buf_append(output, " == ");
        cm_c_append_operand(output, hir, &rvalue->data.equal.right,
            pointer_bits);
        cm_str_buf_append(output,
            ") ? UINT8_C(1) : UINT8_C(0))");
        return;
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        cm_str_buf_append(output, "(uint8_t)((");
        cm_c_append_operand(output, hir, &rvalue->data.less.left,
            pointer_bits);
        cm_str_buf_append(output, " < ");
        cm_c_append_operand(output, hir, &rvalue->data.less.right,
            pointer_bits);
        cm_str_buf_append(output,
            ") ? UINT8_C(1) : UINT8_C(0))");
        return;
    }
    if (rvalue->kind == CM_MIR_RVALUE_BORROW) {
        cm_str_buf_append(output, "&(");
        cm_c_append_place(output, &rvalue->data.borrow.source);
        cm_str_buf_append(output, ")");
        return;
    }
    if (rvalue->kind == CM_MIR_RVALUE_AGGREGATE) {
        cm_str_buf_append(output, "(");
        cm_c_append_type(output, hir, rvalue->type);
        cm_str_buf_append(output, "){ ");
        for (field_index = 0u;
             field_index < rvalue->data.aggregate.field_count;
             ++field_index) {
            char field[32];
            int length;

            if (field_index != 0u) cm_str_buf_append(output, ", ");
            length = snprintf(field, sizeof(field), "._f%u = ",
                (unsigned int)field_index);
            cm_str_buf_append_n(output, field, (size_t)length);
            cm_c_append_operand(output, hir,
                &rvalue->data.aggregate.fields[field_index].value,
                pointer_bits);
        }
        cm_str_buf_append(output, " }");
        return;
    }
    cm_str_buf_append(output, cm_c_type_is_usize(hir, rvalue->type)
        ? "(uintptr_t)(" : "(uint32_t)(");
    cm_c_append_operand(output, hir, &rvalue->data.binary.left,
        pointer_bits);
    cm_str_buf_append(output,
        rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
            ? " + " : " - ");
    cm_c_append_operand(output, hir, &rvalue->data.binary.right,
        pointer_bits);
    cm_str_buf_append(output, ")");
}

static void cm_c_append_assignment(CmStrBuf *output,
    const CmHirContext *hir, const CmMirStatement *statement,
    const char *indent, unsigned int pointer_bits)
{
    char line[64];
    int length;

    cm_str_buf_append(output, indent);
    length = snprintf(line, sizeof(line), "_%u = ",
        (unsigned int)statement->data.assign.destination);
    cm_str_buf_append_n(output, line, (size_t)length);
    cm_c_append_rvalue(output, hir, &statement->data.assign.value,
        pointer_bits);
    cm_str_buf_append(output, ";\n");
}

CmCEmitStatus cm_c_emit_reachable_program(CmStrBuf *output,
    const CmHirContext *hir, const CmMirContext *mir,
    const CmSemanticAdmission *admission,
    const CmMirBodyId *roots, uint32_t root_count,
    const CmTargetDesc *target)
{
    static const char scalar_prefix[] =
        "#include <limits.h>\n"
        "#include <stdint.h>\n\n"
        "#if CHAR_BIT != 8 || !defined(UINT32_MAX) || "
        "UINT32_MAX != 4294967295U\n"
        "# error \"cmrustc requires an exact 32-bit uint32_t\"\n"
        "#endif\n\n";
    static const char aggregate_prefix[] =
        "#include <limits.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        "#if CHAR_BIT != 8 || !defined(UINT32_MAX) || "
        "UINT32_MAX != 4294967295U\n"
        "# error \"cmrustc requires an exact 32-bit uint32_t\"\n"
        "#endif\n"
        "#if !defined(INT32_MAX) || INT32_MAX != 2147483647\n"
        "# error \"cmrustc requires an exact 32-bit int32_t\"\n"
        "#endif\n\n";
    static const char usize32_prefix[] =
        "#if !defined(UINTPTR_MAX) || UINTPTR_MAX != UINT32_MAX\n"
        "# error \"cmrustc target requires an exact 32-bit uintptr_t\"\n"
        "#endif\n"
        "typedef char cmrustc_usize_width[(sizeof(uintptr_t) == 4u) ? 1 : -1];\n\n";
    static const char usize64_prefix[] =
        "#if !defined(UINT64_MAX) || "
        "UINT64_MAX != UINT64_C(18446744073709551615)\n"
        "# error \"cmrustc requires an exact 64-bit uint64_t\"\n"
        "#endif\n"
        "#if !defined(UINTPTR_MAX) || UINTPTR_MAX != UINT64_MAX\n"
        "# error \"cmrustc target requires an exact 64-bit uintptr_t\"\n"
        "#endif\n"
        "typedef char cmrustc_usize_width[(sizeof(uintptr_t) == 8u) ? 1 : -1];\n\n";
    const CmHirCrate *crate_value;
    unsigned char *reachable;
    unsigned char *exported;
    char *names;
    CmCAggregatePlan aggregate_plan;
    size_t body_count;
    size_t index;
    uint32_t root_index;
    int changed;
    int uses_usize;
    CmCEmitStatus status;

    if (output == NULL || hir == NULL || mir == NULL || roots == NULL
        || root_count == 0u || target == NULL) {
        return CM_C_EMIT_INVALID_ARGUMENT;
    }
    if (!cm_c_target_is_supported(target)) {
        return CM_C_EMIT_UNSUPPORTED_TARGET;
    }
    crate_value = hir->crates.len == 1u ? cm_hir_get_crate(hir, 1u) : NULL;
    if (!cm_c_crate_envelope_valid(hir, crate_value)) {
        return CM_C_EMIT_UNSUPPORTED_ENTRY;
    }
    body_count = cm_mir_body_count(mir);
    if (body_count == 0u) return CM_C_EMIT_INVALID_MIR;
    memset(&aggregate_plan, 0, sizeof(aggregate_plan));
    reachable = (unsigned char *)cm_alloc_zeroed(body_count,
        sizeof(unsigned char));
    exported = (unsigned char *)cm_alloc_zeroed(body_count,
        sizeof(unsigned char));
    names = (char *)cm_alloc_zeroed(body_count, CM_C_EXACT_NAME_CAPACITY);
    status = CM_C_EMIT_INVALID_MIR;
    uses_usize = 0;

    for (root_index = 0u; root_index < root_count; ++root_index) {
        size_t root_offset;
        const CmMirBody *root;

        if (roots[root_index] == CM_MIR_BODY_NONE
            || (size_t)roots[root_index] > body_count) goto cleanup_exact;
        root_offset = (size_t)roots[root_index] - 1u;
        if (exported[root_offset] != 0u) goto cleanup_exact;
        root = cm_mir_get_body(mir, roots[root_index]);
        if (!cm_c_exact_export(hir, root)) {
            status = CM_C_EMIT_UNSUPPORTED_ENTRY;
            goto cleanup_exact;
        }
        reachable[root_offset] = 1u;
        exported[root_offset] = 1u;
    }
    do {
        changed = 0;
        for (index = 0u; index < body_count; ++index) {
            const CmMirBody *body;
            uint32_t block_index;

            if (reachable[index] == 0u) continue;
            body = cm_mir_get_body(mir, (CmMirBodyId)(index + 1u));
            if (!cm_c_exact_body_shape(hir, mir, admission,
                    (CmMirBodyId)(index + 1u))) {
                goto cleanup_exact;
            }
            for (block_index = 0u;
                 block_index < body->basic_block_count;
                 ++block_index) {
                CmMirBodyId callee_id;
                size_t callee_offset;

                if (body->basic_blocks[block_index].terminator.kind
                        != CM_MIR_TERMINATOR_CALL) {
                    continue;
                }
                callee_id = body->basic_blocks[block_index]
                    .terminator.data.call.callee_instance;
                if (callee_id == CM_MIR_BODY_NONE
                    || (size_t)callee_id > body_count) {
                    goto cleanup_exact;
                }
                callee_offset = (size_t)callee_id - 1u;
                if (reachable[callee_offset] == 0u) {
                    reachable[callee_offset] = 1u;
                    changed = 1;
                }
            }
        }
    } while (changed);
    for (index = 0u; index < body_count; ++index) {
        const CmMirBody *body;
        size_t old_index;
        uint32_t local_index;

        body = cm_mir_get_body(mir, (CmMirBodyId)(index + 1u));
        if (reachable[index] == 0u
            || !cm_c_exact_body_shape(hir, mir, admission,
                (CmMirBodyId)(index + 1u))
            || !cm_c_exact_name(hir, body, exported[index] != 0u,
                names + index * CM_C_EXACT_NAME_CAPACITY,
                CM_C_EXACT_NAME_CAPACITY)) {
            goto cleanup_exact;
        }
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            if (cm_c_type_contains_usize(hir,
                    body->locals[local_index].type, 0u)) {
                uses_usize = 1;
            }
        }
        for (old_index = 0u; old_index < index; ++old_index) {
            if (strcmp(names + old_index * CM_C_EXACT_NAME_CAPACITY,
                    names + index * CM_C_EXACT_NAME_CAPACITY) == 0) {
                goto cleanup_exact;
            }
        }
    }

    if (uses_usize
        && cm_mir_context_pointer_bits(mir) != target->pointer_bits) {
        goto cleanup_exact;
    }

    if (!cm_c_aggregate_plan_build(&aggregate_plan, hir, mir, reachable,
            body_count, target->pointer_bits)) {
        goto cleanup_exact;
    }

    if (aggregate_plan.order_count == 0u) {
        cm_str_buf_append_n(output, scalar_prefix,
            sizeof(scalar_prefix) - 1u);
    } else {
        cm_str_buf_append_n(output, aggregate_prefix,
            sizeof(aggregate_prefix) - 1u);
    }
    if (uses_usize) {
        if (target->pointer_bits == 32u) {
            cm_str_buf_append_n(output, usize32_prefix,
                sizeof(usize32_prefix) - 1u);
        } else {
            cm_str_buf_append_n(output, usize64_prefix,
                sizeof(usize64_prefix) - 1u);
        }
    }
    if (aggregate_plan.order_count != 0u) {
        cm_c_append_aggregate_definitions(output, &aggregate_plan);
    }
    for (index = 0u; index < body_count; ++index) {
        const CmMirBody *body;
        uint32_t parameter_count;

        if (reachable[index] == 0u) continue;
        body = cm_mir_get_body(mir, (CmMirBodyId)(index + 1u));
        if (!cm_c_body_parameter_count(hir, mir, admission,
                (CmMirBodyId)(index + 1u), &parameter_count)) {
            goto cleanup_exact;
        }
        if (exported[index] == 0u) cm_str_buf_append(output, "static ");
        cm_c_append_type(output, hir,
            body->locals[CM_MIR_RETURN_LOCAL].type);
        cm_str_buf_append(output, " ");
        cm_str_buf_append(output,
            names + index * CM_C_EXACT_NAME_CAPACITY);
        cm_str_buf_append(output, "(");
        cm_c_append_parameters(output, hir, body, parameter_count);
        cm_str_buf_append(output, ");\n");
    }
    cm_str_buf_append(output, "\n");
    for (index = 0u; index < body_count; ++index) {
        const CmMirBody *body;
        const char *name;
        char line[768];
        int length;
        uint32_t local_index;
        uint32_t parameter_count;

        body = cm_mir_get_body(mir, (CmMirBodyId)(index + 1u));
        if (!cm_c_body_parameter_count(hir, mir, admission,
                (CmMirBodyId)(index + 1u), &parameter_count)) {
            goto cleanup_exact;
        }
        name = names + index * CM_C_EXACT_NAME_CAPACITY;
        if (exported[index] == 0u) cm_str_buf_append(output, "static ");
        cm_c_append_type(output, hir,
            body->locals[CM_MIR_RETURN_LOCAL].type);
        cm_str_buf_append(output, " ");
        cm_str_buf_append(output, name);
        cm_str_buf_append(output, "(");
        cm_c_append_parameters(output, hir, body, parameter_count);
        cm_str_buf_append(output, ")\n{\n    ");
        cm_c_append_type(output, hir,
            body->locals[CM_MIR_RETURN_LOCAL].type);
        cm_str_buf_append(output, " _0;\n");
        for (local_index = parameter_count + 1u;
             local_index < body->local_count; ++local_index) {
            cm_str_buf_append(output, "    ");
            cm_c_append_type(output, hir, body->locals[local_index].type);
            length = snprintf(line, sizeof(line), " _%u;\n",
                (unsigned int)local_index);
            cm_str_buf_append_n(output, line, (size_t)length);
        }
        if (cm_c_exact_if_diamond(hir, body, NULL)) {
            const CmMirBasicBlock *entry_block;
            const CmMirBasicBlock *then_block;
            const CmMirBasicBlock *else_block;
            CmMirLocalId condition_local;
            uint32_t statement_index;

            entry_block = &body->basic_blocks[0];
            then_block = &body->basic_blocks[1];
            else_block = &body->basic_blocks[2];
            condition_local = entry_block->terminator.data.switch_bool
                .condition.data.local;
            for (statement_index = 0u;
                 statement_index < entry_block->statement_count;
                 ++statement_index) {
                cm_c_append_assignment(output, hir,
                    &entry_block->statements[statement_index], "    ",
                    target->pointer_bits);
            }
            length = snprintf(line, sizeof(line),
                "    if (_%u != UINT8_C(0)) {\n",
                (unsigned int)condition_local);
            cm_str_buf_append_n(output, line, (size_t)length);
            for (statement_index = 0u;
                 statement_index < then_block->statement_count;
                 ++statement_index) {
                cm_c_append_assignment(output, hir,
                    &then_block->statements[statement_index], "        ",
                    target->pointer_bits);
            }
            cm_str_buf_append(output, "    } else {\n");
            for (statement_index = 0u;
                 statement_index < else_block->statement_count;
                 ++statement_index) {
                cm_c_append_assignment(output, hir,
                    &else_block->statements[statement_index], "        ",
                    target->pointer_bits);
            }
            cm_str_buf_append(output, "    }\n");
        } else if (body->basic_block_count == 1u) {
            uint32_t statement_index;

            for (statement_index = 0u;
                 statement_index
                    < body->basic_blocks[0].statement_count;
                 ++statement_index) {
                const CmMirStatement *statement;

                statement = &body->basic_blocks[0]
                    .statements[statement_index];
                cm_c_append_assignment(output, hir, statement, "    ",
                    target->pointer_bits);
            }
        } else {
            uint32_t block_index;

            for (block_index = 0u;
                 block_index + 1u < body->basic_block_count;
                 ++block_index) {
                CmMirBodyId callee_id;
                const char *callee_name;
                const CmMirBasicBlock *call_block;
                uint32_t argument_index;
                uint32_t statement_index;

                call_block = &body->basic_blocks[block_index];
                for (statement_index = 0u;
                     statement_index < call_block->statement_count;
                     ++statement_index) {
                    const CmMirStatement *statement;

                    statement = &call_block->statements[statement_index];
                    cm_c_append_assignment(output, hir, statement, "    ",
                        target->pointer_bits);
                }
                callee_id = call_block->terminator.data.call
                    .callee_instance;
                callee_name = names + ((size_t)callee_id - 1u)
                    * CM_C_EXACT_NAME_CAPACITY;
                length = snprintf(line, sizeof(line),
                    "    _%u = %s(",
                    (unsigned int)call_block->terminator.data.call
                        .destination,
                    callee_name);
                cm_str_buf_append_n(output, line, (size_t)length);
                for (argument_index = 0u;
                     argument_index
                        < call_block->terminator.data.call.argument_count;
                     ++argument_index) {
                    if (argument_index != 0u) {
                        cm_str_buf_append(output, ", ");
                    }
                    cm_c_append_operand(output, hir,
                        &call_block->terminator.data.call
                            .arguments[argument_index],
                        target->pointer_bits);
                }
                cm_str_buf_append(output, ");\n");
            }
            {
                const CmMirBasicBlock *final_block;
                uint32_t statement_index;

                final_block = &body->basic_blocks[
                    body->basic_block_count - 1u];
                for (statement_index = 0u;
                     statement_index < final_block->statement_count;
                     ++statement_index) {
                    const CmMirStatement *statement;

                    statement = &final_block->statements[statement_index];
                    cm_c_append_assignment(output, hir, statement, "    ",
                        target->pointer_bits);
                }
            }
        }
        cm_str_buf_append(output, "    return _0;\n}\n\n");
    }
    status = CM_C_EMIT_OK;

cleanup_exact:
    cm_c_aggregate_plan_destroy(&aggregate_plan);
    cm_free(names);
    cm_free(exported);
    cm_free(reachable);
    return status;
}

const char *cm_c_emit_status_name(CmCEmitStatus status)
{
    switch (status) {
    case CM_C_EMIT_OK: return "ok";
    case CM_C_EMIT_INVALID_ARGUMENT: return "invalid argument";
    case CM_C_EMIT_UNSUPPORTED_TARGET: return "unsupported target";
    case CM_C_EMIT_INVALID_ENTRY: return "invalid entry";
    case CM_C_EMIT_UNSUPPORTED_ENTRY: return "unsupported entry";
    case CM_C_EMIT_INVALID_MIR: return "invalid MIR";
    }
    return "unknown C emit status";
}
