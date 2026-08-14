#include "cm/mir/model.h"

#include "cm/alloc.h"
#include "cm/hir/instance.h"
#include "cm/hir/semantic_results.h"

#include "../hir/instance_internal.h"
#include "../hir/semantic_results_internal.h"

#include <stdlib.h>
#include <string.h>

#define CM_MIR_STORAGE_ALIGNMENT ((size_t)16u)
#define CM_MIR_EXPRESSION_RECURSION_LIMIT ((size_t)512u)

typedef struct CmMirPublicationEntry {
    CmMirInstance instance;
    CmHirBodyId source_body;
    CmMirBody body;
    int defined;
} CmMirPublicationEntry;

typedef struct CmMirPublicationImpl {
    CmMirContext *context;
    const CmSemanticAdmission *admission;
    const CmHirContext *hir;
    CmHirCrateId crate_id;
    size_t context_body_count;
    uint64_t context_lifetime_id;
    uint64_t admission_capability_id;
    uint64_t barrier_capability_id;
    uint64_t parent_capability_id;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    unsigned int pointer_bits;
    CmVec entries;
} CmMirPublicationImpl;

static uint64_t cm_mir_context_lifetime_counter;

static uint64_t cm_mir_new_context_lifetime_id(void)
{
    if (cm_mir_context_lifetime_counter == UINT64_MAX) abort();
    cm_mir_context_lifetime_counter += 1u;
    return cm_mir_context_lifetime_counter;
}

static const CmMirBody *cm_mir_resolve_body(const CmMirContext *context,
    const CmMirPublicationImpl *publication, CmMirBodyId id);
static int cm_mir_canonical_materialization_valid(const CmHirContext *hir,
    const CmMirInstance *instance);

static int cm_mir_context_valid(const CmMirContext *context)
{
    return context != NULL && context->lifetime_id != UINT64_C(0)
        && (context->pointer_bits == 0u || context->pointer_bits == 32u
            || context->pointer_bits == 64u)
        && context->bodies.elem_size == sizeof(CmMirBody)
        && context->bodies.len <= context->bodies.cap
        && (context->bodies.cap == 0u) == (context->bodies.data == NULL)
        && ((context->admitted_crate == CM_HIR_CRATE_NONE
                && context->admitted_storage_lifetime_id == UINT64_C(0)
                && context->admitted_semantic_generation == UINT64_C(0)
                && context->admitted_rewind_generation == UINT64_C(0)
                && context->admitted_admission_capability_id
                    == UINT64_C(0)
                && context->admitted_barrier_capability_id
                    == UINT64_C(0)
                && context->admitted_parent_capability_id
                    == UINT64_C(0))
            || (context->admitted_crate != CM_HIR_CRATE_NONE
                && context->hir_owner != NULL
                && context->admitted_storage_lifetime_id != UINT64_C(0)
                && context->admitted_semantic_generation != UINT64_C(0)
                && context->admitted_rewind_generation != UINT64_C(0)
                && context->admitted_admission_capability_id
                    != UINT64_C(0)
                && ((context->admitted_barrier_capability_id
                            == UINT64_C(0)
                        && context->admitted_parent_capability_id
                            == UINT64_C(0))
                    || (context->admitted_barrier_capability_id
                            != UINT64_C(0)
                        && context->admitted_parent_capability_id
                            != UINT64_C(0)))));
}

static int cm_mir_admission_identity(const CmSemanticAdmission *admission,
    const CmHirContext **out_hir, CmHirCrateId *out_crate)
{
    const CmHirContext *hir;
    CmHirCrateId crate_id;

    if (!cm_semantic_admission_is_current(admission)) return 0;
    hir = cm_semantic_admission_hir(admission);
    crate_id = cm_semantic_admission_crate(admission);
    if (hir == NULL || crate_id == CM_HIR_CRATE_NONE
        || cm_semantic_admission_generation(admission)
            != hir->semantic_generation) return 0;
    *out_hir = hir;
    *out_crate = crate_id;
    return 1;
}

static int cm_mir_context_accepts_admission(const CmMirContext *context,
    const CmSemanticAdmission *admission, const CmHirContext *hir,
    CmHirCrateId crate_id)
{
    uint64_t capability_id;
    uint64_t barrier_capability_id;
    uint64_t parent_capability_id;

    if (!cm_mir_context_valid(context)) return 0;
    capability_id = cm_semantic_admission_capability_id(admission);
    barrier_capability_id =
        cm_semantic_admission_barrier_capability_id(admission);
    parent_capability_id =
        cm_semantic_admission_parent_capability_id(admission);
    if (capability_id == UINT64_C(0)) return 0;
    if (context->admitted_crate == CM_HIR_CRATE_NONE) {
        return context->bodies.len == 0u && context->hir_owner == NULL;
    }
    return context->hir_owner == hir
        && context->admitted_crate == crate_id
        && context->admitted_storage_lifetime_id == hir->storage.lifetime_id
        && context->admitted_semantic_generation == hir->semantic_generation
        && context->admitted_rewind_generation == hir->rewind_generation
        && context->admitted_admission_capability_id == capability_id
        && context->admitted_barrier_capability_id
            == barrier_capability_id
        && context->admitted_parent_capability_id == parent_capability_id;
}

static void cm_mir_context_latch_admission(CmMirContext *context,
    const CmSemanticAdmission *admission, const CmHirContext *hir,
    CmHirCrateId crate_id)
{
    context->hir_owner = hir;
    context->admitted_crate = crate_id;
    context->admitted_storage_lifetime_id = hir->storage.lifetime_id;
    context->admitted_semantic_generation = hir->semantic_generation;
    context->admitted_rewind_generation = hir->rewind_generation;
    context->admitted_admission_capability_id =
        cm_semantic_admission_capability_id(admission);
    context->admitted_barrier_capability_id =
        cm_semantic_admission_barrier_capability_id(admission);
    context->admitted_parent_capability_id =
        cm_semantic_admission_parent_capability_id(admission);
}

static int cm_mir_instance_is_empty(const CmMirInstance *instance)
{
    return instance != NULL
        && cm_hir_def_id_is_none(instance->definition)
        && cm_hir_def_id_is_none(instance->body_definition)
        && instance->substitutions == NULL
        && instance->substitution_count == 0u
        && instance->body == CM_HIR_BODY_NONE
        && instance->identity_bytes == NULL
        && instance->identity_size == 0u;
}

static int cm_mir_instance_is_canonical(const CmMirInstance *instance)
{
    return instance != NULL
        && !cm_hir_def_id_is_none(instance->definition)
        && !cm_hir_def_id_is_none(instance->body_definition)
        && instance->body != CM_HIR_BODY_NONE
        && instance->identity_bytes != NULL
        && instance->identity_size != 0u
        && (instance->substitution_count == 0u)
            == (instance->substitutions == NULL);
}

static int cm_mir_instance_is_flat(const CmMirInstance *instance)
{
    return instance != NULL
        && !cm_hir_def_id_is_none(instance->definition)
        && cm_hir_def_id_equal(instance->body_definition,
            instance->definition)
        && instance->body == CM_HIR_BODY_NONE
        && instance->identity_bytes == NULL
        && instance->identity_size == 0u
        && (instance->substitution_count == 0u)
            == (instance->substitutions == NULL);
}

static int cm_mir_instance_valid(const CmMirInstance *instance)
{
    return cm_mir_instance_is_empty(instance)
        || cm_mir_instance_is_flat(instance)
        || cm_mir_instance_is_canonical(instance);
}

static int cm_mir_instance_equal(const CmMirInstance *left,
    const CmMirInstance *right)
{
    uint32_t index;

    if (!cm_mir_instance_valid(left) || !cm_mir_instance_valid(right)
        || !cm_hir_def_id_equal(left->definition, right->definition)
        || !cm_hir_def_id_equal(left->body_definition,
            right->body_definition)) {
        return 0;
    }
    if (cm_mir_instance_is_canonical(left)
        || cm_mir_instance_is_canonical(right)) {
        return cm_mir_instance_is_canonical(left)
            && cm_mir_instance_is_canonical(right)
            && left->body == right->body
            && left->identity_size == right->identity_size
            && memcmp(left->identity_bytes, right->identity_bytes,
                left->identity_size) == 0;
    }
    if (left->substitution_count != right->substitution_count) {
        return 0;
    }
    for (index = 0u; index < left->substitution_count; ++index) {
        if (left->substitutions[index] != right->substitutions[index]) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_instance_standalone_clone(CmMirInstance *out_instance,
    const CmMirInstance *source)
{
    CmMirInstance copy;
    size_t substitution_bytes;
    size_t storage_size;
    unsigned char *storage;

    if (out_instance == NULL || !cm_mir_instance_valid(source)
        || cm_mir_instance_is_empty(source)
        || !cm_size_mul((size_t)source->substitution_count,
            sizeof(CmHirTypeId), &substitution_bytes)
        || !cm_size_add(substitution_bytes, source->identity_size,
            &storage_size)) {
        return 0;
    }
    memset(&copy, 0, sizeof(copy));
    copy.definition = source->definition;
    copy.body_definition = source->body_definition;
    copy.substitution_count = source->substitution_count;
    copy.body = source->body;
    copy.identity_size = source->identity_size;
    storage = storage_size == 0u ? NULL
        : (unsigned char *)cm_alloc(storage_size);
    if (substitution_bytes != 0u) {
        copy.substitutions = (CmHirTypeId *)storage;
        memcpy(copy.substitutions, source->substitutions,
            substitution_bytes);
    }
    if (source->identity_size != 0u) {
        copy.identity_bytes = storage + substitution_bytes;
        memcpy(copy.identity_bytes, source->identity_bytes,
            source->identity_size);
    }
    *out_instance = copy;
    return 1;
}

static void cm_mir_instance_standalone_destroy(CmMirInstance *instance)
{
    if (instance == NULL) return;
    cm_free(instance->substitutions != NULL
        ? (void *)instance->substitutions : (void *)instance->identity_bytes);
    memset(instance, 0, sizeof(*instance));
}

static int cm_mir_local_id_valid(const CmMirBody *body, CmMirLocalId local)
{
    return body != NULL && (size_t)local < (size_t)body->local_count;
}

static int cm_mir_legacy_body_shape_valid(const CmMirBody *body)
{
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;

    if (body == NULL || !cm_mir_instance_is_empty(&body->instance)
        || body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_NONE
        || body->owned_storage != NULL
        || body->owner.crate_id == CM_HIR_CRATE_NONE
        || body->owner.index == CM_HIR_DEF_INDEX_NONE
        || body->source_body == CM_HIR_BODY_NONE
        || body->local_count != 1u || body->locals == NULL
        || body->basic_block_count != 1u || body->basic_blocks == NULL) {
        return 0;
    }
    if (body->locals[CM_MIR_RETURN_LOCAL].kind != CM_MIR_LOCAL_RETURN
        || body->locals[CM_MIR_RETURN_LOCAL].type == CM_HIR_TYPE_NONE) {
        return 0;
    }
    block = &body->basic_blocks[CM_MIR_ENTRY_BLOCK];
    if (block->statement_count != 1u || block->statements == NULL
        || block->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
        return 0;
    }
    statement = &block->statements[0];
    return statement->kind == CM_MIR_STATEMENT_ASSIGN
        && statement->data.assign.destination == CM_MIR_RETURN_LOCAL
        && statement->data.assign.value.kind == CM_MIR_RVALUE_USE
        && statement->data.assign.value.type
            == body->locals[CM_MIR_RETURN_LOCAL].type
        && statement->data.assign.value.data.use.kind
            == CM_MIR_CONSTANT_I32
        && statement->data.assign.value.data.use.type
            == statement->data.assign.value.type;
}

static int cm_mir_type_is_u32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_mir_type_is_u8(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U8;
}

static int cm_mir_type_is_fixed_unsigned(const CmHirContext *hir,
    CmHirTypeId id)
{
    return cm_mir_type_is_u8(hir, id) || cm_mir_type_is_u32(hir, id);
}

static int cm_mir_type_is_usize(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static int cm_mir_pointer_bits_valid(unsigned int pointer_bits)
{
    return pointer_bits == 32u || pointer_bits == 64u;
}

static int cm_mir_usize_value_valid(unsigned int pointer_bits,
    uint64_t value)
{
    return pointer_bits == 64u
        || (pointer_bits == 32u && value <= (uint64_t)UINT32_MAX);
}

static int cm_mir_type_is_unsigned_scalar(const CmHirContext *hir,
    CmHirTypeId id, unsigned int pointer_bits)
{
    return cm_mir_type_is_fixed_unsigned(hir, id)
        || (cm_mir_pointer_bits_valid(pointer_bits)
            && cm_mir_type_is_usize(hir, id));
}

static int cm_mir_type_is_i32(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32;
}

static int cm_mir_type_is_bool(const CmHirContext *hir, CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static int cm_mir_type_is_parameter_scalar(const CmHirContext *hir,
    CmHirTypeId id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE);
}

static const CmHirItem *cm_mir_named_struct(const CmHirContext *hir,
    CmHirDefId definition_id);
static const CmHirItem *cm_mir_applied_newtype(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirTypeId *out_field_type);
static int cm_mir_instantiate_executable_type(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance,
    CmHirTypeId declared, CmHirTypeId *out_type);

static int cm_mir_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.binder_index == right->data.binder_index;
    case CM_HIR_REGION_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return 0;
    }
    return 0;
}

/*
 * Implicit method receivers retain an impl-owned `Self` in HIR.  A local,
 * nongeneric impl has one authoritative concrete self type, so MIR can keep
 * the durable HIR type ID while comparing and emitting its concrete shape.
 */
static CmHirTypeId cm_mir_monomorphic_self_type(
    const CmHirContext *hir, CmHirTypeId id, size_t depth)
{
    const CmHirType *type;
    const CmHirDefinition *definition;
    const CmHirItem *owner;

    if (hir == NULL || id == CM_HIR_TYPE_NONE
        || depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) {
        return CM_HIR_TYPE_NONE;
    }
    type = cm_hir_get_type(hir, id);
    if (type == NULL) return CM_HIR_TYPE_NONE;
    if (type->kind != CM_HIR_TYPE_SELF_KIND) return id;
    definition = cm_hir_lookup_definition(hir,
        type->data.self_type.owner);
    owner = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (owner == NULL || owner->kind != CM_HIR_ITEM_IMPL
        || !cm_hir_def_id_equal(owner->definition,
            type->data.self_type.owner)
        || owner->generic_parameter_count != 0u
        || owner->data.impl_item.self_type == id) {
        return CM_HIR_TYPE_NONE;
    }
    return cm_mir_monomorphic_self_type(hir,
        owner->data.impl_item.self_type, depth + 1u);
}

static int cm_mir_type_equal_inner(const CmHirContext *hir,
    CmHirTypeId left, CmHirTypeId right, size_t depth)
{
    const CmHirType *left_type;
    const CmHirType *right_type;

    if (depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) return 0;
    left = cm_mir_monomorphic_self_type(hir, left, depth);
    right = cm_mir_monomorphic_self_type(hir, right, depth);
    if (left == CM_HIR_TYPE_NONE || right == CM_HIR_TYPE_NONE) return 0;
    if (left == right) return cm_hir_get_type(hir, left) != NULL;
    if (cm_mir_type_is_u32(hir, left) && cm_mir_type_is_u32(hir, right)) {
        return 1;
    }
    if (cm_mir_type_is_u8(hir, left) && cm_mir_type_is_u8(hir, right)) {
        return 1;
    }
    if (cm_mir_type_is_usize(hir, left)
        && cm_mir_type_is_usize(hir, right)) {
        return 1;
    }
    if (cm_mir_type_is_bool(hir, left)
        && cm_mir_type_is_bool(hir, right)) {
        return 1;
    }
    left_type = cm_hir_get_type(hir, left);
    right_type = cm_hir_get_type(hir, right);
    if (left_type == NULL || right_type == NULL
        || left_type->kind != right_type->kind) return 0;
    if (left_type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return left_type->data.reference_type.mutability
                == right_type->data.reference_type.mutability
            && cm_mir_region_equal(&left_type->data.reference_type.region,
                &right_type->data.reference_type.region)
            && cm_mir_type_equal_inner(hir,
                left_type->data.reference_type.pointee,
                right_type->data.reference_type.pointee, depth + 1u);
    }
    if (left_type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        uint32_t index;

        if (left_type->data.tuple_type.element_count
                != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
            || right_type->data.tuple_type.element_count
                != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
            || left_type->data.tuple_type.elements == NULL
            || right_type->data.tuple_type.elements == NULL) {
            return 0;
        }
        for (index = 0u;
             index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT; ++index) {
            if (!cm_mir_type_is_parameter_scalar(hir,
                    left_type->data.tuple_type.elements[index])
                || !cm_mir_type_is_parameter_scalar(hir,
                    right_type->data.tuple_type.elements[index])
                || !cm_mir_type_equal_inner(hir,
                    left_type->data.tuple_type.elements[index],
                    right_type->data.tuple_type.elements[index],
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    }
    if (left_type->kind == CM_HIR_TYPE_ADT_KIND
        && left_type->data.named_type.argument_count == 1u
        && right_type->data.named_type.argument_count == 1u
        && left_type->data.named_type.arguments != NULL
        && right_type->data.named_type.arguments != NULL
        && left_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && right_type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE) {
        return cm_hir_def_id_equal(left_type->data.named_type.definition,
                right_type->data.named_type.definition)
            && cm_mir_type_equal_inner(hir,
                left_type->data.named_type.arguments[0].data.type,
                right_type->data.named_type.arguments[0].data.type,
                depth + 1u);
    }
    return left_type->kind == CM_HIR_TYPE_ADT_KIND
        && left_type->data.named_type.argument_count == 0u
        && left_type->data.named_type.arguments == NULL
        && right_type->data.named_type.argument_count == 0u
        && right_type->data.named_type.arguments == NULL
        && cm_hir_def_id_equal(left_type->data.named_type.definition,
            right_type->data.named_type.definition);
}

static int cm_mir_type_equal(const CmHirContext *hir, CmHirTypeId left,
    CmHirTypeId right)
{
    return cm_mir_type_equal_inner(hir, left, right, 0u);
}

static int cm_mir_type_supported(const CmHirContext *hir, CmHirTypeId id,
    unsigned int pointer_bits)
{
    const CmHirType *type;
    uint32_t index;

    id = cm_mir_monomorphic_self_type(hir, id, 0u);
    type = cm_hir_get_type(hir, id);
    if (type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (type->data.tuple_type.element_count
                != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
            || type->data.tuple_type.elements == NULL) {
            return 0;
        }
        for (index = 0u;
             index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT; ++index) {
            if (!cm_mir_type_is_parameter_scalar(hir,
                    type->data.tuple_type.elements[index])
                || !cm_mir_type_supported(hir,
                    type->data.tuple_type.elements[index], pointer_bits)) {
                return 0;
            }
        }
        return 1;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 1u) {
        CmHirTypeId field_type;

        return cm_mir_applied_newtype(hir, id, &field_type) != NULL
            && cm_mir_type_supported(hir, field_type, pointer_bits);
    }
    return type != NULL
        && ((type->kind == CM_HIR_TYPE_INTEGER_KIND
                && (type->data.integer_type.kind == CM_HIR_INT_I32
                    || type->data.integer_type.kind == CM_HIR_INT_U8
                    || type->data.integer_type.kind == CM_HIR_INT_U32
                    || (type->data.integer_type.kind == CM_HIR_INT_USIZE
                        && cm_mir_pointer_bits_valid(pointer_bits))))
            || (type->kind == CM_HIR_TYPE_ADT_KIND
                && type->data.named_type.argument_count == 0u
                && type->data.named_type.arguments == NULL)
            || (type->kind == CM_HIR_TYPE_REFERENCE_KIND
                && (type->data.reference_type.region.kind
                        == CM_HIR_REGION_STATIC
                    || type->data.reference_type.region.kind
                        == CM_HIR_REGION_ERASED)
                && (type->data.reference_type.mutability
                        == CM_HIR_IMMUTABLE
                    || type->data.reference_type.mutability
                        == CM_HIR_MUTABLE)
                && cm_hir_get_type(hir,
                    type->data.reference_type.pointee) != NULL
                && cm_hir_get_type(hir,
                    type->data.reference_type.pointee)->kind
                        != CM_HIR_TYPE_TUPLE_KIND
                && cm_mir_type_supported(hir,
                    type->data.reference_type.pointee, pointer_bits))
            || type->kind == CM_HIR_TYPE_BOOL_KIND);
}

static int cm_mir_type_is_erased_reference(const CmHirContext *hir,
    CmHirTypeId id, unsigned int pointer_bits)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.region.kind == CM_HIR_REGION_ERASED
        && (type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            || type->data.reference_type.mutability == CM_HIR_MUTABLE)
        && cm_mir_type_supported(hir, id, pointer_bits);
}

static int cm_mir_type_is_checked_aggregate(const CmHirContext *hir,
    CmHirTypeId id, CmHirCrateId owner_crate)
{
    const CmHirType *type;
    const CmHirItem *item;

    type = cm_hir_get_type(hir, id);
    item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_mir_named_struct(hir,
            type->data.named_type.definition);
    return item != NULL && item->definition.crate_id == owner_crate
        && item->data.aggregate_item.field_count != 0u
        && item->data.aggregate_item.field_count
            <= CM_MIR_MAX_AGGREGATE_FIELDS;
}

static const CmHirItem *cm_mir_named_struct(const CmHirContext *hir,
    CmHirDefId definition_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(hir, definition_id);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_STRUCT
            && cm_hir_def_id_equal(item->definition, definition_id)
            && cm_hir_def_id_is_none(item->parent_definition)
            && item->generic_parameter_count == 0u
            && item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
            && (item->data.aggregate_item.field_count == 0u)
                == (item->data.aggregate_item.fields == NULL)
        ? item : NULL;
}

static int cm_mir_newtype_has_positive_trait_impl(
    const CmHirContext *hir, CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirType *self_type;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || !item->data.impl_item.has_trait
            || item->data.impl_item.is_negative) {
            continue;
        }
        self_type = cm_hir_get_type(hir, item->data.impl_item.self_type);
        if (self_type != NULL && self_type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(self_type->data.named_type.definition,
                definition)) {
            return 1;
        }
    }
    return 0;
}

static const CmHirItem *cm_mir_applied_newtype(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirTypeId *out_field_type)
{
    const CmHirType *type;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirGenericParam *parameter;
    const CmHirType *declared_field;

    if (out_field_type != NULL) *out_field_type = CM_HIR_TYPE_NONE;
    type = cm_hir_get_type(hir, type_id);
    definition = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 1u
            || type->data.named_type.arguments == NULL
            || type->data.named_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_lookup_definition(hir,
            type->data.named_type.definition);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_equal(item->definition,
            type->data.named_type.definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || item->data.aggregate_item.form != CM_HIR_AGGREGATE_TUPLE
        || item->data.aggregate_item.field_count != 1u
        || item->data.aggregate_item.fields == NULL
        || cm_hir_get_type(hir,
            type->data.named_type.arguments[0].data.type) == NULL
        || cm_mir_newtype_has_positive_trait_impl(hir,
            item->definition)) {
        return NULL;
    }
    parameter = cm_hir_get_generic_param(hir,
        item->generic_parameter_start);
    declared_field = cm_hir_get_type(hir,
        item->data.aggregate_item.fields[0].type);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || parameter->index != 0u
        || !cm_hir_def_id_equal(parameter->owner, item->definition)
        || declared_field == NULL
        || declared_field->kind != CM_HIR_TYPE_PARAMETER_KIND
        || declared_field->data.parameter_type.parameter
            != item->generic_parameter_start) {
        return NULL;
    }
    if (out_field_type != NULL) {
        *out_field_type = type->data.named_type.arguments[0].data.type;
    }
    return item;
}

static int cm_mir_type_target_valid(const CmHirContext *hir,
    CmHirTypeId id, unsigned int pointer_bits, size_t depth)
{
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t index;

    if (depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) return 0;
    id = cm_mir_monomorphic_self_type(hir, id, depth);
    type = cm_hir_get_type(hir, id);
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND) {
        return type->data.integer_type.kind != CM_HIR_INT_USIZE
            || cm_mir_pointer_bits_valid(pointer_bits);
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return (type->data.reference_type.region.kind
                    == CM_HIR_REGION_STATIC
                || type->data.reference_type.region.kind
                    == CM_HIR_REGION_ERASED)
            && (type->data.reference_type.mutability == CM_HIR_IMMUTABLE
                || type->data.reference_type.mutability == CM_HIR_MUTABLE)
            && cm_mir_type_target_valid(hir,
                type->data.reference_type.pointee, pointer_bits,
                depth + 1u);
    }
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (type->data.tuple_type.element_count
                != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
            || type->data.tuple_type.elements == NULL) {
            return 0;
        }
        for (index = 0u;
             index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT; ++index) {
            if (!cm_mir_type_is_parameter_scalar(hir,
                    type->data.tuple_type.elements[index])
                || !cm_mir_type_target_valid(hir,
                    type->data.tuple_type.elements[index], pointer_bits,
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND) return 1;
    if (type->data.named_type.argument_count == 1u) {
        CmHirTypeId field_type;

        return cm_mir_applied_newtype(hir, id, &field_type) != NULL
            && cm_mir_type_target_valid(hir, field_type, pointer_bits,
                depth + 1u);
    }
    item = type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
        ? NULL : cm_mir_named_struct(hir,
            type->data.named_type.definition);
    if (item == NULL) return 0;
    for (index = 0u; index < item->data.aggregate_item.field_count;
         ++index) {
        if (!cm_mir_type_target_valid(hir,
                item->data.aggregate_item.fields[index].type,
                pointer_bits, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_span_within(CmSpan inner, CmSpan outer)
{
    return inner.source != 0u && inner.source == outer.source
        && inner.start <= inner.end && inner.start >= outer.start
        && inner.end <= outer.end;
}

static int cm_mir_place_present(const CmMirPlace *place)
{
    return place != NULL && (place->type != CM_HIR_TYPE_NONE
        || place->projections != NULL || place->projection_count != 0u
        || place->span.source != 0u || place->span.start != 0u
        || place->span.end != 0u);
}

static int cm_mir_place_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place)
{
    const CmHirBody *source_body;
    const CmHirDefinition *body_owner;
    CmHirTypeId current_type;
    uint32_t index;

    if (hir == NULL || body == NULL || place == NULL
        || !cm_mir_local_id_valid(body, place->base)
        || place->type == CM_HIR_TYPE_NONE
        || place->projection_count > CM_MIR_MAX_PLACE_PROJECTIONS
        || (place->projection_count == 0u)
            != (place->projections == NULL)) {
        return 0;
    }
    source_body = cm_hir_get_body(hir, body->source_body);
    body_owner = source_body == NULL ? NULL
        : cm_hir_lookup_definition(hir, source_body->owner);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || body_owner == NULL || body_owner->kind != CM_HIR_DEFINITION_ITEM
        || !cm_hir_def_id_equal(source_body->owner, body->owner)
        || !cm_mir_span_within(place->span, source_body->span)) {
        return 0;
    }
    current_type = body->locals[place->base].type;
    for (index = 0u; index < place->projection_count; ++index) {
        const CmMirPlaceProjection *projection;
        const CmHirType *type;
        const CmHirItem *item;

        projection = &place->projections[index];
        type = cm_hir_get_type(hir, current_type);
        if (projection->kind == CM_MIR_PROJECTION_DEREFERENCE) {
            if (!cm_hir_def_id_is_none(projection->definition)
                || projection->field_index != 0u || type == NULL
                || type->kind != CM_HIR_TYPE_REFERENCE_KIND) {
                return 0;
            }
            current_type = type->data.reference_type.pointee;
            continue;
        }
        if (projection->kind != CM_MIR_PROJECTION_FIELD) return 0;
        if (type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND) {
            if (index != 0u
                || !cm_hir_def_id_is_none(projection->definition)
                || type->data.tuple_type.element_count
                    != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || type->data.tuple_type.elements == NULL
                || projection->field_index
                    >= CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || !cm_mir_type_is_parameter_scalar(hir,
                    type->data.tuple_type
                        .elements[projection->field_index])) {
                return 0;
            }
            current_type = type->data.tuple_type
                .elements[projection->field_index];
            continue;
        }
        if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && type->data.named_type.argument_count == 1u) {
            const CmHirItem *function;
            CmHirTypeId declared_field;
            CmHirTypeId instantiated_field;

            function = body_owner->state != CM_HIR_DEFINITION_BOUND
                ? NULL : cm_hir_get_item(hir, body_owner->entity.item_id);
            item = cm_mir_applied_newtype(hir, current_type,
                &declared_field);
            if (item == NULL || function == NULL
                || function->kind != CM_HIR_ITEM_FUNCTION
                || index != 0u
                || projection->field_index != 0u
                || !cm_hir_def_id_equal(item->definition,
                    projection->definition)
                || !cm_mir_instantiate_executable_type(hir, function,
                    &body->instance, declared_field,
                    &instantiated_field)) {
                return 0;
            }
            current_type = instantiated_field;
            continue;
        }
        item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
                || type->data.named_type.argument_count != 0u
                || type->data.named_type.arguments != NULL
                || !cm_hir_def_id_equal(
                    type->data.named_type.definition,
                    projection->definition)
            ? NULL : cm_mir_named_struct(hir, projection->definition);
        if (item == NULL
            || item->definition.crate_id != body_owner->id.crate_id
            || projection->field_index
                >= item->data.aggregate_item.field_count) {
            return 0;
        }
        current_type = item->data.aggregate_item
            .fields[projection->field_index].type;
    }
    return cm_mir_type_equal(hir, current_type, place->type);
}

CmMirStatus cm_mir_validate_place(const CmHirContext *hir,
    const CmMirBody *body, const CmMirPlace *place)
{
    if (hir == NULL || body == NULL || place == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_place_valid(hir, body, place) ? CM_MIR_OK
        : CM_MIR_INVARIANT_VIOLATION;
}

static const CmHirItem *cm_mir_instance_function(const CmHirContext *hir,
    const CmMirBody *body)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirBody *source_body;

    if (hir == NULL || body == NULL
        || cm_hir_def_id_is_none(body->instance.definition)
        || !cm_hir_def_id_equal(body->owner,
            body->instance.body_definition)
        || body->source_body == CM_HIR_BODY_NONE
        || !cm_mir_instance_valid(&body->instance)
        || (cm_mir_instance_is_canonical(&body->instance)
            && body->instance.body != body->source_body)) {
        return NULL;
    }
    definition = cm_hir_lookup_definition(hir,
        body->instance.body_definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(hir, definition->entity.item_id);
    source_body = cm_hir_get_body(hir, body->source_body);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(item->definition,
            body->instance.body_definition)
        || item->data.function_item.body != body->source_body
        || source_body == NULL
        || !cm_hir_def_id_equal(source_body->owner,
            body->instance.body_definition)) {
        return NULL;
    }
    return item;
}

static int cm_mir_instance_substitutions_valid(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance)
{
    uint32_t index;

    if (cm_mir_instance_is_canonical(instance)) {
        return cm_mir_canonical_materialization_valid(hir, instance);
    }

    if (item == NULL || instance == NULL
        || instance->substitution_count != item->generic_parameter_count
        || (instance->substitution_count == 0u)
            != (instance->substitutions == NULL)) {
        return 0;
    }
    for (index = 0u; index < instance->substitution_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericParamId parameter_id;

        parameter_id = (CmHirGenericParamId)(
            item->generic_parameter_start + index);
        parameter = cm_hir_get_generic_param(hir, parameter_id);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != index
            || !cm_mir_type_is_u32(hir,
                instance->substitutions[index])) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_instantiate_executable_type(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance,
    CmHirTypeId declared, CmHirTypeId *out_type)
{
    const CmHirDefinition *definition;
    const CmHirItem *owner;
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    uint32_t index;

    type = cm_hir_get_type(hir, declared);
    if (type == NULL || out_type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_SELF_KIND) {
        definition = cm_hir_lookup_definition(hir,
            type->data.self_type.owner);
        owner = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        if (item == NULL || instance == NULL || owner == NULL
            || owner->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(owner->definition,
                type->data.self_type.owner)
            || !cm_hir_def_id_equal(item->parent_definition,
                owner->definition)
            || owner->data.impl_item.self_type == declared) {
            return 0;
        }
        return cm_mir_instantiate_executable_type(hir, item, instance,
            owner->data.impl_item.self_type, out_type);
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        CmHirTypeId pointee;

        if (type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
            || (type->data.reference_type.mutability != CM_HIR_IMMUTABLE
                && type->data.reference_type.mutability != CM_HIR_MUTABLE)
            || !cm_mir_instantiate_executable_type(hir, item, instance,
                type->data.reference_type.pointee, &pointee)
            || pointee == CM_HIR_TYPE_NONE
            || !cm_mir_type_supported(hir, declared, 0u)) {
            return 0;
        }
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U8
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE)) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_BOOL_KIND) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count
            == CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
        && type->data.tuple_type.elements != NULL) {
        for (index = 0u;
             index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT; ++index) {
            if (!cm_mir_type_is_parameter_scalar(hir,
                    type->data.tuple_type.elements[index])) {
                return 0;
            }
        }
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && cm_mir_named_struct(hir,
            type->data.named_type.definition) != NULL) {
        *out_type = declared;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 1u) {
        CmHirTypeId field_type;
        CmHirTypeId instantiated_field;

        if (cm_mir_applied_newtype(hir, declared, &field_type) == NULL
            || !cm_mir_instantiate_executable_type(hir, item, instance,
                field_type, &instantiated_field)
            || !cm_mir_type_is_fixed_unsigned(hir,
                instantiated_field)) {
            return 0;
        }
        *out_type = declared;
        return 1;
    }
    if (type->kind != CM_HIR_TYPE_PARAMETER_KIND) return 0;
    parameter = cm_hir_get_generic_param(hir,
        type->data.parameter_type.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
        || parameter->index >= instance->substitution_count) {
        return 0;
    }
    index = parameter->index;
    definition = cm_hir_lookup_definition(hir, parameter->owner);
    owner = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (owner == NULL
        || owner->generic_parameter_start + index
            != type->data.parameter_type.parameter
        || owner->generic_parameter_count != instance->substitution_count
        || (!cm_hir_def_id_equal(owner->definition, item->definition)
            && (!cm_mir_instance_is_canonical(instance)
                || item->generic_parameter_count != 0u
                || !cm_hir_def_id_equal(item->parent_definition,
                    owner->definition)
                || owner->kind != CM_HIR_ITEM_IMPL))
        || (cm_mir_instance_is_canonical(instance)
            ? !cm_mir_type_is_fixed_unsigned(hir,
                instance->substitutions[index])
            : !cm_mir_type_is_u32(hir,
                instance->substitutions[index]))) {
        return 0;
    }
    *out_type = instance->substitutions[index];
    return 1;
}

static int cm_mir_instance_type_supported(const CmHirContext *hir,
    const CmHirItem *item, const CmMirInstance *instance,
    CmHirTypeId type_id, unsigned int pointer_bits)
{
    CmHirTypeId field_type;
    CmHirTypeId instantiated_field;

    if (cm_mir_applied_newtype(hir, type_id, &field_type) == NULL) {
        return cm_mir_type_supported(hir, type_id, pointer_bits);
    }
    return cm_mir_instantiate_executable_type(hir, item, instance,
            field_type, &instantiated_field)
        && cm_mir_type_supported(hir, instantiated_field, pointer_bits)
        && cm_mir_type_target_valid(hir, instantiated_field, pointer_bits,
            0u);
}

static int cm_mir_move_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand)
{
    if (body == NULL || operand == NULL || operand->type == CM_HIR_TYPE_NONE) {
        return 0;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        return cm_mir_local_id_valid(body, operand->data.local)
            && cm_mir_type_equal(hir,
                body->locals[operand->data.local].type, operand->type);
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_place_valid(hir, body, &operand->data.place)
            && cm_mir_type_equal(hir, operand->data.place.type,
                operand->type);
    }
    return 0;
}

static int cm_mir_unsigned_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand,
    unsigned int pointer_bits)
{
    if (operand == NULL || !cm_mir_type_is_unsigned_scalar(hir,
            operand->type, pointer_bits)) {
        return 0;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        return cm_mir_move_operand_valid(hir, body, operand);
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_move_operand_valid(hir, body, operand);
    }
    if (cm_mir_type_is_fixed_unsigned(hir, operand->type)) {
        return operand->kind == CM_MIR_CONSTANT_U32
            && (!cm_mir_type_is_u8(hir, operand->type)
                || operand->data.u32_value <= (uint32_t)UINT8_MAX);
    }
    return operand->kind == CM_MIR_CONSTANT_USIZE
        && cm_mir_usize_value_valid(pointer_bits,
            operand->data.usize_value);
}

static int cm_mir_operand_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirOperand *operand,
    unsigned int pointer_bits)
{
    if (operand == NULL) return 0;
    if (operand->kind == CM_MIR_CONSTANT_I32) {
        return cm_mir_type_is_i32(hir, operand->type);
    }
    if (operand->kind == CM_MIR_CONSTANT_U32) {
        return cm_mir_type_is_fixed_unsigned(hir, operand->type)
            && (!cm_mir_type_is_u8(hir, operand->type)
                || operand->data.u32_value <= (uint32_t)UINT8_MAX);
    }
    if (operand->kind == CM_MIR_CONSTANT_USIZE) {
        return cm_mir_type_is_usize(hir, operand->type)
            && cm_mir_usize_value_valid(pointer_bits,
                operand->data.usize_value);
    }
    return cm_mir_move_operand_valid(hir, body, operand);
}

static int cm_mir_aggregate_rvalue_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    const CmHirBody *source_body;
    const CmHirType *type;
    const CmHirItem *item;
    uint32_t index;

    source_body = cm_hir_get_body(hir, body->source_body);
    type = cm_hir_get_type(hir, rvalue->type);
    item = type == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
            || type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL
            || !cm_hir_def_id_equal(type->data.named_type.definition,
                rvalue->data.aggregate.definition)
        ? NULL : cm_mir_named_struct(hir,
            rvalue->data.aggregate.definition);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || item == NULL
        || item->definition.crate_id != source_body->owner.crate_id
        || item->data.aggregate_item.field_count
            != rvalue->data.aggregate.field_count
        || rvalue->data.aggregate.field_count
            > CM_MIR_MAX_AGGREGATE_FIELDS
        || (rvalue->data.aggregate.field_count == 0u)
            != (rvalue->data.aggregate.fields == NULL)
        || !cm_mir_span_within(rvalue->span, source_body->span)) {
        return 0;
    }
    for (index = 0u; index < rvalue->data.aggregate.field_count; ++index) {
        const CmMirAggregateField *field;

        field = &rvalue->data.aggregate.fields[index];
        if (field->field_index != index
            || !cm_mir_operand_valid(hir, body, &field->value,
                pointer_bits)
            || !cm_mir_type_equal(hir, field->value.type,
                item->data.aggregate_item.fields[index].type)) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_rvalue_valid(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    if (hir == NULL || body == NULL || rvalue == NULL
        || cm_hir_get_type(hir, rvalue->type) == NULL) {
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        return cm_mir_operand_valid(hir, body, &rvalue->data.use,
                pointer_bits)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.use.type);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY) {
        return (rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
                || rvalue->data.binary.operator_kind
                    == CM_MIR_BINARY_SUBTRACT)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.binary.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.binary.right, pointer_bits)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.binary.left.type)
            && cm_mir_type_equal(hir, rvalue->type,
                rvalue->data.binary.right.type)
            && cm_mir_type_is_unsigned_scalar(hir, rvalue->type,
                pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        return cm_mir_type_is_bool(hir, rvalue->type)
            && cm_mir_type_is_u32(hir, rvalue->data.equal.left.type)
            && cm_mir_type_is_u32(hir, rvalue->data.equal.right.type)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.equal.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.equal.right, pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        return cm_mir_type_is_bool(hir, rvalue->type)
            && cm_mir_type_is_usize(hir, rvalue->data.less.left.type)
            && cm_mir_type_is_usize(hir, rvalue->data.less.right.type)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.less.left, pointer_bits)
            && cm_mir_unsigned_operand_valid(hir, body,
                &rvalue->data.less.right, pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_AGGREGATE) {
        return cm_mir_aggregate_rvalue_valid(hir, body, rvalue,
            pointer_bits);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BORROW) {
        const CmHirBody *source_body;
        const CmHirType *reference;
        CmHirMutability mutability;

        source_body = cm_hir_get_body(hir, body->source_body);
        reference = cm_hir_get_type(hir, rvalue->type);
        if (rvalue->data.borrow.kind == CM_MIR_BORROW_SHARED) {
            mutability = CM_HIR_IMMUTABLE;
        } else if (rvalue->data.borrow.kind == CM_MIR_BORROW_MUTABLE) {
            mutability = CM_HIR_MUTABLE;
        } else {
            return 0;
        }
        return reference != NULL
            && reference->kind == CM_HIR_TYPE_REFERENCE_KIND
            && reference->data.reference_type.mutability == mutability
            /* This model has no static-place proof; never mint 'static. */
            && reference->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && source_body != NULL
            && source_body->state == CM_HIR_BODY_TYPED
            && cm_mir_span_within(rvalue->span, source_body->span)
            && cm_mir_span_within(rvalue->data.borrow.source.span,
                rvalue->span)
            && cm_mir_place_valid(hir, body,
                &rvalue->data.borrow.source)
            && cm_mir_type_equal(hir,
                reference->data.reference_type.pointee,
                rvalue->data.borrow.source.type);
    }
    return 0;
}

CmMirStatus cm_mir_validate_rvalue(const CmHirContext *hir,
    const CmMirBody *body, const CmMirRvalue *rvalue,
    unsigned int pointer_bits)
{
    if (hir == NULL || body == NULL || rvalue == NULL
        || (pointer_bits != 0u && !cm_mir_pointer_bits_valid(pointer_bits))) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_rvalue_valid(hir, body, rvalue, pointer_bits)
        ? CM_MIR_OK : CM_MIR_INVARIANT_VIOLATION;
}

static int cm_mir_destination_type(const CmHirContext *hir,
    const CmMirBody *body, CmMirLocalId legacy,
    const CmMirPlace *place, CmHirTypeId *out_type)
{
    if (!cm_mir_local_id_valid(body, legacy) || out_type == NULL) return 0;
    if (!cm_mir_place_present(place)) {
        *out_type = body->locals[legacy].type;
        return 1;
    }
    if (place->base != legacy || !cm_mir_place_valid(hir, body, place)) {
        return 0;
    }
    *out_type = place->type;
    return 1;
}

typedef struct CmMirParameterLayout {
    uint32_t hir_parameter_local_count;
    uint32_t tuple_binding_local_count;
    uint32_t non_temporary_local_count;
} CmMirParameterLayout;

static int cm_mir_parameter_layout(const CmHirContext *hir,
    const CmHirItem *item, const CmHirBody *source_body,
    int require_signature_type_match, CmMirParameterLayout *out_layout)
{
    const CmHirFunctionSignature *signature;
    uint32_t hir_local_index;
    uint32_t parameter_index;
    uint32_t tuple_binding_count;
    uint32_t user_local_count;

    if (hir == NULL || item == NULL || source_body == NULL
        || out_layout == NULL || item->kind != CM_HIR_ITEM_FUNCTION) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    if (signature->parameter_count > 2u
        || source_body->parameter_count != signature->parameter_count
        || (signature->parameter_count != 0u
            && signature->parameters == NULL)
        || (source_body->local_count != 0u
            && source_body->locals == NULL)) {
        return 0;
    }
    hir_local_index = 0u;
    tuple_binding_count = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            const CmHirLocal *local;

            if (hir_local_index >= source_body->local_count) return 0;
            local = &source_body->locals[hir_local_index];
            if (local->parameter_index != parameter_index
                || local->parameter_binding_index != 0u
                || local->name != parameter->name
                || (require_signature_type_match
                    && !cm_mir_type_equal(hir, local->type,
                        parameter->type))) {
                return 0;
            }
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) {
            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || parameter->name != CM_INTERN_ID_NONE) {
                return 0;
            }
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_TUPLE_PATTERN) {
            const CmHirType *tuple_type;
            uint32_t field_index;

            tuple_type = cm_hir_get_type(hir, parameter->type);
            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count
                    != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                return 0;
            }
            for (field_index = 0u;
                 field_index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
                 ++field_index) {
                const CmHirLocal *local;

                if (hir_local_index >= source_body->local_count
                    || !cm_mir_type_is_parameter_scalar(hir,
                        tuple_type->data.tuple_type.elements[field_index])) {
                    return 0;
                }
                local = &source_body->locals[hir_local_index];
                if (local->parameter_index != parameter_index
                    || local->parameter_binding_index != field_index
                    || local->name
                        != parameter->tuple_bindings[field_index].name
                    || local->type
                        != tuple_type->data.tuple_type.elements[field_index]
                    || local->mutability != CM_HIR_IMMUTABLE
                    || local->span.source
                        != parameter->tuple_bindings[field_index].span.source
                    || local->span.start
                        != parameter->tuple_bindings[field_index].span.start
                    || local->span.end
                        != parameter->tuple_bindings[field_index].span.end) {
                    return 0;
                }
                hir_local_index += 1u;
                tuple_binding_count += 1u;
            }
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            const CmHirLocal *local;
            CmHirTypeId field_type;

            if (parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
                || parameter->name != CM_INTERN_ID_NONE
                || cm_mir_applied_newtype(hir, parameter->type,
                    &field_type) == NULL
                || hir_local_index >= source_body->local_count) {
                return 0;
            }
            local = &source_body->locals[hir_local_index];
            if (local->parameter_index != parameter_index
                || local->parameter_binding_index != 0u
                || local->name != parameter->newtype_binding.name
                || !cm_mir_type_equal(hir, local->type, field_type)
                || local->mutability != CM_HIR_IMMUTABLE
                || local->span.source
                    != parameter->newtype_binding.span.source
                || local->span.start
                    != parameter->newtype_binding.span.start
                || local->span.end
                    != parameter->newtype_binding.span.end) {
                return 0;
            }
            hir_local_index += 1u;
            tuple_binding_count += 1u;
            continue;
        }
        return 0;
    }
    user_local_count = source_body->local_count - hir_local_index;
    if (tuple_binding_count > UINT32_MAX - 1u
            - signature->parameter_count
        || user_local_count > UINT32_MAX - 1u
            - signature->parameter_count - tuple_binding_count) {
        return 0;
    }
    out_layout->hir_parameter_local_count = hir_local_index;
    out_layout->tuple_binding_local_count = tuple_binding_count;
    out_layout->non_temporary_local_count = 1u
        + signature->parameter_count + tuple_binding_count
        + user_local_count;
    return 1;
}

static int cm_mir_hir_local_id(const CmHirFunctionSignature *signature,
    const CmHirBody *source_body, const CmMirParameterLayout *layout,
    uint32_t hir_local, CmMirLocalId *out_local)
{
    uint32_t parameter_index;
    uint32_t parameter_local;
    uint32_t tuple_binding;

    if (signature == NULL || source_body == NULL || layout == NULL
        || out_local == NULL || hir_local >= source_body->local_count) {
        return 0;
    }
    if (hir_local >= layout->hir_parameter_local_count) {
        *out_local = 1u + signature->parameter_count
            + layout->tuple_binding_local_count
            + hir_local - layout->hir_parameter_local_count;
        return *out_local < layout->non_temporary_local_count;
    }
    parameter_local = 0u;
    tuple_binding = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            if (parameter_local == hir_local) {
                *out_local = parameter_index + 1u;
                return 1;
            }
            parameter_local += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            if (parameter_local == hir_local) {
                *out_local = 1u + signature->parameter_count
                    + tuple_binding;
                return *out_local < layout->non_temporary_local_count;
            }
            parameter_local += 1u;
            tuple_binding += 1u;
            continue;
        }
        if (parameter->binding_kind != CM_HIR_BINDING_TUPLE_PATTERN) {
            return 0;
        }
        if (hir_local - parameter_local
                < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT) {
            *out_local = 1u + signature->parameter_count + tuple_binding
                + hir_local - parameter_local;
            return *out_local < layout->non_temporary_local_count;
        }
        parameter_local += CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
        tuple_binding += CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
    }
    return 0;
}

typedef struct CmMirTreeMatch {
    const CmMirContext *context;
    const CmMirPublicationImpl *publication;
    const CmHirContext *hir;
    const CmHirItem *item;
    const CmMirBody *body;
    const CmSemanticAdmission *admission;
    const CmSemanticResults *semantic_results;
    const CmHirInstanceSpec *semantic_instance;
    unsigned int pointer_bits;
    uint32_t basic_block_index;
    uint32_t statement_index;
    CmMirLocalId next_temporary;
    uint32_t visible_local_count;
    CmMirParameterLayout parameter_layout;
    CmHirExprId allowed_if_expression;
    CmMirPlaceProjection expected_projections[
        CM_MIR_EXPRESSION_RECURSION_LIMIT];
    size_t expected_projection_count;
} CmMirTreeMatch;

typedef struct CmMirSemanticInstanceQuery {
    CmHirGenericArg *arguments;
    CmHirInstanceSpec spec;
} CmMirSemanticInstanceQuery;

static int cm_mir_semantic_instance_query_init(
    CmMirSemanticInstanceQuery *query, const CmHirContext *hir,
    const CmMirBody *body)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirItem *impl_item;
    uint32_t index;

    memset(query, 0, sizeof(*query));
    if (body == NULL
        || (body->instance.substitution_count == 0u)
            != (body->instance.substitutions == NULL)) {
        return 0;
    }
    if (body->instance.substitution_count != 0u) {
        query->arguments = (CmHirGenericArg *)cm_alloc_zeroed(
            body->instance.substitution_count, sizeof(CmHirGenericArg));
    }
    cm_hir_instance_spec_init(&query->spec);
    query->spec.selected_callable = body->instance.definition;
    query->spec.body_definition = body->instance.body_definition;
    definition = cm_hir_lookup_definition(hir, body->instance.definition);
    item = definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) goto invalid;
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        query->spec.item_arguments = query->arguments;
        query->spec.item_argument_count = body->instance.substitution_count;
    } else {
        definition = cm_hir_lookup_definition(hir,
            item->parent_definition);
        impl_item = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
                || definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
            || item->generic_parameter_count != 0u
            || impl_item->generic_parameter_count
                != body->instance.substitution_count
            || impl_item->generic_parameter_count > 1u
            || !impl_item->data.impl_item.has_trait
            || impl_item->data.impl_item.is_negative
            || cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)) goto invalid;
        query->spec.declared_trait_callable =
            item->data.function_item.trait_item_definition;
        query->spec.enclosing_impl = impl_item->definition;
        query->spec.implemented_trait =
            impl_item->data.impl_item.trait_type.definition;
        query->spec.enclosing_impl_arguments = query->arguments;
        query->spec.enclosing_impl_argument_count =
            body->instance.substitution_count;
        query->spec.self_owner = impl_item->definition;
        query->spec.self_type = body->instance.substitution_count == 0u
            ? impl_item->data.impl_item.self_type
            : body->instance.substitutions[0];
    }
    for (index = 0u; index < body->instance.substitution_count; ++index) {
        query->arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        query->arguments[index].data.type =
            body->instance.substitutions[index];
    }
    return 1;

invalid:
    cm_free(query->arguments);
    memset(query, 0, sizeof(*query));
    return 0;
}

static void cm_mir_semantic_instance_query_destroy(
    CmMirSemanticInstanceQuery *query)
{
    if (query == NULL) return;
    cm_free(query->arguments);
    memset(query, 0, sizeof(*query));
}

static int cm_mir_canonical_materialization_valid(const CmHirContext *hir,
    const CmMirInstance *instance)
{
    CmHirCanonicalInstance identity;
    CmHirCanonicalInstance encoded;
    CmHirDecodedCanonicalInstance decoded;
    const CmHirCanonicalArgumentPart *executable_arguments;
    uint32_t executable_argument_count;
    uint32_t index;
    int valid;

    if (!cm_mir_instance_is_canonical(instance)) return 0;
    cm_hir_canonical_instance_init(&identity);
    identity.definition = instance->definition;
    identity.body_definition = instance->body_definition;
    identity.body = instance->body;
    identity.bytes = instance->identity_bytes;
    identity.size = instance->identity_size;
    if (cm_hir_canonical_instance_validate(hir,
            instance->definition.crate_id, &identity)
            != CM_HIR_INSTANCE_OK) return 0;
    cm_hir_decoded_canonical_instance_init(&decoded);
    cm_hir_canonical_instance_init(&encoded);
    if (cm_hir_canonical_instance_decode(hir,
            instance->definition.crate_id, &identity, &decoded)
            != CM_HIR_INSTANCE_OK) {
        return 0;
    }
    if (cm_hir_def_id_is_none(decoded.parts.enclosing_impl)) {
        executable_arguments = decoded.parts.item_arguments;
        executable_argument_count = decoded.parts.item_argument_count;
    } else if (cm_hir_def_id_equal(decoded.parts.selected_callable,
            decoded.parts.declared_trait_callable)) {
        executable_arguments = decoded.parts.method_arguments;
        executable_argument_count = decoded.parts.method_argument_count;
    } else {
        executable_arguments = decoded.parts.enclosing_impl_arguments;
        executable_argument_count =
            decoded.parts.enclosing_impl_argument_count;
    }
    valid = cm_hir_canonical_instance_encode_parts(hir,
            instance->definition.crate_id, &decoded.parts, &encoded)
                == CM_HIR_INSTANCE_OK
        && cm_hir_def_id_equal(encoded.definition, instance->definition)
        && cm_hir_def_id_equal(encoded.body_definition,
            instance->body_definition)
        && encoded.body == instance->body
        && encoded.size == instance->identity_size
        && memcmp(encoded.bytes, instance->identity_bytes,
            encoded.size) == 0
        && instance->substitution_count == executable_argument_count;
    for (index = 0u; valid && index < executable_argument_count; ++index) {
        valid = executable_arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE
            && cm_hir_canonical_type_matches(hir,
                instance->substitutions[index],
                executable_arguments[index].bytes,
                executable_arguments[index].size) == CM_HIR_INSTANCE_OK;
    }
    cm_hir_canonical_instance_destroy(&encoded);
    cm_hir_decoded_canonical_instance_destroy(&decoded);
    return valid;
}

static int cm_mir_canonical_identity(const CmMirInstance *instance,
    CmHirCanonicalInstance *out_identity)
{
    if (!cm_mir_instance_is_canonical(instance) || out_identity == NULL) {
        return 0;
    }
    cm_hir_canonical_instance_init(out_identity);
    out_identity->definition = instance->definition;
    out_identity->body_definition = instance->body_definition;
    out_identity->body = instance->body;
    out_identity->bytes = instance->identity_bytes;
    out_identity->size = instance->identity_size;
    return 1;
}

static CmSemanticResultsStatus cm_mir_semantic_signature_query(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    const CmHirInstanceSpec *instance,
    CmSemanticFunctionSignatureView *out_view)
{
    CmMirSemanticInstanceQuery query;
    CmHirCanonicalInstance canonical;
    CmSemanticResultsStatus status;

    if (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_signature(results, admission,
            body->source_body, out_view);
    }
    if (body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&body->instance, &canonical)) {
        return cm_semantic_results_canonical_instance_signature(results,
            admission, &canonical, out_view);
    }
    if (instance != NULL) {
        return cm_semantic_results_instance_signature(results, admission,
            instance, out_view);
    }
    if (!cm_mir_semantic_instance_query_init(&query,
            cm_semantic_results_hir(results, admission), body)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_signature(results, admission,
        &query.spec, out_view);
    cm_mir_semantic_instance_query_destroy(&query);
    return status;
}

static CmSemanticResultsStatus cm_mir_semantic_signature_parameter_query(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    const CmHirInstanceSpec *instance, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmMirSemanticInstanceQuery query;
    CmHirCanonicalInstance canonical;
    CmSemanticResultsStatus status;

    if (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_signature_parameter(results, admission,
            body->source_body, parameter, out_view);
    }
    if (body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&body->instance, &canonical)) {
        return
            cm_semantic_results_canonical_instance_signature_parameter(
                results, admission, &canonical, parameter, out_view);
    }
    if (instance != NULL) {
        return cm_semantic_results_instance_signature_parameter(results,
            admission, instance, parameter, out_view);
    }
    if (!cm_mir_semantic_instance_query_init(&query,
            cm_semantic_results_hir(results, admission), body)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_signature_parameter(results,
        admission, &query.spec, parameter, out_view);
    cm_mir_semantic_instance_query_destroy(&query);
    return status;
}

static CmSemanticResultsStatus cm_mir_semantic_expression_query(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    const CmHirInstanceSpec *instance, CmHirExprId expression,
    CmSemanticExpressionView *out_view)
{
    CmHirCanonicalInstance canonical;

    if (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_expression(results, admission,
            body->source_body, expression, out_view);
    }
    if (body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&body->instance, &canonical)
        ? cm_semantic_results_canonical_instance_expression(results,
            admission, &canonical, expression, out_view)
        : cm_semantic_results_instance_expression(results, admission,
            instance, expression, out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_adjustment_query(
    const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    const CmHirInstanceSpec *instance, CmHirExprId expression,
    uint32_t adjustment, CmSemanticAdjustmentView *out_view)
{
    CmHirCanonicalInstance canonical;

    if (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_expression_adjustment(results, admission,
            body->source_body, expression, adjustment, out_view);
    }
    if (body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&body->instance, &canonical)
        ? cm_semantic_results_canonical_instance_expression_adjustment(
            results, admission, &canonical, expression, adjustment,
            out_view)
        : cm_semantic_results_instance_expression_adjustment(results,
            admission, instance, expression, adjustment, out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_primitive_binary_query(
    const CmMirTreeMatch *match, CmHirExprId expression,
    CmSemanticPrimitiveBinaryView *out_view)
{
    CmHirCanonicalInstance canonical;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_primitive_binary(match->semantic_results,
            match->admission, match->body->source_body, expression,
            out_view);
    }
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&match->body->instance, &canonical)
        ? cm_semantic_results_canonical_instance_primitive_binary(
            match->semantic_results, match->admission, &canonical,
            expression, out_view)
        : cm_semantic_results_instance_primitive_binary(
            match->semantic_results, match->admission,
            match->semantic_instance, expression, out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_field_selection_query(
    const CmMirTreeMatch *match, CmHirExprId expression,
    CmSemanticFieldSelectionView *out_view)
{
    CmHirCanonicalInstance canonical;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_field_selection(match->semantic_results,
            match->admission, match->body->source_body, expression,
            out_view);
    }
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&match->body->instance, &canonical)
        ? cm_semantic_results_canonical_instance_field_selection(
            match->semantic_results, match->admission, &canonical,
            expression, out_view)
        : cm_semantic_results_instance_field_selection(
            match->semantic_results, match->admission,
            match->semantic_instance, expression, out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_callable_query(
    const CmMirTreeMatch *match, const CmMirBody *callee,
    CmHirExprId expression,
    CmSemanticCallableSelectionView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target_identity;
    CmMirSemanticInstanceQuery target;
    CmSemanticResultsStatus status;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_selection(
            match->semantic_results,
            match->admission, match->body->source_body, expression,
            out_view);
    }
    if (cm_mir_instance_is_canonical(&match->body->instance)
        != cm_mir_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&match->body->instance, &caller)
        && cm_mir_canonical_identity(&callee->instance, &target_identity)) {
        return
            cm_semantic_results_canonical_instance_callable_selection_for_callee(
                match->semantic_results, match->admission, &caller,
                expression, &target_identity, out_view);
    }
    memset(&target, 0, sizeof(target));
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        || !cm_mir_semantic_instance_query_init(&target, match->hir,
            callee)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_callable_selection_for_callee(
        match->semantic_results, match->admission,
        match->semantic_instance, expression, &target.spec, out_view);
    cm_mir_semantic_instance_query_destroy(&target);
    return status;
}

/* Definition lookup hint; callee-bound authority is checked after resolve. */
static CmSemanticResultsStatus cm_mir_semantic_callable_hint_query(
    const CmMirTreeMatch *match, CmHirExprId expression,
    CmSemanticCallableSelectionView *out_view)
{
    CmHirCanonicalInstance caller;

    if (match->body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_selection(
            match->semantic_results, match->admission,
            match->body->source_body, expression, out_view);
    }
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&match->body->instance, &caller)
        ? cm_semantic_results_canonical_instance_callable_selection(
            match->semantic_results, match->admission, &caller,
            expression, out_view)
        : cm_semantic_results_instance_callable_selection(
            match->semantic_results, match->admission,
            match->semantic_instance, expression, out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_callable_argument_query(
    const CmMirTreeMatch *match, CmHirExprId expression, uint32_t argument,
    CmHirExprId *out_expression)
{
    CmHirCanonicalInstance caller;

    if (match->body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_argument(match->semantic_results,
            match->admission, match->body->source_body, expression,
            argument, out_expression);
    }
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&match->body->instance, &caller)
        ? cm_semantic_results_canonical_instance_callable_argument(
            match->semantic_results, match->admission, &caller, expression,
            argument, out_expression)
        : cm_semantic_results_instance_callable_argument(
            match->semantic_results, match->admission,
            match->semantic_instance, expression, argument,
            out_expression);
}

static CmSemanticResultsStatus
cm_mir_semantic_callable_generic_argument_query(
    const CmMirTreeMatch *match, CmHirExprId expression,
    CmSemanticCallableGenericArgumentDomain domain, uint32_t argument,
    CmSemanticGenericArgumentView *out_view)
{
    CmHirCanonicalInstance caller;

    if (match == NULL || out_view == NULL) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (match->body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_generic_argument(
            match->semantic_results, match->admission,
            match->body->source_body, expression, domain, argument,
            out_view);
    }
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    return cm_mir_canonical_identity(&match->body->instance, &caller)
        ? cm_semantic_results_canonical_instance_callable_generic_argument(
            match->semantic_results, match->admission, &caller, expression,
            domain, argument, out_view)
        : cm_semantic_results_instance_callable_generic_argument(
            match->semantic_results, match->admission,
            match->semantic_instance, expression, domain, argument,
            out_view);
}

static CmSemanticResultsStatus cm_mir_semantic_callable_parameter_query(
    const CmMirTreeMatch *match, const CmMirBody *callee,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target_identity;
    CmMirSemanticInstanceQuery target;
    CmSemanticResultsStatus status;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_callable_parameter(
            match->semantic_results,
            match->admission, match->body->source_body, expression,
            parameter, out_view);
    }
    if (cm_mir_instance_is_canonical(&match->body->instance)
        != cm_mir_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&match->body->instance, &caller)
        && cm_mir_canonical_identity(&callee->instance, &target_identity)) {
        return
            cm_semantic_results_canonical_instance_callable_parameter_for_callee(
                match->semantic_results, match->admission, &caller,
                expression, &target_identity, parameter, out_view);
    }
    memset(&target, 0, sizeof(target));
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        || !cm_mir_semantic_instance_query_init(&target, match->hir,
            callee)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_callable_parameter_for_callee(
        match->semantic_results, match->admission,
        match->semantic_instance, expression, &target.spec, parameter,
        out_view);
    cm_mir_semantic_instance_query_destroy(&target);
    return status;
}

static CmSemanticResultsStatus cm_mir_semantic_direct_call_query(
    const CmMirTreeMatch *match, const CmMirBody *callee,
    CmHirExprId expression, CmSemanticDirectCallView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target_identity;
    CmMirSemanticInstanceQuery target;
    CmSemanticResultsStatus status;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_direct_call(match->semantic_results,
            match->admission, match->body->source_body, expression,
            out_view);
    }
    if (cm_mir_instance_is_canonical(&match->body->instance)
        != cm_mir_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&match->body->instance, &caller)
        && cm_mir_canonical_identity(&callee->instance, &target_identity)) {
        return cm_semantic_results_canonical_instance_direct_call(
            match->semantic_results, match->admission, &caller, expression,
            &target_identity, out_view);
    }
    memset(&target, 0, sizeof(target));
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        || !cm_mir_semantic_instance_query_init(&target, match->hir,
            callee)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_direct_call(
        match->semantic_results, match->admission,
        match->semantic_instance, expression, &target.spec, out_view);
    cm_mir_semantic_instance_query_destroy(&target);
    return status;
}

static CmSemanticResultsStatus cm_mir_semantic_direct_call_parameter_query(
    const CmMirTreeMatch *match, const CmMirBody *callee,
    CmHirExprId expression, uint32_t parameter,
    CmSemanticTypeView *out_view)
{
    CmHirCanonicalInstance caller;
    CmHirCanonicalInstance target_identity;
    CmMirSemanticInstanceQuery target;
    CmSemanticResultsStatus status;

    if (match->body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_BODY) {
        return cm_semantic_results_direct_call_parameter(
            match->semantic_results, match->admission,
            match->body->source_body, expression, parameter, out_view);
    }
    if (cm_mir_instance_is_canonical(&match->body->instance)
        != cm_mir_instance_is_canonical(&callee->instance)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    if (cm_mir_canonical_identity(&match->body->instance, &caller)
        && cm_mir_canonical_identity(&callee->instance, &target_identity)) {
        return
            cm_semantic_results_canonical_instance_direct_call_parameter(
                match->semantic_results, match->admission, &caller,
                expression, &target_identity, parameter, out_view);
    }
    memset(&target, 0, sizeof(target));
    if (match->body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        || !cm_mir_semantic_instance_query_init(&target, match->hir,
            callee)) {
        return CM_SEMANTIC_RESULTS_INVALID_ARGUMENT;
    }
    status = cm_semantic_results_instance_direct_call_parameter(
        match->semantic_results, match->admission,
        match->semantic_instance, expression, &target.spec, parameter,
        out_view);
    cm_mir_semantic_instance_query_destroy(&target);
    return status;
}

static int cm_mir_semantic_view_equal(const CmSemanticTypeView *left,
    const CmSemanticTypeView *right)
{
    int equal;

    equal = 0;
    return cm_semantic_type_view_equal(left, right, &equal)
            == CM_SEMANTIC_RESULTS_OK
        && equal;
}

static int cm_mir_semantic_view_matches_hir(
    const CmMirTreeMatch *match, const CmSemanticTypeView *view,
    CmHirTypeId type)
{
    int equal;

    equal = 0;
    return match != NULL && match->semantic_results != NULL
        && match->admission != NULL
        && cm_semantic_type_view_matches_monomorphic_hir(
            match->semantic_results, match->admission, view, type, &equal)
                == CM_SEMANTIC_RESULTS_OK
        && equal;
}

static int cm_mir_semantic_view_matches(const CmSemanticResults *results,
    const CmSemanticAdmission *admission, const CmSemanticTypeView *view,
    CmHirTypeId type)
{
    int equal;

    equal = 0;
    return results != NULL && admission != NULL
        && cm_semantic_type_view_matches_monomorphic_hir(results, admission,
            view, type, &equal) == CM_SEMANTIC_RESULTS_OK
        && equal;
}

static int cm_mir_selected_callee_matches_selection(
    const CmMirTreeMatch *match, const CmMirBody *callee,
    const CmSemanticCallableSelectionView *selection,
    const CmHirExpr *expression)
{
    CmMirSemanticInstanceQuery query;
    int self_matches;
    int valid;

    if (match == NULL || callee == NULL || selection == NULL
        || expression == NULL
        || (expression->kind != CM_HIR_EXPR_QUALIFIED_CALL
            && expression->kind != CM_HIR_EXPR_METHOD_CALL)) return 0;
    memset(&query, 0, sizeof(query));
    self_matches = 0;
    if (!cm_mir_semantic_instance_query_init(&query, match->hir, callee)) {
        return 0;
    }
    if (cm_semantic_type_view_matches_monomorphic_hir(
            match->semantic_results, match->admission,
            &selection->requested_self_type, query.spec.self_type,
            &self_matches) != CM_SEMANTIC_RESULTS_OK) {
        cm_mir_semantic_instance_query_destroy(&query);
        return 0;
    }
    valid = cm_hir_def_id_equal(query.spec.selected_callable,
            selection->selected_callable)
        && cm_hir_def_id_equal(query.spec.declared_trait_callable,
            selection->declared_trait_callable)
        && cm_hir_def_id_equal(query.spec.enclosing_impl,
            selection->selected_impl)
        && cm_hir_def_id_equal(query.spec.implemented_trait,
            selection->requested_trait)
        && cm_hir_def_id_equal(query.spec.self_owner,
            selection->selected_impl)
        && self_matches
        && query.spec.item_argument_count == 0u
        && query.spec.item_arguments == NULL
        && query.spec.method_argument_count == 0u
        && query.spec.method_arguments == NULL
        && query.spec.enclosing_impl_argument_count == 0u
        && query.spec.enclosing_impl_arguments == NULL
        && query.spec.implemented_trait_argument_count == 0u
        && query.spec.implemented_trait_arguments == NULL;
    cm_mir_semantic_instance_query_destroy(&query);
    return valid;
}

static int cm_mir_callable_arguments(const CmHirExpr *expression,
    CmHirExprId storage[2], const CmHirExprId **out_arguments,
    uint32_t *out_count)
{
    uint32_t index;

    if (expression == NULL || storage == NULL || out_arguments == NULL
        || out_count == NULL) return 0;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        if (expression->data.qualified_call.argument_count == 0u
            || expression->data.qualified_call.argument_count > 2u
            || expression->data.qualified_call.arguments == NULL) return 0;
        *out_arguments = expression->data.qualified_call.arguments;
        *out_count = expression->data.qualified_call.argument_count;
        return 1;
    }
    if (expression->kind != CM_HIR_EXPR_METHOD_CALL
        || expression->data.method_call.receiver == CM_HIR_EXPR_NONE
        || expression->data.method_call.argument_count > 1u
        || (expression->data.method_call.argument_count != 0u
            && expression->data.method_call.arguments == NULL)) return 0;
    storage[0] = expression->data.method_call.receiver;
    for (index = 0u; index < expression->data.method_call.argument_count;
         ++index) {
        storage[index + 1u] = expression->data.method_call.arguments[index];
    }
    *out_arguments = storage;
    *out_count = expression->data.method_call.argument_count + 1u;
    return 1;
}

static const CmHirItem *cm_mir_definition_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_mir_method_trait_in_scope(const CmHirExpr *expression,
    CmHirDefId trait_definition)
{
    uint32_t index;

    if (expression == NULL || expression->kind != CM_HIR_EXPR_METHOD_CALL) {
        return 0;
    }
    for (index = 0u;
         index < expression->data.method_call.in_scope_trait_count; ++index) {
        if (cm_hir_def_id_equal(
                expression->data.method_call.in_scope_traits[index],
                trait_definition)) return 1;
    }
    return 0;
}

typedef struct CmMirAdjustedReceiver {
    int present;
    CmMirBorrowKind borrow_kind;
    CmHirTypeId source_type;
    CmHirTypeId target_type;
    CmSemanticTypeView adjusted_type;
    CmMirLocalId source_local;
    CmSpan span;
} CmMirAdjustedReceiver;

static int cm_mir_adjusted_receiver_recipe(const CmMirTreeMatch *match,
    const CmHirExpr *call, CmHirExprId call_id,
    const CmSemanticCallableSelectionView *selection,
    CmMirAdjustedReceiver *out_receiver)
{
    const CmHirExpr *receiver;
    const CmHirBody *source_body;
    const CmHirItem *declared;
    const CmHirItem *selected;
    const CmHirType *target;
    CmSemanticExpressionView expression_view;
    CmSemanticAdjustmentView adjustment;
    CmHirReceiverKind receiver_kind;
    CmHirMutability mutability;

    if (match == NULL || call == NULL || selection == NULL
        || out_receiver == NULL || call->kind != CM_HIR_EXPR_METHOD_CALL
        || selection->body != match->body->source_body
        || selection->expression != call_id
        || selection->syntax != CM_HIR_CALLABLE_DOT_METHOD
        || selection->receiver_argument != 0u
        || selection->receiver_expression != call->data.method_call.receiver) {
        return 0;
    }
    memset(out_receiver, 0, sizeof(*out_receiver));
    receiver = cm_hir_get_expr(match->hir,
        call->data.method_call.receiver);
    source_body = cm_hir_get_body(match->hir, match->body->source_body);
    declared = cm_mir_definition_item(match->hir,
        selection->declared_trait_callable);
    selected = cm_mir_definition_item(match->hir,
        selection->selected_callable);
    memset(&expression_view, 0, sizeof(expression_view));
    if (receiver == NULL || declared == NULL
        || selected == NULL || declared->kind != CM_HIR_ITEM_FUNCTION
        || selected->kind != CM_HIR_ITEM_FUNCTION
        || receiver->owner_body != match->body->source_body
        || cm_mir_semantic_expression_query(match->semantic_results,
            match->admission, match->body, match->semantic_instance,
            call->data.method_call.receiver, &expression_view)
                != CM_SEMANTIC_RESULTS_OK
        || expression_view.body != match->body->source_body
        || expression_view.expression != call->data.method_call.receiver
        || !cm_mir_semantic_view_matches_hir(match,
            &expression_view.unadjusted_type, receiver->type)
        || !cm_mir_semantic_view_equal(&selection->requested_self_type,
            &expression_view.unadjusted_type)) {
        return 0;
    }
    receiver_kind = declared->data.function_item.signature.receiver;
    if (selected->data.function_item.signature.receiver != receiver_kind
        || declared->data.function_item.signature.parameter_count
            != selection->argument_count
        || selected->data.function_item.signature.parameter_count
            != selection->argument_count
        || selection->argument_count == 0u
        || declared->data.function_item.signature.parameters == NULL
        || selected->data.function_item.signature.parameters == NULL) {
        return 0;
    }
    if (receiver_kind == CM_HIR_RECEIVER_VALUE) {
        return expression_view.adjustment_count == 0u
            && cm_mir_semantic_view_equal(&expression_view.unadjusted_type,
                &expression_view.adjusted_type);
    }
    if ((receiver_kind != CM_HIR_RECEIVER_REF_SHARED
            && receiver_kind != CM_HIR_RECEIVER_REF_MUTABLE)
        || expression_view.adjustment_count != 1u
        || source_body == NULL || receiver->kind != CM_HIR_EXPR_LOCAL
        || receiver->data.local.local_index >= source_body->local_count
        || receiver->data.local.local_index >= match->visible_local_count
        || !cm_mir_hir_local_id(&match->item->data.function_item.signature,
            source_body, &match->parameter_layout,
            receiver->data.local.local_index, &out_receiver->source_local)) {
        return 0;
    }
    memset(&adjustment, 0, sizeof(adjustment));
    if (cm_mir_semantic_adjustment_query(match->semantic_results,
            match->admission, match->body, match->semantic_instance,
            call->data.method_call.receiver, 0u, &adjustment)
                != CM_SEMANTIC_RESULTS_OK
        || adjustment.body != match->body->source_body
        || adjustment.expression != call->data.method_call.receiver
        || adjustment.index != 0u
        || adjustment.has_selected_trait
        || !cm_hir_def_id_is_none(adjustment.selected_trait)
        || !cm_hir_def_id_is_none(adjustment.selected_method)
        || !cm_hir_def_id_is_none(adjustment.selected_impl)
        || !cm_mir_semantic_view_equal(&adjustment.source_type,
            &expression_view.unadjusted_type)
        || !cm_mir_semantic_view_equal(&adjustment.target_type,
            &expression_view.adjusted_type)) {
        return 0;
    }
    if (receiver_kind == CM_HIR_RECEIVER_REF_SHARED) {
        if (adjustment.kind != CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED) {
            return 0;
        }
        out_receiver->borrow_kind = CM_MIR_BORROW_SHARED;
        mutability = CM_HIR_IMMUTABLE;
    } else {
        if (adjustment.kind != CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
            || source_body->locals[receiver->data.local.local_index]
                .mutability != CM_HIR_MUTABLE) {
            return 0;
        }
        out_receiver->borrow_kind = CM_MIR_BORROW_MUTABLE;
        mutability = CM_HIR_MUTABLE;
    }
    out_receiver->source_type = receiver->type;
    out_receiver->target_type =
        selected->data.function_item.signature.parameters[0].type;
    out_receiver->adjusted_type = expression_view.adjusted_type;
    out_receiver->span = receiver->span;
    target = cm_hir_get_type(match->hir, out_receiver->target_type);
    if (target == NULL || target->kind != CM_HIR_TYPE_REFERENCE_KIND
        || target->data.reference_type.region.kind != CM_HIR_REGION_ERASED
        || target->data.reference_type.mutability != mutability
        || !cm_mir_type_equal(match->hir,
            target->data.reference_type.pointee, receiver->type)
        || !cm_mir_type_supported(match->hir, out_receiver->target_type,
            match->pointer_bits)) {
        return 0;
    }
    out_receiver->present = 1;
    return 1;
}

static int cm_mir_adjusted_receiver_assignment_matches(
    CmMirTreeMatch *match, const CmMirAdjustedReceiver *receiver,
    CmMirOperand *out_operand)
{
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;
    const CmMirRvalue *rvalue;
    CmMirLocalId destination;

    if (match == NULL || receiver == NULL || !receiver->present
        || out_operand == NULL
        || match->basic_block_index >= match->body->basic_block_count) {
        return 0;
    }
    block = &match->body->basic_blocks[match->basic_block_index];
    destination = match->next_temporary;
    if (match->statement_index >= block->statement_count
        || !cm_mir_local_id_valid(match->body, destination)
        || match->body->locals[destination].kind != CM_MIR_LOCAL_TEMPORARY
        || !cm_mir_type_equal(match->hir,
            match->body->locals[destination].type, receiver->target_type)) {
        return 0;
    }
    statement = &block->statements[match->statement_index];
    rvalue = &statement->data.assign.value;
    if (statement->kind != CM_MIR_STATEMENT_ASSIGN
        || statement->data.assign.destination != destination
        || cm_mir_place_present(&statement->data.assign.destination_place)
        || rvalue->kind != CM_MIR_RVALUE_BORROW
        || rvalue->data.borrow.kind != receiver->borrow_kind
        || !cm_mir_type_equal(match->hir, rvalue->type,
            receiver->target_type)
        || rvalue->span.source != receiver->span.source
        || rvalue->span.start != receiver->span.start
        || rvalue->span.end != receiver->span.end
        || rvalue->data.borrow.source.base != receiver->source_local
        || !cm_mir_type_equal(match->hir,
            rvalue->data.borrow.source.type, receiver->source_type)
        || rvalue->data.borrow.source.projection_count != 0u
        || rvalue->data.borrow.source.projections != NULL
        || rvalue->data.borrow.source.span.source != receiver->span.source
        || rvalue->data.borrow.source.span.start != receiver->span.start
        || rvalue->data.borrow.source.span.end != receiver->span.end
        || cm_mir_validate_rvalue(match->hir, match->body, rvalue,
            match->pointer_bits) != CM_MIR_OK) {
        return 0;
    }
    memset(out_operand, 0, sizeof(*out_operand));
    out_operand->kind = CM_MIR_OPERAND_MOVE;
    out_operand->type = receiver->target_type;
    out_operand->data.local = destination;
    ++match->statement_index;
    ++match->next_temporary;
    return 1;
}

static int cm_mir_place_equal(const CmHirContext *hir,
    const CmMirPlace *left, const CmMirPlace *right)
{
    uint32_t index;

    if (left->base != right->base
        || !cm_mir_type_equal(hir, left->type, right->type)
        || left->projection_count != right->projection_count
        || left->span.source != right->span.source
        || left->span.start != right->span.start
        || left->span.end != right->span.end
        || (left->projection_count == 0u)
            != (left->projections == NULL)
        || (right->projection_count == 0u)
            != (right->projections == NULL)) {
        return 0;
    }
    for (index = 0u; index < left->projection_count; ++index) {
        if (left->projections[index].kind
                != right->projections[index].kind
            || !cm_hir_def_id_equal(left->projections[index].definition,
                right->projections[index].definition)
            || left->projections[index].field_index
                != right->projections[index].field_index) {
            return 0;
        }
    }
    return 1;
}

static int cm_mir_operand_equal(const CmHirContext *hir,
    const CmMirOperand *actual, const CmMirOperand *expected)
{
    if (actual->kind != expected->kind
        || !cm_mir_type_equal(hir, actual->type, expected->type)) {
        return 0;
    }
    if (actual->kind == CM_MIR_OPERAND_MOVE) {
        return actual->data.local == expected->data.local;
    }
    if (actual->kind == CM_MIR_OPERAND_MOVE_PLACE
        || actual->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_place_equal(hir, &actual->data.place,
            &expected->data.place);
    }
    if (actual->kind == CM_MIR_CONSTANT_U32) {
        return actual->data.u32_value == expected->data.u32_value;
    }
    if (actual->kind == CM_MIR_CONSTANT_USIZE) {
        return actual->data.usize_value == expected->data.usize_value;
    }
    if (actual->kind == CM_MIR_CONSTANT_I32) {
        return actual->data.i32_value == expected->data.i32_value;
    }
    return 0;
}

static int cm_mir_use_assignment_matches(CmMirTreeMatch *match,
    CmMirLocalId destination, CmHirTypeId instantiated,
    const CmMirOperand *operand)
{
    const CmMirBasicBlock *block;
    const CmMirStatement *statement;
    const CmMirRvalue *rvalue;

    if (!cm_mir_local_id_valid(match->body, destination)
        || match->basic_block_index >= match->body->basic_block_count) {
        return 0;
    }
    block = &match->body->basic_blocks[match->basic_block_index];
    if (match->statement_index >= block->statement_count) return 0;
    statement = &block->statements[match->statement_index];
    rvalue = &statement->data.assign.value;
    if (statement->kind != CM_MIR_STATEMENT_ASSIGN
        || statement->data.assign.destination != destination
        || cm_mir_place_present(&statement->data.assign.destination_place)
        || rvalue->kind != CM_MIR_RVALUE_USE
        || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
        || !cm_mir_type_equal(match->hir,
            match->body->locals[destination].type, instantiated)
        || !cm_mir_operand_equal(match->hir, &rvalue->data.use, operand)) {
        return 0;
    }
    ++match->statement_index;
    return 1;
}

static int cm_mir_tuple_parameter_prologue_matches(CmMirTreeMatch *match,
    const CmHirBody *source_body)
{
    const CmHirFunctionSignature *signature;
    uint32_t hir_local_index;
    uint32_t parameter_index;

    if (match == NULL || source_body == NULL) return 0;
    signature = &match->item->data.function_item.signature;
    hir_local_index = 0u;
    for (parameter_index = 0u;
         parameter_index < signature->parameter_count; ++parameter_index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[parameter_index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            const CmHirType *parameter_type;
            CmHirTypeId declared_field;
            CmHirTypeId field_type;
            CmMirPlaceProjection projection;
            CmMirOperand operand;
            CmMirLocalId destination;

            parameter_type = cm_hir_get_type(match->hir, parameter->type);
            if (parameter_type == NULL
                || cm_mir_applied_newtype(match->hir, parameter->type,
                    &declared_field) == NULL
                || !cm_mir_instantiate_executable_type(match->hir,
                    match->item, &match->body->instance, declared_field,
                    &field_type)
                || !cm_mir_hir_local_id(signature, source_body,
                    &match->parameter_layout, hir_local_index,
                    &destination)) {
                return 0;
            }
            memset(&projection, 0, sizeof(projection));
            projection.kind = CM_MIR_PROJECTION_FIELD;
            projection.definition =
                parameter_type->data.named_type.definition;
            projection.field_index = 0u;
            memset(&operand, 0, sizeof(operand));
            operand.kind = CM_MIR_OPERAND_MOVE_PLACE;
            operand.type = field_type;
            operand.data.place.base = parameter_index + 1u;
            operand.data.place.type = field_type;
            operand.data.place.projections = &projection;
            operand.data.place.projection_count = 1u;
            operand.data.place.span = parameter->newtype_binding.span;
            if (!cm_mir_use_assignment_matches(match, destination,
                    field_type, &operand)) {
                return 0;
            }
            hir_local_index += 1u;
            continue;
        }
        if (parameter->binding_kind != CM_HIR_BINDING_TUPLE_PATTERN) {
            return 0;
        }
        {
            const CmHirType *tuple_type;
            uint32_t field_index;

            tuple_type = cm_hir_get_type(match->hir, parameter->type);
            if (tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count
                    != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                return 0;
            }
            for (field_index = 0u;
                 field_index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
                 ++field_index) {
                CmMirPlaceProjection projection;
                CmMirOperand operand;
                CmMirLocalId destination;

                if (!cm_mir_hir_local_id(signature, source_body,
                        &match->parameter_layout, hir_local_index,
                        &destination)) {
                    return 0;
                }
                memset(&projection, 0, sizeof(projection));
                projection.kind = CM_MIR_PROJECTION_FIELD;
                projection.definition = cm_hir_def_id_none();
                projection.field_index = field_index;
                memset(&operand, 0, sizeof(operand));
                operand.kind = CM_MIR_OPERAND_COPY_PLACE;
                operand.type = tuple_type->data.tuple_type
                    .elements[field_index];
                operand.data.place.base = parameter_index + 1u;
                operand.data.place.type = operand.type;
                operand.data.place.projections = &projection;
                operand.data.place.projection_count = 1u;
                operand.data.place.span =
                    parameter->tuple_bindings[field_index].span;
                if (!cm_mir_use_assignment_matches(match, destination,
                        operand.type, &operand)) {
                    return 0;
                }
                hir_local_index += 1u;
            }
        }
    }
    return hir_local_index
        == match->parameter_layout.hir_parameter_local_count;
}

static int cm_mir_expression_matches(CmMirTreeMatch *match,
    CmHirExprId expression_id, int has_destination,
    CmMirLocalId requested_destination, size_t depth,
    CmMirOperand *out_operand)
{
    const CmHirExpr *expression;
    CmSemanticExpressionView authenticated_expression;
    CmHirTypeId instantiated;

    if (depth >= match->hir->expressions.len
        || depth >= CM_MIR_EXPRESSION_RECURSION_LIMIT) {
        return 0;
    }
    expression = cm_hir_get_expr(match->hir, expression_id);
    if (expression == NULL
        || expression->owner_body != match->body->source_body
        || !cm_mir_instantiate_executable_type(match->hir, match->item,
            &match->body->instance, expression->type, &instantiated)) {
        return 0;
    }
    if (match->semantic_results != NULL
        && (match->admission == NULL
            || cm_mir_semantic_expression_query(match->semantic_results,
                match->admission, match->body, match->semantic_instance,
                expression_id, &authenticated_expression)
                    != CM_SEMANTIC_RESULTS_OK
            || authenticated_expression.adjustment_count != 0u
            || !cm_mir_semantic_view_equal(
                &authenticated_expression.unadjusted_type,
                &authenticated_expression.adjusted_type)
            || !cm_mir_semantic_view_matches_hir(match,
                &authenticated_expression.adjusted_type, instantiated))) {
        return 0;
    }
    memset(out_operand, 0, sizeof(*out_operand));
    out_operand->type = instantiated;
    if (expression->kind == CM_HIR_EXPR_BLOCK) {
        const CmHirExpr *tail;

        tail = cm_hir_get_expr(match->hir,
            expression->data.block.tail_expression);
        return expression->data.block.statement_count == 0u
            && expression->data.block.statements == NULL
            && tail != NULL
            && tail->owner_body == match->body->source_body
            && cm_mir_type_equal(match->hir, expression->type, tail->type)
            && cm_mir_span_within(tail->span, expression->span)
            && cm_mir_expression_matches(match,
                expression->data.block.tail_expression, has_destination,
                requested_destination, depth + 1u, out_operand);
    }
    if (expression->kind == CM_HIR_EXPR_LOCAL) {
        CmMirLocalId source_local;

        if (expression->data.local.local_index == UINT32_MAX
            || expression->data.local.local_index
                >= match->visible_local_count
            || !cm_mir_hir_local_id(
                &match->item->data.function_item.signature,
                cm_hir_get_body(match->hir, match->body->source_body),
                &match->parameter_layout,
                expression->data.local.local_index, &source_local)) {
            return 0;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = source_local;
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_INTEGER) {
        if (expression->data.integer.high_bits != 0u
            || (cm_mir_type_is_u8(match->hir, instantiated)
                && expression->data.integer.low_bits > (uint64_t)UINT8_MAX)
            || (cm_mir_type_is_u32(match->hir, instantiated)
                && expression->data.integer.low_bits > (uint64_t)UINT32_MAX)
            || (cm_mir_type_is_i32(match->hir, instantiated)
                && expression->data.integer.low_bits > (uint64_t)INT32_MAX)
            || (cm_mir_type_is_usize(match->hir, instantiated)
                && !cm_mir_usize_value_valid(match->pointer_bits,
                    expression->data.integer.low_bits))
            || (!cm_mir_type_is_fixed_unsigned(match->hir, instantiated)
                && !cm_mir_type_is_i32(match->hir, instantiated)
                && !cm_mir_type_is_usize(match->hir, instantiated))) {
            return 0;
        }
        if (cm_mir_type_is_i32(match->hir, instantiated)) {
            out_operand->kind = CM_MIR_CONSTANT_I32;
            out_operand->data.i32_value =
                (int32_t)expression->data.integer.low_bits;
        } else if (cm_mir_type_is_fixed_unsigned(match->hir,
                instantiated)) {
            out_operand->kind = CM_MIR_CONSTANT_U32;
            out_operand->data.u32_value =
                (uint32_t)expression->data.integer.low_bits;
        } else {
            out_operand->kind = CM_MIR_CONSTANT_USIZE;
            out_operand->data.usize_value =
                expression->data.integer.low_bits;
        }
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_FIELD) {
        CmSemanticFieldSelectionView selection;
        CmSemanticExpressionView authenticated_base;
        CmMirOperand base;
        CmMirPlace place;
        size_t projection_start;
        uint32_t base_projection_count;

        if (match->semantic_results != NULL
            && (cm_mir_semantic_field_selection_query(match,
                    expression_id, &selection) != CM_SEMANTIC_RESULTS_OK
                || selection.body != match->body->source_body
                || selection.expression != expression_id
                || selection.base_expression != expression->data.field.base
                || !cm_hir_def_id_equal(selection.aggregate_definition,
                    expression->data.field.definition)
                || selection.field_index
                    != expression->data.field.field_index
                || cm_mir_semantic_expression_query(match->semantic_results,
                    match->admission, match->body,
                    match->semantic_instance, expression->data.field.base,
                    &authenticated_base) != CM_SEMANTIC_RESULTS_OK
                || authenticated_base.adjustment_count != 0u
                || !cm_mir_semantic_view_equal(&selection.base_type,
                    &authenticated_base.adjusted_type)
                || !cm_mir_semantic_view_equal(&selection.field_type,
                    &authenticated_expression.adjusted_type))) {
            return 0;
        }
        if (!cm_mir_expression_matches(match, expression->data.field.base,
                0, CM_MIR_RETURN_LOCAL, depth + 1u, &base)) {
            return 0;
        }
        memset(&place, 0, sizeof(place));
        if (base.kind == CM_MIR_OPERAND_MOVE) {
            place.base = base.data.local;
            base_projection_count = 0u;
        } else if (base.kind == CM_MIR_OPERAND_MOVE_PLACE
            || base.kind == CM_MIR_OPERAND_COPY_PLACE) {
            place.base = base.data.place.base;
            base_projection_count = base.data.place.projection_count;
        } else {
            return 0;
        }
        if (base_projection_count >= CM_MIR_MAX_PLACE_PROJECTIONS
            || match->expected_projection_count
                > CM_MIR_EXPRESSION_RECURSION_LIMIT
                    - ((size_t)base_projection_count + 1u)) {
            return 0;
        }
        projection_start = match->expected_projection_count;
        place.projections = &match->expected_projections[projection_start];
        place.projection_count = base_projection_count + 1u;
        if (base_projection_count != 0u) {
            memcpy(place.projections, base.data.place.projections,
                (size_t)base_projection_count
                    * sizeof(CmMirPlaceProjection));
        }
        place.projections[base_projection_count].kind =
            CM_MIR_PROJECTION_FIELD;
        place.projections[base_projection_count].definition =
            expression->data.field.definition;
        place.projections[base_projection_count].field_index =
            expression->data.field.field_index;
        match->expected_projection_count += place.projection_count;
        place.type = instantiated;
        place.span = expression->span;
        out_operand->kind = cm_mir_type_is_i32(match->hir, instantiated)
                || cm_mir_type_is_fixed_unsigned(match->hir, instantiated)
            ? CM_MIR_OPERAND_COPY_PLACE : CM_MIR_OPERAND_MOVE_PLACE;
        out_operand->data.place = place;
        if (!cm_mir_place_valid(match->hir, match->body, &place)) return 0;
        if (has_destination) {
            if (!cm_mir_use_assignment_matches(match, requested_destination,
                    instantiated, out_operand)) {
                return 0;
            }
            out_operand->kind = CM_MIR_OPERAND_MOVE;
            out_operand->data.local = requested_destination;
        }
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_AGGREGATE) {
        const CmMirBasicBlock *block;
        const CmMirStatement *statement;
        const CmMirRvalue *rvalue;
        CmMirOperand fields[CM_MIR_MAX_AGGREGATE_FIELDS];
        int seen[CM_MIR_MAX_AGGREGATE_FIELDS];
        CmMirLocalId destination;
        uint32_t index;

        if (expression->data.aggregate.field_count
                > CM_MIR_MAX_AGGREGATE_FIELDS
            || (expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)) {
            return 0;
        }
        memset(fields, 0, sizeof(fields));
        memset(seen, 0, sizeof(seen));
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            const CmHirAggregateFieldValue *field;

            field = &expression->data.aggregate.fields[index];
            if (field->field_index >= expression->data.aggregate.field_count
                || seen[field->field_index]
                || !cm_mir_expression_matches(match, field->value, 0,
                    CM_MIR_RETURN_LOCAL, depth + 1u,
                    &fields[field->field_index])) {
                return 0;
            }
            seen[field->field_index] = 1;
        }
        if (match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        if (match->statement_index >= block->statement_count) return 0;
        statement = &block->statements[match->statement_index];
        rvalue = &statement->data.assign.value;
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || statement->kind != CM_MIR_STATEMENT_ASSIGN
            || statement->data.assign.destination != destination
            || cm_mir_place_present(
                &statement->data.assign.destination_place)
            || rvalue->kind != CM_MIR_RVALUE_AGGREGATE
            || !cm_hir_def_id_equal(rvalue->data.aggregate.definition,
                expression->data.aggregate.definition)
            || rvalue->data.aggregate.field_count
                != expression->data.aggregate.field_count
            || rvalue->span.source != expression->span.source
            || rvalue->span.start != expression->span.start
            || rvalue->span.end != expression->span.end
            || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        for (index = 0u; index < rvalue->data.aggregate.field_count;
             ++index) {
            if (rvalue->data.aggregate.fields[index].field_index != index
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.aggregate.fields[index].value,
                    &fields[index])) {
                return 0;
            }
        }
        ++match->statement_index;
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_BINARY) {
        CmSemanticPrimitiveBinaryView selection;
        CmSemanticExpressionView authenticated_left;
        CmSemanticExpressionView authenticated_right;
        const CmMirBasicBlock *block;
        const CmMirStatement *statement;
        const CmMirRvalue *rvalue;
        CmMirOperand left;
        CmMirOperand right;
        CmMirLocalId destination;

        if (match->semantic_results != NULL
            && (cm_mir_semantic_primitive_binary_query(match,
                    expression_id, &selection) != CM_SEMANTIC_RESULTS_OK
                || selection.body != match->body->source_body
                || selection.expression != expression_id
                || selection.operator_kind
                    != expression->data.binary.operator_kind
                || selection.left_expression
                    != expression->data.binary.left
                || selection.right_expression
                    != expression->data.binary.right
                || cm_mir_semantic_expression_query(match->semantic_results,
                    match->admission, match->body,
                    match->semantic_instance, expression->data.binary.left,
                    &authenticated_left) != CM_SEMANTIC_RESULTS_OK
                || cm_mir_semantic_expression_query(match->semantic_results,
                    match->admission, match->body,
                    match->semantic_instance, expression->data.binary.right,
                    &authenticated_right) != CM_SEMANTIC_RESULTS_OK
                || authenticated_left.adjustment_count != 0u
                || authenticated_right.adjustment_count != 0u
                || !cm_mir_semantic_view_equal(&selection.left_type,
                    &authenticated_left.adjusted_type)
                || !cm_mir_semantic_view_equal(&selection.right_type,
                    &authenticated_right.adjusted_type)
                || !cm_mir_semantic_view_equal(&selection.result_type,
                    &authenticated_expression.adjusted_type))) {
            return 0;
        }
        if ((expression->data.binary.operator_kind != CM_HIR_BINARY_ADD
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_SUBTRACT
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_EQUAL
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_LESS)
            || !cm_mir_expression_matches(match,
                expression->data.binary.left, 0, CM_MIR_RETURN_LOCAL,
                depth + 1u, &left)
            || !cm_mir_expression_matches(match,
                expression->data.binary.right, 0, CM_MIR_RETURN_LOCAL,
                depth + 1u, &right)
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        if (match->statement_index >= block->statement_count) return 0;
        statement = &block->statements[match->statement_index];
        rvalue = &statement->data.assign.value;
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || statement->kind != CM_MIR_STATEMENT_ASSIGN
            || statement->data.assign.destination != destination
            || cm_mir_place_present(
                &statement->data.assign.destination_place)
            || !cm_mir_type_equal(match->hir, rvalue->type, instantiated)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        if (expression->data.binary.operator_kind == CM_HIR_BINARY_EQUAL) {
            if (rvalue->kind != CM_MIR_RVALUE_EQUAL
                || !cm_mir_type_is_bool(match->hir, instantiated)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.equal.left, &left)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.equal.right, &right)) {
                return 0;
            }
        } else if (expression->data.binary.operator_kind
                == CM_HIR_BINARY_LESS) {
            if (rvalue->kind != CM_MIR_RVALUE_LESS
                || !cm_mir_type_is_bool(match->hir, instantiated)
                || !cm_mir_type_is_usize(match->hir, left.type)
                || !cm_mir_type_is_usize(match->hir, right.type)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.less.left, &left)
                || !cm_mir_operand_equal(match->hir,
                    &rvalue->data.less.right, &right)) {
                return 0;
            }
        } else if (rvalue->kind != CM_MIR_RVALUE_BINARY
            || rvalue->data.binary.operator_kind
                != (expression->data.binary.operator_kind
                        == CM_HIR_BINARY_ADD
                    ? CM_MIR_BINARY_ADD : CM_MIR_BINARY_SUBTRACT)
            || !cm_mir_operand_equal(match->hir,
                &rvalue->data.binary.left, &left)
            || !cm_mir_operand_equal(match->hir,
                &rvalue->data.binary.right, &right)) {
            return 0;
        }
        ++match->statement_index;
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_IF) {
        const CmMirBasicBlock *switch_block;
        const CmMirBasicBlock *then_end_block;
        const CmMirBasicBlock *else_end_block;
        const CmMirTerminator *switch_terminator;
        CmMirOperand condition;
        CmMirOperand branch_result;
        CmMirLocalId destination;
        CmMirBasicBlockId switch_block_id;
        CmMirBasicBlockId then_end;
        CmMirBasicBlockId else_end;
        CmMirBasicBlockId join;

        if (expression_id != match->allowed_if_expression
            || !cm_mir_expression_matches(match,
                expression->data.if_expr.condition, 0,
                CM_MIR_RETURN_LOCAL, depth + 1u, &condition)
            || condition.kind != CM_MIR_OPERAND_MOVE
            || !cm_mir_type_is_bool(match->hir, condition.type)
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (!cm_mir_local_id_valid(match->body, destination)
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        switch_block_id = match->basic_block_index;
        switch_block = &match->body->basic_blocks[switch_block_id];
        switch_terminator = &switch_block->terminator;
        if (match->statement_index != switch_block->statement_count
            || switch_terminator->kind != CM_MIR_TERMINATOR_SWITCH_BOOL
            || !cm_mir_operand_equal(match->hir,
                &switch_terminator->data.switch_bool.condition, &condition)
            || switch_terminator->data.switch_bool.true_target
                != switch_block_id + 1u
            || switch_terminator->data.switch_bool.true_target
                >= match->body->basic_block_count
            || switch_terminator->data.switch_bool.false_target
                >= match->body->basic_block_count) {
            return 0;
        }

        match->basic_block_index =
            switch_terminator->data.switch_bool.true_target;
        match->statement_index = 0u;
        if (!cm_mir_expression_matches(match,
                expression->data.if_expr.then_expression, 1, destination,
                depth + 1u, &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        then_end = match->basic_block_index;
        then_end_block = &match->body->basic_blocks[then_end];
        if (match->statement_index != then_end_block->statement_count
            || then_end_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
            || switch_terminator->data.switch_bool.false_target
                != then_end + 1u) {
            return 0;
        }

        match->basic_block_index =
            switch_terminator->data.switch_bool.false_target;
        match->statement_index = 0u;
        if (!cm_mir_expression_matches(match,
                expression->data.if_expr.else_expression, 1, destination,
                depth + 1u, &branch_result)
            || branch_result.kind != CM_MIR_OPERAND_MOVE
            || branch_result.data.local != destination
            || match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        else_end = match->basic_block_index;
        else_end_block = &match->body->basic_blocks[else_end];
        join = then_end_block->terminator.data.goto_block.target;
        if (match->statement_index != else_end_block->statement_count
            || else_end_block->terminator.kind != CM_MIR_TERMINATOR_GOTO
            || else_end_block->terminator.data.goto_block.target != join
            || join != else_end + 1u
            || join >= match->body->basic_block_count) {
            return 0;
        }
        match->basic_block_index = join;
        match->statement_index = 0u;
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        return 1;
    }
    if (expression->kind == CM_HIR_EXPR_CALL
        || expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
        || expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        const CmMirBasicBlock *block;
        const CmMirTerminator *terminator;
        const CmMirBody *callee_body;
        const CmHirExprId *call_arguments;
        CmHirExprId call_argument_storage[2];
        CmMirOperand arguments[2];
        CmMirLocalId destination;
        CmHirDefId callee_definition;
        CmSemanticDirectCallView semantic_call;
        CmSemanticCallableSelectionView semantic_callable;
        CmSemanticCallableSelectionView semantic_callable_hint;
        CmSemanticFunctionSignatureView semantic_signature;
        CmSemanticExpressionView semantic_expression;
        CmSemanticGenericArgumentView semantic_impl_argument;
        CmMirAdjustedReceiver adjusted_receiver;
        uint32_t call_argument_count;
        uint32_t call_substitution_count;
        int selected_call;
        uint32_t index;

        selected_call = expression->kind != CM_HIR_EXPR_CALL;
        call_arguments = NULL;
        call_argument_count = 0u;
        if (!selected_call) {
            call_arguments = expression->data.call.arguments;
            call_argument_count = expression->data.call.argument_count;
        } else if (!cm_mir_callable_arguments(expression,
                call_argument_storage, &call_arguments,
                &call_argument_count)) return 0;
        call_substitution_count = selected_call
            ? 0u : expression->data.call.type_substitution_count;
        callee_definition = selected_call ? cm_hir_def_id_none()
            : expression->data.call.callee;
        memset(&semantic_call, 0, sizeof(semantic_call));
        memset(&semantic_callable, 0, sizeof(semantic_callable));
        memset(&semantic_callable_hint, 0, sizeof(semantic_callable_hint));
        memset(&semantic_signature, 0, sizeof(semantic_signature));
        memset(&semantic_expression, 0, sizeof(semantic_expression));
        memset(&semantic_impl_argument, 0,
            sizeof(semantic_impl_argument));
        memset(&adjusted_receiver, 0, sizeof(adjusted_receiver));
        if (call_argument_count == 0u
            || call_argument_count > 2u
            || call_arguments == NULL
            || (!selected_call
                && expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)) {
            return 0;
        }
        memset(arguments, 0, sizeof(arguments));
        if (expression->kind == CM_HIR_EXPR_METHOD_CALL
            && (match->semantic_results == NULL || match->admission == NULL
                || cm_mir_semantic_callable_hint_query(match, expression_id,
                    &semantic_callable_hint) != CM_SEMANTIC_RESULTS_OK
                || !cm_mir_adjusted_receiver_recipe(match, expression,
                    expression_id, &semantic_callable_hint,
                    &adjusted_receiver))) {
            return 0;
        }
        for (index = 0u; index < call_argument_count; ++index) {
            if (index == 0u && adjusted_receiver.present) {
                if (!cm_mir_adjusted_receiver_assignment_matches(match,
                        &adjusted_receiver, &arguments[0])) {
                    return 0;
                }
                continue;
            }
            if (!cm_mir_expression_matches(match,
                    call_arguments[index], 0,
                    CM_MIR_RETURN_LOCAL,
                    depth + 1u, &arguments[index])
                || !cm_mir_operand_valid(match->hir, match->body,
                    &arguments[index], match->pointer_bits)) {
                return 0;
            }
        }
        if (match->basic_block_index >= match->body->basic_block_count) {
            return 0;
        }
        block = &match->body->basic_blocks[match->basic_block_index];
        terminator = &block->terminator;
        callee_body = terminator->kind == CM_MIR_TERMINATOR_CALL
            ? cm_mir_resolve_body(match->context, match->publication,
                terminator->data.call.callee_instance) : NULL;
        if (selected_call && terminator->kind == CM_MIR_TERMINATOR_CALL) {
            callee_definition = terminator->data.call.callee.definition;
        }
        destination = has_destination ? requested_destination
                                      : match->next_temporary;
        if (match->statement_index != block->statement_count
            || terminator->kind != CM_MIR_TERMINATOR_CALL
            || !cm_mir_local_id_valid(match->body, destination)
            || terminator->data.call.destination != destination
            || cm_mir_place_present(
                &terminator->data.call.destination_place)
            || terminator->data.call.target
                != match->basic_block_index + 1u
            || terminator->data.call.target
                >= match->body->basic_block_count
            || !cm_hir_def_id_equal(callee_definition,
                terminator->data.call.callee.definition)
            || call_argument_count
                != terminator->data.call.argument_count
            || !cm_mir_type_equal(match->hir,
                match->body->locals[destination].type, instantiated)) {
            return 0;
        }
        if (selected_call) {
            const CmHirItem *declared;
            const CmHirExpr *receiver;

            declared = NULL;
            receiver = NULL;
            if (match->semantic_results == NULL || match->admission == NULL
                || callee_body == NULL
                || cm_mir_semantic_callable_query(match, callee_body,
                    expression_id, &semantic_callable)
                        != CM_SEMANTIC_RESULTS_OK
                || semantic_callable.body != match->body->source_body
                || semantic_callable.expression != expression_id
                || cm_hir_def_id_is_none(semantic_callable.selected_impl)
                || cm_hir_def_id_is_none(
                    semantic_callable.selected_callable)
                || !cm_hir_def_id_equal(
                    semantic_callable.selected_callable,
                    terminator->data.call.callee.definition)
                || semantic_callable.argument_count != call_argument_count
                || (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
                    && (semantic_callable.syntax
                            != CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
                        || semantic_callable.syntax
                            != expression->data.qualified_call.syntax
                        || !cm_hir_def_id_equal(
                            semantic_callable.requested_trait,
                            expression->data.qualified_call.requested_trait)
                        || !cm_hir_def_id_equal(
                            semantic_callable.declared_trait_callable,
                            expression->data.qualified_call
                                .declared_trait_callable)
                        || semantic_callable.receiver_argument
                            != expression->data.qualified_call
                                .receiver_argument
                        || !cm_mir_semantic_view_matches_hir(match,
                            &semantic_callable.requested_self_type,
                            expression->data.qualified_call
                                .requested_self_type)))) {
                return 0;
            }
            if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
                CmMirAdjustedReceiver authenticated_receiver;

                memset(&authenticated_receiver, 0,
                    sizeof(authenticated_receiver));
                declared = cm_mir_definition_item(match->hir,
                    semantic_callable.declared_trait_callable);
                receiver = cm_hir_get_expr(match->hir,
                    expression->data.method_call.receiver);
                if (semantic_callable.syntax != CM_HIR_CALLABLE_DOT_METHOD
                    || semantic_callable.syntax
                        != expression->data.method_call.syntax
                    || semantic_callable.receiver_argument != 0u
                    || semantic_callable.receiver_expression
                        != expression->data.method_call.receiver
                    || !cm_mir_method_trait_in_scope(expression,
                        semantic_callable.requested_trait)
                    || declared == NULL
                    || declared->kind != CM_HIR_ITEM_FUNCTION
                    || declared->name
                        != expression->data.method_call.method_name
                    || !cm_hir_def_id_equal(declared->parent_definition,
                        semantic_callable.requested_trait)
                    || receiver == NULL
                    || receiver->owner_body != expression->owner_body
                    || !cm_mir_semantic_view_matches_hir(match,
                        &semantic_callable.requested_self_type,
                        receiver->type)
                    || !cm_mir_adjusted_receiver_recipe(match, expression,
                        expression_id, &semantic_callable,
                        &authenticated_receiver)
                    || authenticated_receiver.present
                        != adjusted_receiver.present
                    || (adjusted_receiver.present
                        && (authenticated_receiver.borrow_kind
                                != adjusted_receiver.borrow_kind
                            || authenticated_receiver.source_type
                                != adjusted_receiver.source_type
                            || authenticated_receiver.target_type
                                != adjusted_receiver.target_type
                            || authenticated_receiver.source_local
                                != adjusted_receiver.source_local
                            || !cm_mir_semantic_view_equal(
                                &authenticated_receiver.adjusted_type,
                                &adjusted_receiver.adjusted_type)))) return 0;
            }
            if (semantic_callable.receiver_argument
                    == CM_HIR_CALLABLE_RECEIVER_NONE) {
                if (semantic_callable.receiver_expression
                        != CM_HIR_EXPR_NONE) return 0;
            } else if (semantic_callable.receiver_argument
                    >= call_argument_count
                || semantic_callable.receiver_expression
                    != call_arguments[semantic_callable.receiver_argument]) {
                return 0;
            }
            callee_definition = semantic_callable.selected_callable;
            if (semantic_callable.item_argument_count != 0u
                || semantic_callable.method_argument_count != 0u
                || semantic_callable.implemented_trait_argument_count != 0u
                || semantic_callable.enclosing_impl_argument_count > 1u) {
                return 0;
            }
            if (semantic_callable.enclosing_impl_argument_count == 1u
                && !cm_hir_def_id_equal(
                    semantic_callable.selected_callable,
                    semantic_callable.declared_trait_callable)) {
                CmHirTypeId callee_substitution;

                if (cm_mir_semantic_callable_generic_argument_query(match,
                        expression_id,
                        CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ENCLOSING_IMPL,
                        0u, &semantic_impl_argument)
                            != CM_SEMANTIC_RESULTS_OK
                    || semantic_impl_argument.kind != CM_HIR_GENERIC_ARG_TYPE
                    || terminator->data.call.callee.substitution_count != 1u
                    || terminator->data.call.callee.substitutions == NULL
                    || cm_semantic_type_view_materialize_existing_hir(
                        match->semantic_results, match->admission,
                        &semantic_impl_argument.normalized,
                        &callee_substitution) != CM_SEMANTIC_RESULTS_OK
                    || !cm_mir_type_is_fixed_unsigned(match->hir,
                        callee_substitution)
                    || !cm_mir_type_equal(match->hir, callee_substitution,
                        terminator->data.call.callee.substitutions[0])) {
                    return 0;
                }
                call_substitution_count = 1u;
            }
        }
        if (call_substitution_count
                != terminator->data.call.callee.substitution_count) {
            return 0;
        }
        if (match->semantic_results != NULL) {
            if (match->admission == NULL
                || callee_body == NULL
                || (!selected_call
                    && cm_mir_semantic_direct_call_query(match, callee_body,
                        expression_id, &semantic_call)
                            != CM_SEMANTIC_RESULTS_OK)
                || cm_mir_semantic_signature_query(
                    match->semantic_results, match->admission, callee_body,
                    NULL,
                    &semantic_signature)
                        != CM_SEMANTIC_RESULTS_OK
                || cm_mir_semantic_expression_query(
                    match->semantic_results, match->admission, match->body,
                    match->semantic_instance, expression_id,
                    &semantic_expression) != CM_SEMANTIC_RESULTS_OK
                || (selected_call
                    ? ((match->body->semantic_evidence
                                == CM_MIR_SEMANTIC_EVIDENCE_BODY
                            && !cm_mir_selected_callee_matches_selection(
                                match, callee_body, &semantic_callable,
                                expression))
                        || !cm_hir_def_id_equal(
                            semantic_signature.definition,
                            semantic_callable.body_definition)
                        || semantic_signature.parameter_count
                            != semantic_callable.argument_count
                        || !cm_mir_semantic_view_equal(
                            &semantic_callable.return_type,
                            &semantic_signature.return_type)
                        || !cm_mir_semantic_view_equal(
                            &semantic_callable.return_type,
                            &semantic_expression.adjusted_type))
                    : (!cm_hir_def_id_equal(semantic_call.callee,
                            terminator->data.call.callee.definition)
                        || !cm_hir_def_id_equal(
                            semantic_signature.definition,
                            semantic_call.callee)
                        || semantic_call.parameter_count
                            != terminator->data.call.argument_count
                        || semantic_signature.parameter_count
                            != semantic_call.parameter_count
                        || !cm_mir_semantic_view_equal(
                            &semantic_call.return_type,
                            &semantic_signature.return_type)
                        || !cm_mir_semantic_view_equal(
                            &semantic_call.return_type,
                            &semantic_expression.adjusted_type)))) {
                return 0;
            }
            if (!cm_mir_semantic_view_matches_hir(match,
                    selected_call ? &semantic_callable.return_type
                              : &semantic_call.return_type,
                    match->body->locals[destination].type)) {
                return 0;
            }
        }
        for (index = 0u; !selected_call
             && index < call_substitution_count; ++index) {
            CmHirTypeId substitution;
            CmHirTypeId callee_substitution;

            if (!cm_mir_instantiate_executable_type(match->hir, match->item,
                    &match->body->instance,
                    expression->data.call.type_substitutions[index],
                    &substitution)) {
                return 0;
            }
            callee_substitution =
                terminator->data.call.callee.substitutions[index];
            if (cm_mir_instance_is_canonical(
                    &terminator->data.call.callee)
                    ? !cm_mir_type_equal(match->hir, substitution,
                        callee_substitution)
                    : substitution != callee_substitution) {
                return 0;
            }
        }
        for (index = 0u; index < call_argument_count; ++index) {
            if (match->semantic_results != NULL) {
                CmSemanticTypeView call_parameter;
                CmSemanticTypeView signature_parameter;
                CmSemanticExpressionView argument_expression;
                CmHirExprId argument_expression_id;

                if ((selected_call
                        ? cm_mir_semantic_callable_parameter_query(match,
                            callee_body, expression_id, index,
                            &call_parameter)
                        : cm_mir_semantic_direct_call_parameter_query(match,
                            callee_body, expression_id, index,
                            &call_parameter)) != CM_SEMANTIC_RESULTS_OK
                    || (selected_call
                        && (cm_mir_semantic_callable_argument_query(match,
                                expression_id, index,
                                &argument_expression_id)
                                != CM_SEMANTIC_RESULTS_OK
                            || argument_expression_id
                                != call_arguments[index]))
                    || cm_mir_semantic_signature_parameter_query(
                        match->semantic_results, match->admission,
                        callee_body, NULL, index,
                        &signature_parameter) != CM_SEMANTIC_RESULTS_OK
                    || cm_mir_semantic_expression_query(
                        match->semantic_results, match->admission,
                        match->body, match->semantic_instance,
                        call_arguments[index],
                        &argument_expression) != CM_SEMANTIC_RESULTS_OK
                    || !cm_mir_semantic_view_equal(&call_parameter,
                        &signature_parameter)
                    || !cm_mir_semantic_view_equal(&call_parameter,
                        &argument_expression.adjusted_type)
                    || (index == 0u && adjusted_receiver.present
                        ? (!cm_mir_semantic_view_equal(&call_parameter,
                                &adjusted_receiver.adjusted_type)
                            || !cm_mir_type_equal(match->hir,
                                terminator->data.call.arguments[index].type,
                                adjusted_receiver.target_type))
                        : !cm_mir_semantic_view_matches_hir(match,
                            &call_parameter,
                            terminator->data.call.arguments[index].type))) {
                    return 0;
                }
            }
            if (!cm_mir_operand_equal(match->hir,
                    &terminator->data.call.arguments[index],
                    &arguments[index])) {
                return 0;
            }
        }
        if (!has_destination) {
            if (match->body->locals[destination].kind
                    != CM_MIR_LOCAL_TEMPORARY
                || destination == UINT32_MAX) {
                return 0;
            }
            ++match->next_temporary;
        }
        out_operand->kind = CM_MIR_OPERAND_MOVE;
        out_operand->data.local = destination;
        match->basic_block_index = terminator->data.call.target;
        match->statement_index = 0u;
        return 1;
    }
    return 0;
}

static int cm_mir_root_shape_valid(const CmMirContext *context,
    const CmMirPublicationImpl *publication,
    const CmHirContext *hir,
    const CmHirItem *item, const CmMirBody *body,
    unsigned int pointer_bits, const CmMirParameterLayout *parameter_layout,
    const CmSemanticAdmission *admission,
    const CmSemanticResults *semantic_results,
    const CmHirInstanceSpec *semantic_instance)
{
    const CmHirBody *source_body;
    const CmHirExpr *root;
    const CmMirBasicBlock *final_block;
    CmMirTreeMatch match;
    CmMirOperand result;
    CmHirExprId root_id;
    uint32_t statement_index;

    source_body = cm_hir_get_body(hir, body->source_body);
    if (source_body == NULL || source_body->state != CM_HIR_BODY_TYPED
        || source_body->root_expression == CM_HIR_EXPR_NONE
        || body->basic_block_count == 0u || parameter_layout == NULL) {
        return 0;
    }
    memset(&match, 0, sizeof(match));
    match.context = context;
    match.publication = publication;
    match.hir = hir;
    match.item = item;
    match.body = body;
    match.admission = admission;
    match.semantic_results = semantic_results;
    match.semantic_instance = semantic_instance;
    match.pointer_bits = pointer_bits;
    match.parameter_layout = *parameter_layout;
    match.next_temporary = parameter_layout->non_temporary_local_count;
    match.visible_local_count =
        parameter_layout->hir_parameter_local_count;
    if (!cm_mir_tuple_parameter_prologue_matches(&match, source_body)) {
        return 0;
    }
    root_id = source_body->root_expression;
    root = cm_hir_get_expr(hir, root_id);
    if (root == NULL) return 0;
    if (root->kind == CM_HIR_EXPR_BLOCK) {
        if (root->owner_body != body->source_body
            || !cm_mir_type_equal(hir, root->type,
                source_body->expected_type)
            || root->data.block.tail_expression == CM_HIR_EXPR_NONE
            || root->data.block.statement_count
                != source_body->local_count
                    - parameter_layout->hir_parameter_local_count
            || (root->data.block.statement_count == 0u)
                != (root->data.block.statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index < root->data.block.statement_count;
             ++statement_index) {
            const CmHirStatement *statement;
            const CmHirLocal *local;
            CmMirLocalId destination;

            statement = &root->data.block.statements[statement_index];
            if (!cm_mir_hir_local_id(
                    &item->data.function_item.signature, source_body,
                    parameter_layout,
                    parameter_layout->hir_parameter_local_count
                        + statement_index,
                    &destination)) {
                return 0;
            }
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.local_index
                    != parameter_layout->hir_parameter_local_count
                        + statement_index
                || statement->data.let_statement.initializer
                    == CM_HIR_EXPR_NONE
                || statement->data.let_statement.local_index
                    >= source_body->local_count) {
                return 0;
            }
            local = &source_body->locals[
                statement->data.let_statement.local_index];
            if (local->parameter_index != CM_HIR_PARAMETER_INDEX_NONE
                || local->mutability != CM_HIR_IMMUTABLE
                || body->locals[destination].kind != CM_MIR_LOCAL_USER
                || !cm_mir_expression_matches(&match,
                    statement->data.let_statement.initializer, 1,
                    destination, 0u, &result)
                || result.kind != CM_MIR_OPERAND_MOVE
                || result.data.local != destination) {
                return 0;
            }
            ++match.visible_local_count;
        }
        root_id = root->data.block.tail_expression;
        root = cm_hir_get_expr(hir, root_id);
        if (root == NULL) return 0;
    } else if (source_body->local_count
            != parameter_layout->hir_parameter_local_count) {
        return 0;
    }
    match.allowed_if_expression = root_id;
    if (match.visible_local_count != source_body->local_count
        || root->owner_body != body->source_body
        || !cm_mir_expression_matches(&match, root_id, 1,
            CM_MIR_RETURN_LOCAL, 0u, &result)
        || result.kind != CM_MIR_OPERAND_MOVE
        || result.data.local != CM_MIR_RETURN_LOCAL
        || match.basic_block_index + 1u != body->basic_block_count) {
        return 0;
    }
    final_block = &body->basic_blocks[match.basic_block_index];
    return match.statement_index == final_block->statement_count
        && final_block->terminator.kind == CM_MIR_TERMINATOR_RETURN
        && match.next_temporary == body->local_count;
}

static int cm_mir_exact_body_shape_valid_impl(const CmMirContext *context,
    const CmMirPublicationImpl *publication, const CmHirContext *hir,
    const CmMirBody *body, int stored,
    const CmSemanticAdmission *admission,
    const CmSemanticResults *semantic_results,
    const CmHirInstanceSpec *semantic_instance)
{
    const CmHirItem *item;
    const CmHirBody *source_body;
    const CmHirFunctionSignature *signature;
    CmSemanticFunctionSignatureView semantic_signature;
    CmMirParameterLayout parameter_layout;
    uint32_t parameter_count;
    CmHirTypeId instantiated;
    uint32_t index;

    if (body == NULL || (body->owned_storage != NULL) != stored
        || (body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_NONE
            && body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_BODY
            && body->semantic_evidence
                != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE)
        || body->basic_block_count == 0u || body->basic_blocks == NULL) {
        return 0;
    }
    item = cm_mir_instance_function(hir, body);
    if (item == NULL || !cm_mir_instance_substitutions_valid(hir, item,
            &body->instance)) {
        return 0;
    }
    signature = &item->data.function_item.signature;
    source_body = cm_hir_get_body(hir, body->source_body);
    parameter_count = signature->parameter_count;
    memset(&semantic_signature, 0, sizeof(semantic_signature));
    if (semantic_results != NULL) {
        if (admission == NULL
            || cm_mir_semantic_signature_query(semantic_results, admission,
                body, semantic_instance, &semantic_signature)
                    != CM_SEMANTIC_RESULTS_OK
            || !cm_hir_def_id_equal(semantic_signature.definition,
                body->instance.body_definition)
            || semantic_signature.parameter_count == UINT32_MAX) {
            return 0;
        }
        parameter_count = semantic_signature.parameter_count;
    }
    if (signature->is_variadic
        || (signature->parameter_count != 0u
            && signature->parameters == NULL)
        || parameter_count == UINT32_MAX
        || parameter_count != signature->parameter_count
        || source_body == NULL
        || source_body->parameter_count != parameter_count
        || source_body->local_count == UINT32_MAX
        || !cm_mir_parameter_layout(hir, item, source_body,
            semantic_results == NULL,
            &parameter_layout)
        || body->local_count
            < parameter_layout.non_temporary_local_count
        || body->locals == NULL) {
        return 0;
    }
    if (semantic_results == NULL) {
        if (!cm_mir_instantiate_executable_type(hir, item, &body->instance,
                signature->return_type, &instantiated)) return 0;
    } else {
        if (!cm_mir_semantic_view_matches(semantic_results, admission,
                &semantic_signature.return_type, body->locals[0].type)) {
            return 0;
        }
        instantiated = body->locals[0].type;
    }
    if (!cm_mir_instance_type_supported(hir, item, &body->instance,
            instantiated, context->pointer_bits)
        || !cm_mir_type_target_valid(hir, instantiated,
            context->pointer_bits, 0u)
        || cm_mir_type_is_bool(hir, instantiated)
        || body->locals[0].kind != CM_MIR_LOCAL_RETURN
        || (semantic_results == NULL
            && body->locals[0].type != instantiated)) {
        return 0;
    }
    for (index = 0u; index < parameter_count; ++index) {
        CmSemanticTypeView semantic_parameter;

        memset(&semantic_parameter, 0, sizeof(semantic_parameter));
        if (semantic_results == NULL) {
            if (!cm_mir_instantiate_executable_type(hir, item,
                    &body->instance, signature->parameters[index].type,
                    &instantiated)) return 0;
        } else {
            if (cm_mir_semantic_signature_parameter_query(
                    semantic_results, admission, body, semantic_instance,
                    index, &semantic_parameter) != CM_SEMANTIC_RESULTS_OK
                || !cm_mir_semantic_view_matches(semantic_results,
                    admission, &semantic_parameter,
                    body->locals[index + 1u].type)) return 0;
            instantiated = body->locals[index + 1u].type;
        }
        if (!cm_mir_instance_type_supported(hir, item, &body->instance,
                instantiated, context->pointer_bits)
            || !cm_mir_type_target_valid(hir, instantiated,
                context->pointer_bits, 0u)
            || cm_mir_type_is_bool(hir, instantiated)
            || body->locals[index + 1u].kind != CM_MIR_LOCAL_ARGUMENT
            || body->locals[index + 1u].type != instantiated) {
            return 0;
        }
    }
    for (index = 0u; index < source_body->local_count; ++index) {
        CmMirLocalId mapped_local;
        CmMirLocalKind expected_kind;

        if (!cm_mir_hir_local_id(signature, source_body,
                &parameter_layout, index, &mapped_local)
            || (index >= parameter_layout.hir_parameter_local_count
                && (source_body->locals[index].parameter_index
                        != CM_HIR_PARAMETER_INDEX_NONE
                    || source_body->locals[index].mutability
                        != CM_HIR_IMMUTABLE))
            || !cm_mir_instantiate_executable_type(hir, item,
                &body->instance,
                source_body->locals[index].type, &instantiated)
            || !cm_mir_instance_type_supported(hir, item, &body->instance,
                instantiated, context->pointer_bits)
            || !cm_mir_type_target_valid(hir, instantiated,
                context->pointer_bits, 0u)
            || body->locals[mapped_local].type != instantiated) {
            return 0;
        }
        expected_kind = mapped_local <= parameter_count
            ? CM_MIR_LOCAL_ARGUMENT : CM_MIR_LOCAL_USER;
        if (body->locals[mapped_local].kind != expected_kind) return 0;
    }
    for (index = parameter_layout.non_temporary_local_count;
        index < body->local_count; ++index) {
        if (body->locals[index].kind != CM_MIR_LOCAL_TEMPORARY
            || !cm_mir_type_supported(hir, body->locals[index].type,
                context->pointer_bits)
            || !cm_mir_type_target_valid(hir,
                body->locals[index].type, context->pointer_bits, 0u)) {
            return 0;
        }
    }

    for (index = 0u; index < body->basic_block_count; ++index) {
        const CmMirBasicBlock *block;
        uint32_t statement_index;

        block = &body->basic_blocks[index];
        if ((block->statement_count == 0u) != (block->statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index < block->statement_count; ++statement_index) {
            const CmMirStatement *statement;
            const CmMirRvalue *rvalue;
            CmHirTypeId destination_type;

            statement = &block->statements[statement_index];
            if (statement->kind != CM_MIR_STATEMENT_ASSIGN
                || !cm_mir_destination_type(hir, body,
                    statement->data.assign.destination,
                    &statement->data.assign.destination_place,
                    &destination_type)) {
                return 0;
            }
            rvalue = &statement->data.assign.value;
            if (!cm_mir_rvalue_valid(hir, body, rvalue,
                    context->pointer_bits)
                || !cm_mir_type_equal(hir,
                    destination_type, rvalue->type)) {
                return 0;
            }
        }

        if (block->terminator.kind == CM_MIR_TERMINATOR_CALL) {
            const CmMirBody *callee;
            const CmHirItem *callee_item;
            const CmMirTerminator *terminator;
            CmHirTypeId destination_type;
            int has_aggregate_argument;
            uint32_t argument_index;

            terminator = &block->terminator;
            callee = cm_mir_resolve_body(context, publication,
                terminator->data.call.callee_instance);
            callee_item = callee == NULL ? NULL
                : cm_mir_instance_function(hir, callee);
            if (callee == NULL || cm_mir_instance_is_empty(&callee->instance)
                || callee_item == NULL || callee->local_count == 0u
                || callee->locals == NULL
                || callee->local_count
                    <= terminator->data.call.argument_count
                || !cm_mir_instance_equal(&terminator->data.call.callee,
                    &callee->instance)
                || !cm_mir_destination_type(hir, body,
                    terminator->data.call.destination,
                    &terminator->data.call.destination_place,
                    &destination_type)
                || terminator->data.call.target >= body->basic_block_count
                || (semantic_results == NULL
                    && terminator->data.call.argument_count
                        != callee_item->data.function_item.signature
                            .parameter_count)
                || (terminator->data.call.argument_count != 0u
                    && terminator->data.call.arguments == NULL)
                || !cm_mir_type_equal(hir,
                    destination_type,
                    callee->locals[CM_MIR_RETURN_LOCAL].type)
                || !cm_mir_type_is_unsigned_scalar(hir,
                    callee->locals[CM_MIR_RETURN_LOCAL].type,
                    context->pointer_bits)) {
                return 0;
            }
            has_aggregate_argument = 0;
            for (argument_index = 0u;
                 argument_index < terminator->data.call.argument_count;
                 ++argument_index) {
                const CmMirOperand *argument;
                CmHirTypeId parameter_type;

                argument = &terminator->data.call.arguments[argument_index];
                parameter_type = callee->locals[argument_index + 1u].type;
                if ((!cm_mir_type_is_unsigned_scalar(hir, parameter_type,
                            context->pointer_bits)
                        && !cm_mir_type_is_erased_reference(hir,
                            parameter_type, context->pointer_bits)
                        && !cm_mir_type_is_checked_aggregate(hir,
                            parameter_type,
                            callee_item->definition.crate_id))
                    || !cm_mir_operand_valid(hir, body, argument,
                        context->pointer_bits)
                    || argument->kind == CM_MIR_CONSTANT_U32
                    || !cm_mir_type_equal(hir, argument->type,
                        parameter_type)) {
                    return 0;
                }
                if (cm_mir_type_is_checked_aggregate(hir, parameter_type,
                        callee_item->definition.crate_id)) {
                    has_aggregate_argument = 1;
                }
            }
            if (has_aggregate_argument
                && (callee_item->generic_parameter_count != 0u
                    || callee->instance.substitution_count != 0u
                    || terminator->data.call.callee.substitution_count
                        != 0u
                    || callee_item->definition.crate_id
                        != item->definition.crate_id)) {
                return 0;
            }
        } else if (block->terminator.kind == CM_MIR_TERMINATOR_GOTO) {
            if (block->terminator.data.goto_block.target
                    >= body->basic_block_count) {
                return 0;
            }
        } else if (block->terminator.kind
                == CM_MIR_TERMINATOR_SWITCH_BOOL) {
            const CmMirOperand *condition;

            condition = &block->terminator.data.switch_bool.condition;
            if (condition->kind != CM_MIR_OPERAND_MOVE
                || !cm_mir_type_is_bool(hir, condition->type)
                || !cm_mir_move_operand_valid(hir, body, condition)
                || block->terminator.data.switch_bool.true_target
                    >= body->basic_block_count
                || block->terminator.data.switch_bool.false_target
                    >= body->basic_block_count
                || block->terminator.data.switch_bool.true_target
                    == block->terminator.data.switch_bool.false_target) {
                return 0;
            }
        } else if (block->terminator.kind != CM_MIR_TERMINATOR_RETURN) {
            return 0;
        }
    }
    return cm_mir_root_shape_valid(context, publication, hir, item, body,
        context->pointer_bits, &parameter_layout, admission,
        semantic_results, semantic_instance);
}

static int cm_mir_exact_body_shape_valid_with_publication(
    const CmMirContext *context, const CmMirPublicationImpl *publication,
    const CmHirContext *hir, const CmMirBody *body, int stored,
    const CmSemanticAdmission *admission,
    const CmSemanticResults *semantic_results)
{
    CmMirSemanticInstanceQuery query;
    const CmHirInstanceSpec *instance;
    int valid;

    memset(&query, 0, sizeof(query));
    instance = NULL;
    if (semantic_results != NULL && body != NULL
        && body->semantic_evidence
            == CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
        && !cm_mir_instance_is_canonical(&body->instance)) {
        if (!cm_mir_semantic_instance_query_init(&query, hir, body)) return 0;
        instance = &query.spec;
    }
    valid = cm_mir_exact_body_shape_valid_impl(context, publication, hir,
        body, stored, admission, semantic_results, instance);
    cm_mir_semantic_instance_query_destroy(&query);
    return valid;
}

static int cm_mir_exact_body_shape_valid(const CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, int stored,
    const CmSemanticAdmission *admission,
    const CmSemanticResults *semantic_results)
{
    return cm_mir_exact_body_shape_valid_with_publication(context, NULL,
        hir, body, stored, admission, semantic_results);
}

static int cm_mir_storage_add(size_t *total, size_t count,
    size_t element_size)
{
    size_t remainder;
    size_t padding;
    size_t bytes;
    size_t aligned;

    if (count == 0u) return 1;
    remainder = *total % CM_MIR_STORAGE_ALIGNMENT;
    padding = remainder == 0u ? 0u : CM_MIR_STORAGE_ALIGNMENT - remainder;
    return cm_size_add(*total, padding, &aligned)
        && cm_size_mul(count, element_size, &bytes)
        && cm_size_add(aligned, bytes, total);
}

static void *cm_mir_storage_take(unsigned char *storage, size_t *offset,
    size_t count, size_t element_size)
{
    size_t remainder;

    if (count == 0u) return NULL;
    remainder = *offset % CM_MIR_STORAGE_ALIGNMENT;
    if (remainder != 0u) *offset += CM_MIR_STORAGE_ALIGNMENT - remainder;
    {
        void *result;

        result = storage + *offset;
        *offset += count * element_size;
        return result;
    }
}

static int cm_mir_place_storage_size(size_t *total,
    const CmMirPlace *place)
{
    if (!cm_mir_place_present(place)) return 1;
    return place->projection_count <= CM_MIR_MAX_PLACE_PROJECTIONS
        && (place->projection_count == 0u)
            == (place->projections == NULL)
        && cm_mir_storage_add(total, place->projection_count,
            sizeof(CmMirPlaceProjection));
}

static int cm_mir_operand_storage_size(size_t *total,
    const CmMirOperand *operand)
{
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        return cm_mir_place_storage_size(total, &operand->data.place);
    }
    return operand->kind == CM_MIR_CONSTANT_I32
        || operand->kind == CM_MIR_CONSTANT_U32
        || operand->kind == CM_MIR_CONSTANT_USIZE
        || operand->kind == CM_MIR_OPERAND_MOVE;
}

static int cm_mir_rvalue_storage_size(size_t *total,
    const CmMirRvalue *rvalue)
{
    uint32_t index;

    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        return cm_mir_operand_storage_size(total, &rvalue->data.use);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.binary.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.binary.right);
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.equal.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.equal.right);
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        return cm_mir_operand_storage_size(total,
                &rvalue->data.less.left)
            && cm_mir_operand_storage_size(total,
                &rvalue->data.less.right);
    }
    if (rvalue->kind == CM_MIR_RVALUE_BORROW) {
        return cm_mir_place_storage_size(total,
            &rvalue->data.borrow.source);
    }
    if (rvalue->kind != CM_MIR_RVALUE_AGGREGATE
        || rvalue->data.aggregate.field_count > CM_MIR_MAX_AGGREGATE_FIELDS
        || (rvalue->data.aggregate.field_count == 0u)
            != (rvalue->data.aggregate.fields == NULL)
        || !cm_mir_storage_add(total,
            rvalue->data.aggregate.field_count,
            sizeof(CmMirAggregateField))) {
        return 0;
    }
    for (index = 0u; index < rvalue->data.aggregate.field_count; ++index) {
        if (!cm_mir_operand_storage_size(total,
                &rvalue->data.aggregate.fields[index].value)) {
            return 0;
        }
    }
    return 1;
}

static void cm_mir_copy_place(unsigned char *storage, size_t *offset,
    const CmMirPlace *source, CmMirPlace *copy)
{
    *copy = *source;
    copy->projections = (CmMirPlaceProjection *)cm_mir_storage_take(storage,
        offset, source->projection_count, sizeof(CmMirPlaceProjection));
    if (source->projection_count != 0u) {
        memcpy(copy->projections, source->projections,
            (size_t)source->projection_count
                * sizeof(CmMirPlaceProjection));
    }
}

static void cm_mir_copy_operand(unsigned char *storage, size_t *offset,
    const CmMirOperand *source, CmMirOperand *copy)
{
    *copy = *source;
    if (source->kind == CM_MIR_OPERAND_MOVE_PLACE
        || source->kind == CM_MIR_OPERAND_COPY_PLACE) {
        cm_mir_copy_place(storage, offset, &source->data.place,
            &copy->data.place);
    }
}

static void cm_mir_copy_rvalue(unsigned char *storage, size_t *offset,
    const CmMirRvalue *source, CmMirRvalue *copy)
{
    uint32_t index;

    *copy = *source;
    if (source->kind == CM_MIR_RVALUE_USE) {
        cm_mir_copy_operand(storage, offset, &source->data.use,
            &copy->data.use);
    } else if (source->kind == CM_MIR_RVALUE_BINARY) {
        cm_mir_copy_operand(storage, offset, &source->data.binary.left,
            &copy->data.binary.left);
        cm_mir_copy_operand(storage, offset, &source->data.binary.right,
            &copy->data.binary.right);
    } else if (source->kind == CM_MIR_RVALUE_EQUAL) {
        cm_mir_copy_operand(storage, offset, &source->data.equal.left,
            &copy->data.equal.left);
        cm_mir_copy_operand(storage, offset, &source->data.equal.right,
            &copy->data.equal.right);
    } else if (source->kind == CM_MIR_RVALUE_LESS) {
        cm_mir_copy_operand(storage, offset, &source->data.less.left,
            &copy->data.less.left);
        cm_mir_copy_operand(storage, offset, &source->data.less.right,
            &copy->data.less.right);
    } else if (source->kind == CM_MIR_RVALUE_BORROW) {
        cm_mir_copy_place(storage, offset, &source->data.borrow.source,
            &copy->data.borrow.source);
    } else if (source->kind == CM_MIR_RVALUE_AGGREGATE) {
        copy->data.aggregate.fields = (CmMirAggregateField *)
            cm_mir_storage_take(storage, offset,
                source->data.aggregate.field_count,
                sizeof(CmMirAggregateField));
        for (index = 0u; index < source->data.aggregate.field_count;
             ++index) {
            copy->data.aggregate.fields[index] =
                source->data.aggregate.fields[index];
            cm_mir_copy_operand(storage, offset,
                &source->data.aggregate.fields[index].value,
                &copy->data.aggregate.fields[index].value);
        }
    }
}

static int cm_mir_body_storage_size(const CmMirBody *body, size_t *out_size)
{
    size_t total;
    uint32_t block_index;

    if (body == NULL
        || !cm_mir_instance_valid(&body->instance)
        || body->local_count == 0u || body->locals == NULL
        || body->basic_block_count == 0u || body->basic_blocks == NULL) {
        return 0;
    }
    total = 0u;
    if (!cm_mir_storage_add(&total, body->instance.substitution_count,
            sizeof(CmHirTypeId))
        || !cm_mir_storage_add(&total, body->instance.identity_size,
            sizeof(unsigned char))
        || !cm_mir_storage_add(&total, body->local_count,
            sizeof(CmMirLocal))
        || !cm_mir_storage_add(&total, body->basic_block_count,
            sizeof(CmMirBasicBlock))) {
        return 0;
    }
    for (block_index = 0u; block_index < body->basic_block_count;
         ++block_index) {
        const CmMirTerminator *terminator;
        uint32_t statement_index;
        uint32_t argument_index;

        if (!cm_mir_storage_add(&total,
                body->basic_blocks[block_index].statement_count,
                sizeof(CmMirStatement))) {
            return 0;
        }
        if ((body->basic_blocks[block_index].statement_count == 0u)
                != (body->basic_blocks[block_index].statements == NULL)) {
            return 0;
        }
        for (statement_index = 0u;
             statement_index
                < body->basic_blocks[block_index].statement_count;
             ++statement_index) {
            const CmMirStatement *statement;

            statement = &body->basic_blocks[block_index]
                .statements[statement_index];
            if (statement->kind != CM_MIR_STATEMENT_ASSIGN
                || !cm_mir_place_storage_size(&total,
                    &statement->data.assign.destination_place)
                || !cm_mir_rvalue_storage_size(&total,
                    &statement->data.assign.value)) {
                return 0;
            }
        }
        terminator = &body->basic_blocks[block_index].terminator;
        if (terminator->kind == CM_MIR_TERMINATOR_CALL
            && ((terminator->data.call.argument_count == 0u)
                    != (terminator->data.call.arguments == NULL)
                || !cm_mir_instance_valid(
                    &terminator->data.call.callee)
                || cm_mir_instance_is_empty(
                    &terminator->data.call.callee))) {
            return 0;
        }
        if (terminator->kind == CM_MIR_TERMINATOR_CALL
            && (!cm_mir_storage_add(&total,
                    terminator->data.call.argument_count,
                    sizeof(CmMirOperand))
                || !cm_mir_storage_add(&total,
                    terminator->data.call.callee.substitution_count,
                    sizeof(CmHirTypeId))
                || !cm_mir_storage_add(&total,
                    terminator->data.call.callee.identity_size,
                    sizeof(unsigned char)))) {
            return 0;
        }
        if (terminator->kind == CM_MIR_TERMINATOR_CALL) {
            if (!cm_mir_place_storage_size(&total,
                    &terminator->data.call.destination_place)) {
                return 0;
            }
            for (argument_index = 0u;
                 argument_index < terminator->data.call.argument_count;
                 ++argument_index) {
                if (!cm_mir_operand_storage_size(&total,
                        &terminator->data.call.arguments[argument_index])) {
                    return 0;
                }
            }
        } else if (terminator->kind == CM_MIR_TERMINATOR_SWITCH_BOOL
            && !cm_mir_operand_storage_size(&total,
                &terminator->data.switch_bool.condition)) {
            return 0;
        }
    }
    *out_size = total;
    return 1;
}

static CmMirStatus cm_mir_copy_body(const CmMirBody *body, CmMirBody *copy)
{
    unsigned char *storage;
    size_t storage_size;
    size_t offset;
    uint32_t block_index;

    if (!cm_mir_body_storage_size(body, &storage_size)) {
        return CM_MIR_ID_EXHAUSTED;
    }
    storage = (unsigned char *)cm_alloc(storage_size);
    memset(copy, 0, sizeof(*copy));
    copy->owner = body->owner;
    copy->source_body = body->source_body;
    copy->semantic_evidence = body->semantic_evidence;
    copy->instance.definition = body->instance.definition;
    copy->instance.body_definition = body->instance.body_definition;
    copy->instance.substitution_count = body->instance.substitution_count;
    copy->instance.body = body->instance.body;
    copy->instance.identity_size = body->instance.identity_size;
    copy->local_count = body->local_count;
    copy->basic_block_count = body->basic_block_count;
    copy->owned_storage = storage;
    offset = 0u;
    copy->instance.substitutions = (CmHirTypeId *)cm_mir_storage_take(
        storage, &offset, body->instance.substitution_count,
        sizeof(CmHirTypeId));
    if (body->instance.substitution_count != 0u) {
        memcpy(copy->instance.substitutions, body->instance.substitutions,
            (size_t)body->instance.substitution_count * sizeof(CmHirTypeId));
    }
    copy->instance.identity_bytes = (unsigned char *)cm_mir_storage_take(
        storage, &offset, body->instance.identity_size,
        sizeof(unsigned char));
    if (body->instance.identity_size != 0u) {
        memcpy(copy->instance.identity_bytes, body->instance.identity_bytes,
            body->instance.identity_size);
    }
    copy->locals = (CmMirLocal *)cm_mir_storage_take(storage, &offset,
        body->local_count, sizeof(CmMirLocal));
    memcpy(copy->locals, body->locals,
        (size_t)body->local_count * sizeof(CmMirLocal));
    copy->basic_blocks = (CmMirBasicBlock *)cm_mir_storage_take(storage,
        &offset, body->basic_block_count, sizeof(CmMirBasicBlock));
    memset(copy->basic_blocks, 0,
        (size_t)body->basic_block_count * sizeof(CmMirBasicBlock));

    for (block_index = 0u; block_index < body->basic_block_count;
         ++block_index) {
        const CmMirBasicBlock *source_block;
        CmMirBasicBlock *copy_block;

        source_block = &body->basic_blocks[block_index];
        copy_block = &copy->basic_blocks[block_index];
        copy_block->statement_count = source_block->statement_count;
        copy_block->statements = (CmMirStatement *)cm_mir_storage_take(
            storage, &offset, source_block->statement_count,
            sizeof(CmMirStatement));
        if (source_block->statement_count != 0u) {
            memcpy(copy_block->statements, source_block->statements,
                (size_t)source_block->statement_count
                    * sizeof(CmMirStatement));
        }
        {
            uint32_t statement_index;

            for (statement_index = 0u;
                 statement_index < source_block->statement_count;
                 ++statement_index) {
                const CmMirStatement *source_statement;
                CmMirStatement *copy_statement;

                source_statement = &source_block->statements[statement_index];
                copy_statement = &copy_block->statements[statement_index];
                if (cm_mir_place_present(
                        &source_statement->data.assign.destination_place)) {
                    cm_mir_copy_place(storage, &offset,
                        &source_statement->data.assign.destination_place,
                        &copy_statement->data.assign.destination_place);
                }
                cm_mir_copy_rvalue(storage, &offset,
                    &source_statement->data.assign.value,
                    &copy_statement->data.assign.value);
            }
        }
        copy_block->terminator = source_block->terminator;
        if (source_block->terminator.kind == CM_MIR_TERMINATOR_CALL) {
            const CmMirTerminator *source_terminator;
            CmMirTerminator *copy_terminator;

            source_terminator = &source_block->terminator;
            copy_terminator = &copy_block->terminator;
            if (cm_mir_place_present(
                    &source_terminator->data.call.destination_place)) {
                cm_mir_copy_place(storage, &offset,
                    &source_terminator->data.call.destination_place,
                    &copy_terminator->data.call.destination_place);
            }
            copy_terminator->data.call.arguments =
                (CmMirOperand *)cm_mir_storage_take(storage, &offset,
                    source_terminator->data.call.argument_count,
                    sizeof(CmMirOperand));
            if (source_terminator->data.call.argument_count != 0u) {
                uint32_t argument_index;

                for (argument_index = 0u;
                     argument_index
                        < source_terminator->data.call.argument_count;
                     ++argument_index) {
                    cm_mir_copy_operand(storage, &offset,
                        &source_terminator->data.call
                            .arguments[argument_index],
                        &copy_terminator->data.call
                            .arguments[argument_index]);
                }
            }
            copy_terminator->data.call.callee.substitutions =
                (CmHirTypeId *)cm_mir_storage_take(storage, &offset,
                    source_terminator->data.call.callee.substitution_count,
                    sizeof(CmHirTypeId));
            if (source_terminator->data.call.callee.substitution_count != 0u) {
                memcpy(copy_terminator->data.call.callee.substitutions,
                    source_terminator->data.call.callee.substitutions,
                    (size_t)source_terminator->data.call.callee
                        .substitution_count * sizeof(CmHirTypeId));
            }
            copy_terminator->data.call.callee.identity_bytes =
                (unsigned char *)cm_mir_storage_take(storage, &offset,
                    source_terminator->data.call.callee.identity_size,
                    sizeof(unsigned char));
            if (source_terminator->data.call.callee.identity_size != 0u) {
                memcpy(copy_terminator->data.call.callee.identity_bytes,
                    source_terminator->data.call.callee.identity_bytes,
                    source_terminator->data.call.callee.identity_size);
            }
        } else if (source_block->terminator.kind
                == CM_MIR_TERMINATOR_SWITCH_BOOL) {
            cm_mir_copy_operand(storage, &offset,
                &source_block->terminator.data.switch_bool.condition,
                &copy_block->terminator.data.switch_bool.condition);
        }
    }
    return CM_MIR_OK;
}

static void cm_mir_body_storage_destroy(CmMirBody *body)
{
    if (body == NULL) return;
    cm_free(body->owned_storage);
    memset(body, 0, sizeof(*body));
}

static int cm_mir_publication_impl_valid(
    const CmMirPublicationImpl *publication)
{
    return publication != NULL && publication->context != NULL
        && publication->admission != NULL && publication->hir != NULL
        && publication->crate_id != CM_HIR_CRATE_NONE
        && publication->entries.elem_size == sizeof(CmMirPublicationEntry)
        && publication->entries.len <= publication->entries.cap
        && (publication->entries.cap == 0u)
            == (publication->entries.data == NULL);
}

static int cm_mir_publication_current(
    const CmMirPublicationImpl *publication)
{
    return cm_mir_publication_impl_valid(publication)
        && cm_mir_context_valid(publication->context)
        && cm_semantic_admission_is_current(publication->admission)
        && cm_semantic_admission_hir(publication->admission)
            == publication->hir
        && cm_semantic_admission_crate(publication->admission)
            == publication->crate_id
        && cm_semantic_admission_capability_id(publication->admission)
            == publication->admission_capability_id
        && cm_semantic_admission_barrier_capability_id(
            publication->admission) == publication->barrier_capability_id
        && cm_semantic_admission_parent_capability_id(
            publication->admission) == publication->parent_capability_id
        && publication->context->lifetime_id
            == publication->context_lifetime_id
        && publication->context->bodies.len
            == publication->context_body_count
        && publication->context->pointer_bits == publication->pointer_bits
        && publication->hir->storage.lifetime_id
            == publication->storage_lifetime_id
        && publication->hir->semantic_generation
            == publication->semantic_generation
        && publication->hir->rewind_generation
            == publication->rewind_generation
        && cm_mir_context_accepts_admission(publication->context,
            publication->admission, publication->hir,
            publication->crate_id);
}

static const CmMirPublicationEntry *cm_mir_publication_entry(
    const CmMirPublicationImpl *publication, CmMirBodyId id)
{
    size_t index;

    if (!cm_mir_publication_impl_valid(publication)
        || id == CM_MIR_BODY_NONE
        || (size_t)id <= publication->context_body_count) return NULL;
    index = (size_t)id - publication->context_body_count - 1u;
    return (const CmMirPublicationEntry *)cm_vec_at_const(
        &publication->entries, index);
}

static CmMirPublicationEntry *cm_mir_publication_entry_mut(
    CmMirPublicationImpl *publication, CmMirBodyId id)
{
    return (CmMirPublicationEntry *)cm_mir_publication_entry(publication,
        id);
}

static const CmMirBody *cm_mir_resolve_body(const CmMirContext *context,
    const CmMirPublicationImpl *publication, CmMirBodyId id)
{
    const CmMirPublicationEntry *entry;

    if (publication == NULL || (size_t)id <= context->bodies.len) {
        return cm_mir_get_body(context, id);
    }
    entry = cm_mir_publication_entry(publication, id);
    return entry != NULL && entry->defined ? &entry->body : NULL;
}

void cm_mir_context_init(CmMirContext *context)
{
    if (context == NULL) return;
    memset(context, 0, sizeof(*context));
    cm_vec_init(&context->bodies, sizeof(CmMirBody));
    context->lifetime_id = cm_mir_new_context_lifetime_id();
}

void cm_mir_context_destroy(CmMirContext *context)
{
    size_t index;

    if (context == NULL) return;
    if (context->bodies.elem_size == sizeof(CmMirBody)) {
        for (index = 0u; index < context->bodies.len; ++index) {
            cm_mir_body_storage_destroy((CmMirBody *)cm_vec_at(
                &context->bodies, index));
        }
        cm_vec_destroy(&context->bodies);
    }
    memset(context, 0, sizeof(*context));
}

void cm_mir_publication_init(CmMirPublication *publication)
{
    if (publication == NULL) return;
    publication->implementation = NULL;
}

void cm_mir_publication_destroy(CmMirPublication *publication)
{
    CmMirPublicationImpl *implementation;
    size_t index;

    if (publication == NULL) return;
    implementation = (CmMirPublicationImpl *)publication->implementation;
    if (implementation != NULL
        && implementation->entries.elem_size
            == sizeof(CmMirPublicationEntry)) {
        for (index = 0u; index < implementation->entries.len; ++index) {
            CmMirPublicationEntry *entry;

            entry = (CmMirPublicationEntry *)cm_vec_at(
                &implementation->entries, index);
            if (entry != NULL) {
                cm_mir_instance_standalone_destroy(&entry->instance);
                cm_mir_body_storage_destroy(&entry->body);
            }
        }
        cm_vec_destroy(&implementation->entries);
    }
    cm_free(implementation);
    publication->implementation = NULL;
}

CmMirStatus cm_mir_publication_begin(CmMirPublication *publication,
    CmMirContext *context, const CmSemanticAdmission *admission)
{
    CmMirPublicationImpl *implementation;
    const CmHirContext *hir;
    CmHirCrateId crate_id;

    if (publication == NULL || publication->implementation != NULL
        || context == NULL) return CM_MIR_INVALID_ARGUMENT;
    if (!cm_mir_admission_identity(admission, &hir, &crate_id)
        || !cm_mir_context_accepts_admission(context, admission, hir,
            crate_id)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    implementation = (CmMirPublicationImpl *)cm_alloc_zeroed(1u,
        sizeof(CmMirPublicationImpl));
    implementation->context = context;
    implementation->admission = admission;
    implementation->hir = hir;
    implementation->crate_id = crate_id;
    implementation->context_body_count = context->bodies.len;
    implementation->context_lifetime_id = context->lifetime_id;
    implementation->admission_capability_id =
        cm_semantic_admission_capability_id(admission);
    implementation->barrier_capability_id =
        cm_semantic_admission_barrier_capability_id(admission);
    implementation->parent_capability_id =
        cm_semantic_admission_parent_capability_id(admission);
    implementation->storage_lifetime_id = hir->storage.lifetime_id;
    implementation->semantic_generation = hir->semantic_generation;
    implementation->rewind_generation = hir->rewind_generation;
    implementation->pointer_bits = context->pointer_bits;
    cm_vec_init(&implementation->entries,
        sizeof(CmMirPublicationEntry));
    publication->implementation = implementation;
    return CM_MIR_OK;
}

CmMirStatus cm_mir_publication_begin_regions(CmMirPublication *publication,
    CmMirContext *context, const CmSemanticAdmission *admission)
{
    const CmSemanticResults *results;

    results = cm_semantic_admission_results(admission);
    if (cm_semantic_results_seal_kind(results)
            != CM_SEMANTIC_RESULTS_SEAL_INSTANCE_CLOSURE
        || cm_semantic_admission_barrier_capability_id(admission)
            == UINT64_C(0)
        || cm_semantic_admission_parent_capability_id(admission)
            == UINT64_C(0)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    return cm_mir_publication_begin(publication, context, admission);
}

static CmMirStatus cm_mir_publication_find_key(
    const CmMirPublication *publication, const CmMirInstance *key,
    CmMirBodyId *out_id)
{
    const CmMirPublicationImpl *implementation;
    CmMirStatus status;
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    implementation = publication == NULL ? NULL
        : (const CmMirPublicationImpl *)publication->implementation;
    if (!cm_mir_publication_current(implementation) || out_id == NULL
        || !cm_mir_instance_valid(key)
        || cm_mir_instance_is_empty(key)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    status = cm_mir_instance_is_canonical(key)
        ? cm_mir_find_canonical(implementation->context, key, out_id)
        : cm_mir_find_instance(implementation->context, key->definition,
            key->substitutions, key->substitution_count, out_id);
    if (status == CM_MIR_OK) return status;
    for (index = 0u; index < implementation->entries.len; ++index) {
        const CmMirPublicationEntry *entry;

        entry = (const CmMirPublicationEntry *)cm_vec_at_const(
            &implementation->entries, index);
        if (entry == NULL) return CM_MIR_INVARIANT_VIOLATION;
        if (cm_mir_instance_equal(&entry->instance, key)) {
            *out_id = (CmMirBodyId)(implementation->context_body_count
                + index + 1u);
            return CM_MIR_OK;
        }
    }
    return CM_MIR_INVALID_ID;
}

CmMirStatus cm_mir_publication_find_instance(
    const CmMirPublication *publication, CmHirDefId definition,
    const CmHirTypeId *substitutions, uint32_t substitution_count,
    CmMirBodyId *out_id)
{
    CmMirInstance key;

    memset(&key, 0, sizeof(key));
    key.definition = definition;
    key.body_definition = definition;
    key.substitutions = (CmHirTypeId *)substitutions;
    key.substitution_count = substitution_count;
    return cm_mir_publication_find_key(publication, &key, out_id);
}

CmMirStatus cm_mir_publication_find_canonical(
    const CmMirPublication *publication, const CmMirInstance *instance,
    CmMirBodyId *out_id)
{
    if (!cm_mir_instance_is_canonical(instance)) {
        if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_publication_find_key(publication, instance, out_id);
}

static CmMirStatus cm_mir_publication_reserve_key(
    CmMirPublication *publication, const CmMirInstance *instance,
    CmHirBodyId source_body, CmMirBodyId *out_id)
{
    CmMirPublicationImpl *implementation;
    CmMirPublicationEntry entry;
    CmMirBody query_body;
    CmMirSemanticInstanceQuery query;
    CmSemanticBodyView semantic_body;
    const CmSemanticResults *semantic_results;
    const CmHirBody *hir_body;
    CmMirBodyId existing;
    size_t future_count;
    int admitted;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    implementation = publication == NULL ? NULL
        : (CmMirPublicationImpl *)publication->implementation;
    if (!cm_mir_publication_current(implementation) || out_id == NULL
        || !cm_mir_instance_valid(instance)
        || cm_mir_instance_is_empty(instance)
        || source_body == CM_HIR_BODY_NONE
        || (cm_mir_instance_is_canonical(instance)
            && instance->body != source_body)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (cm_mir_publication_find_key(publication, instance,
            &existing) == CM_MIR_OK) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    future_count = implementation->context_body_count
        + implementation->entries.len;
    if (future_count >= (size_t)UINT32_MAX) return CM_MIR_ID_EXHAUSTED;
    hir_body = cm_hir_get_body(implementation->hir, source_body);
    semantic_results = cm_semantic_admission_results(
        implementation->admission);
    memset(&query, 0, sizeof(query));
    memset(&semantic_body, 0, sizeof(semantic_body));
    admitted = 0;
    if (hir_body == NULL || hir_body->state != CM_HIR_BODY_TYPED
        || !cm_hir_def_id_equal(hir_body->owner,
            instance->body_definition)
        || instance->definition.crate_id != implementation->crate_id
        || semantic_results == NULL
        || (cm_mir_instance_is_canonical(instance)
            && !cm_mir_canonical_materialization_valid(
                implementation->hir, instance))) {
        return CM_MIR_INVALID_ADMISSION;
    }
    if (cm_mir_instance_is_canonical(instance)) {
        admitted = cm_semantic_results_canonical_instance_body(
            semantic_results, implementation->admission,
            instance->definition, instance->body_definition,
            instance->body,
            instance->identity_bytes, instance->identity_size,
            &semantic_body) == CM_SEMANTIC_RESULTS_OK;
    } else {
        memset(&query_body, 0, sizeof(query_body));
        query_body.instance = *instance;
        admitted = cm_mir_semantic_instance_query_init(&query,
                implementation->hir, &query_body)
            && cm_semantic_results_instance_body(semantic_results,
                implementation->admission, &query.spec, &semantic_body)
                    == CM_SEMANTIC_RESULTS_OK;
    }
    cm_mir_semantic_instance_query_destroy(&query);
    if (!admitted || semantic_body.body != source_body
        || !cm_hir_def_id_equal(semantic_body.owner,
            instance->body_definition)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    cm_vec_reserve(&implementation->entries,
        implementation->entries.len + 1u);
    memset(&entry, 0, sizeof(entry));
    if (!cm_mir_instance_standalone_clone(&entry.instance, instance)) {
        return CM_MIR_ID_EXHAUSTED;
    }
    entry.source_body = source_body;
    (void)cm_vec_push(&implementation->entries, &entry);
    *out_id = (CmMirBodyId)(future_count + 1u);
    return CM_MIR_OK;
}

CmMirStatus cm_mir_publication_reserve(CmMirPublication *publication,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirBodyId source_body,
    CmMirBodyId *out_id)
{
    CmMirInstance key;

    memset(&key, 0, sizeof(key));
    key.definition = definition;
    key.body_definition = definition;
    key.substitutions = (CmHirTypeId *)substitutions;
    key.substitution_count = substitution_count;
    return cm_mir_publication_reserve_key(publication, &key, source_body,
        out_id);
}

CmMirStatus cm_mir_publication_reserve_canonical(
    CmMirPublication *publication, const CmMirInstance *instance,
    CmHirBodyId source_body, CmMirBodyId *out_id)
{
    if (!cm_mir_instance_is_canonical(instance)) {
        if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_publication_reserve_key(publication, instance,
        source_body, out_id);
}

CmMirStatus cm_mir_publication_get_instance(
    const CmMirPublication *publication, CmMirBodyId id,
    CmMirInstance *out_instance, CmHirBodyId *out_source_body)
{
    const CmMirPublicationImpl *implementation;
    const CmMirPublicationEntry *entry;
    const CmMirBody *body;

    if (out_instance != NULL) memset(out_instance, 0, sizeof(*out_instance));
    if (out_source_body != NULL) *out_source_body = CM_HIR_BODY_NONE;
    implementation = publication == NULL ? NULL
        : (const CmMirPublicationImpl *)publication->implementation;
    if (!cm_mir_publication_current(implementation)
        || out_instance == NULL || out_source_body == NULL
        || id == CM_MIR_BODY_NONE) return CM_MIR_INVALID_ARGUMENT;
    if ((size_t)id <= implementation->context_body_count) {
        body = cm_mir_get_body(implementation->context, id);
        if (body == NULL || cm_mir_instance_is_empty(&body->instance)) {
            return CM_MIR_INVALID_ID;
        }
        *out_instance = body->instance;
        *out_source_body = body->source_body;
        return CM_MIR_OK;
    }
    entry = cm_mir_publication_entry(implementation, id);
    if (entry == NULL) return CM_MIR_INVALID_ID;
    *out_instance = entry->instance;
    *out_source_body = entry->source_body;
    return CM_MIR_OK;
}

const CmMirBody *cm_mir_publication_get_body(
    const CmMirPublication *publication, CmMirBodyId id)
{
    const CmMirPublicationImpl *implementation;

    implementation = publication == NULL ? NULL
        : (const CmMirPublicationImpl *)publication->implementation;
    return cm_mir_publication_current(implementation)
        ? cm_mir_resolve_body(implementation->context, implementation, id)
        : NULL;
}

CmMirStatus cm_mir_publication_define(CmMirPublication *publication,
    CmMirBodyId id, const CmMirBody *body)
{
    CmMirPublicationImpl *implementation;
    CmMirPublicationEntry *entry;
    CmMirBody copy;
    CmMirStatus status;

    implementation = publication == NULL ? NULL
        : (CmMirPublicationImpl *)publication->implementation;
    if (!cm_mir_publication_current(implementation) || body == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    entry = cm_mir_publication_entry_mut(implementation, id);
    if (entry == NULL) return CM_MIR_INVALID_ID;
    if (entry->defined || body->owned_storage != NULL
        || !cm_mir_instance_equal(&entry->instance, &body->instance)
        || body->source_body != entry->source_body
        || !cm_hir_def_id_equal(body->owner,
            entry->instance.body_definition)
        || body->semantic_evidence
            != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    status = cm_mir_copy_body(body, &copy);
    if (status != CM_MIR_OK) return status;
    entry->body = copy;
    entry->defined = 1;
    return CM_MIR_OK;
}

CmMirStatus cm_mir_publication_validate(
    const CmMirPublication *publication)
{
    const CmMirPublicationImpl *implementation;
    const CmSemanticResults *results;
    size_t index;

    implementation = publication == NULL ? NULL
        : (const CmMirPublicationImpl *)publication->implementation;
    if (!cm_mir_publication_current(implementation)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    if (implementation->entries.len == 0u) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    results = cm_semantic_admission_results(implementation->admission);
    if (results == NULL) return CM_MIR_INVALID_ADMISSION;
    for (index = 0u; index < implementation->entries.len; ++index) {
        const CmMirPublicationEntry *entry;

        entry = (const CmMirPublicationEntry *)cm_vec_at_const(
            &implementation->entries, index);
        if (entry == NULL || !entry->defined
            || !cm_mir_exact_body_shape_valid_with_publication(
                implementation->context, implementation,
                implementation->hir, &entry->body, 1,
                implementation->admission, results)) {
            return CM_MIR_INVARIANT_VIOLATION;
        }
    }
    return CM_MIR_OK;
}

CmMirStatus cm_mir_publication_commit(CmMirPublication *publication)
{
    CmMirPublicationImpl *implementation;
    CmMirStatus status;
    size_t index;

    implementation = publication == NULL ? NULL
        : (CmMirPublicationImpl *)publication->implementation;
    status = cm_mir_publication_validate(publication);
    if (status != CM_MIR_OK) return status;
    cm_vec_reserve(&implementation->context->bodies,
        implementation->context_body_count + implementation->entries.len);
    for (index = 0u; index < implementation->entries.len; ++index) {
        CmMirPublicationEntry *entry;

        entry = (CmMirPublicationEntry *)cm_vec_at(
            &implementation->entries, index);
        (void)cm_vec_push(&implementation->context->bodies, &entry->body);
        memset(&entry->body, 0, sizeof(entry->body));
        entry->defined = 0;
    }
    if (implementation->context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_context_latch_admission(implementation->context,
            implementation->admission, implementation->hir,
            implementation->crate_id);
    }
    cm_mir_publication_destroy(publication);
    return CM_MIR_OK;
}

CmMirStatus cm_mir_context_set_pointer_bits(CmMirContext *context,
    unsigned int pointer_bits)
{
    if (!cm_mir_context_valid(context)
        || !cm_mir_pointer_bits_valid(pointer_bits)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (context->bodies.len != 0u
        || (context->pointer_bits != 0u
            && context->pointer_bits != pointer_bits)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    context->pointer_bits = pointer_bits;
    return CM_MIR_OK;
}

unsigned int cm_mir_context_pointer_bits(const CmMirContext *context)
{
    return cm_mir_context_valid(context) ? context->pointer_bits : 0u;
}

CmMirStatus cm_mir_add_body(CmMirContext *context, const CmMirBody *body,
    CmMirBodyId *out_id)
{
    CmMirBody copy;
    CmMirStatus status;
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || body == NULL || out_id == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (!cm_mir_legacy_body_shape_valid(body)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    if (context->bodies.len >= (size_t)UINT32_MAX) {
        return CM_MIR_ID_EXHAUSTED;
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *old_body;

        old_body = (const CmMirBody *)cm_vec_at_const(&context->bodies,
            index);
        if (old_body == NULL || cm_hir_def_id_equal(old_body->owner,
                body->owner)
            || old_body->source_body == body->source_body) {
            return CM_MIR_INVARIANT_VIOLATION;
        }
    }
    cm_vec_reserve(&context->bodies, context->bodies.len + 1u);
    status = cm_mir_copy_body(body, &copy);
    if (status != CM_MIR_OK) return status;
    (void)cm_vec_push(&context->bodies, &copy);
    *out_id = (CmMirBodyId)context->bodies.len;
    return CM_MIR_OK;
}

static CmMirStatus cm_mir_add_exact_copy(CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, CmMirBodyId *out_id)
{
    CmMirBody copy;
    CmMirStatus status;
    size_t index;

    if (context->bodies.len >= (size_t)UINT32_MAX) {
        return CM_MIR_ID_EXHAUSTED;
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *old_body;

        old_body = (const CmMirBody *)cm_vec_at_const(&context->bodies,
            index);
        if (old_body == NULL
            || (cm_mir_instance_is_empty(&old_body->instance)
                && (cm_hir_def_id_equal(old_body->owner, body->owner)
                    || old_body->source_body == body->source_body))
            || (!cm_mir_instance_is_empty(&old_body->instance)
                && cm_mir_instance_equal(&old_body->instance,
                    &body->instance))) {
            return CM_MIR_INVARIANT_VIOLATION;
        }
    }
    cm_vec_reserve(&context->bodies, context->bodies.len + 1u);
    status = cm_mir_copy_body(body, &copy);
    if (status != CM_MIR_OK) return status;
    (void)cm_vec_push(&context->bodies, &copy);
    if (context->hir_owner == NULL) context->hir_owner = hir;
    *out_id = (CmMirBodyId)context->bodies.len;
    return CM_MIR_OK;
}

CmMirStatus cm_mir_add_monomorphized_body(CmMirContext *context,
    const CmHirContext *hir, const CmMirBody *body, CmMirBodyId *out_id)
{
    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || hir == NULL || body == NULL
        || body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_NONE
        || out_id == NULL || (context->hir_owner != NULL
            && context->hir_owner != hir)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (!cm_mir_exact_body_shape_valid(context, hir, body, 0,
            NULL, NULL)) {
        return CM_MIR_INVARIANT_VIOLATION;
    }
    return cm_mir_add_exact_copy(context, hir, body, out_id);
}

CmMirStatus cm_mir_add_admitted_monomorphized_body(CmMirContext *context,
    const CmSemanticAdmission *admission, const CmMirBody *body,
    CmMirBodyId *out_id)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    const CmHirBody *source_body;
    CmMirSemanticInstanceQuery query;
    CmSemanticBodyView semantic_body;
    CmSemanticBodyView canonical_body;
    CmHirCrateId crate_id;
    CmMirStatus status;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (context == NULL || body == NULL || out_id == NULL) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    if (!cm_mir_admission_identity(admission, &hir, &crate_id)
        || !cm_mir_context_accepts_admission(context, admission, hir,
            crate_id)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    memset(&query, 0, sizeof(query));
    memset(&canonical_body, 0, sizeof(canonical_body));
    memset(&semantic_body, 0, sizeof(semantic_body));
    source_body = cm_hir_get_body(hir, body->source_body);
    semantic_results = cm_semantic_admission_results(admission);
    if (source_body == NULL || source_body->owner.crate_id != crate_id
        || body->owner.crate_id != crate_id
        || body->instance.definition.crate_id != crate_id
        || body->instance.body_definition.crate_id != crate_id
        || semantic_results == NULL
        || (body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_BODY
            && body->semantic_evidence
                != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE)
        || (cm_mir_instance_is_canonical(&body->instance)
            && body->semantic_evidence
                != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE)
        || (body->semantic_evidence
                == CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE
            && cm_mir_instance_is_canonical(&body->instance)
            && (!cm_mir_canonical_materialization_valid(hir,
                    &body->instance)
                || cm_semantic_results_canonical_instance_body(
                    semantic_results, admission,
                    body->instance.definition,
                    body->instance.body_definition, body->instance.body,
                    body->instance.identity_bytes,
                    body->instance.identity_size, &canonical_body)
                        != CM_SEMANTIC_RESULTS_OK
                || canonical_body.body != body->source_body
                || !cm_hir_def_id_equal(canonical_body.owner,
                    body->instance.body_definition)))
        || (body->semantic_evidence == CM_MIR_SEMANTIC_EVIDENCE_BODY
            ? cm_semantic_results_body(semantic_results, admission,
                body->source_body, &semantic_body)
            : cm_mir_instance_is_canonical(&body->instance)
                ? cm_semantic_results_canonical_instance_body(
                    semantic_results, admission, body->instance.definition,
                    body->instance.body_definition, body->instance.body,
                    body->instance.identity_bytes,
                    body->instance.identity_size, &semantic_body)
                : cm_mir_semantic_instance_query_init(&query, hir, body)
                    ? cm_semantic_results_instance_body(semantic_results,
                        admission, &query.spec, &semantic_body)
                    : CM_SEMANTIC_RESULTS_INVALID_ARGUMENT)
            != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(semantic_body.owner,
            body->instance.body_definition)) {
        cm_mir_semantic_instance_query_destroy(&query);
        return CM_MIR_INVALID_ADMISSION;
    }
    if (!cm_mir_exact_body_shape_valid(context, hir, body, 0,
            admission, semantic_results)) {
        cm_mir_semantic_instance_query_destroy(&query);
        return CM_MIR_INVARIANT_VIOLATION;
    }
    cm_mir_semantic_instance_query_destroy(&query);
    status = cm_mir_add_exact_copy(context, hir, body, out_id);
    if (status == CM_MIR_OK
        && context->admitted_crate == CM_HIR_CRATE_NONE) {
        cm_mir_context_latch_admission(context, admission, hir, crate_id);
    }
    return status;
}

CmMirStatus cm_mir_validate_monomorphized_body(
    const CmMirContext *context, const CmHirContext *hir, CmMirBodyId id)
{
    const CmMirBody *body;

    if (!cm_mir_context_valid(context) || hir == NULL
        || context->hir_owner != hir) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    body = cm_mir_get_body(context, id);
    if (body == NULL) return CM_MIR_INVALID_ID;
    return cm_mir_exact_body_shape_valid(context, hir, body, 1,
        NULL, NULL)
        ? CM_MIR_OK : CM_MIR_INVARIANT_VIOLATION;
}

CmMirStatus cm_mir_validate_admitted_monomorphized_body(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    const CmMirBody *body;
    CmSemanticFunctionSignatureView signature;
    CmHirCrateId crate_id;

    if (context == NULL) return CM_MIR_INVALID_ARGUMENT;
    if (!cm_mir_admission_identity(admission, &hir, &crate_id)
        || context->admitted_crate == CM_HIR_CRATE_NONE
        || !cm_mir_context_accepts_admission(context, admission, hir,
            crate_id)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    semantic_results = cm_semantic_admission_results(admission);
    body = cm_mir_get_body(context, id);
    if (semantic_results == NULL || body == NULL
        || (body->semantic_evidence != CM_MIR_SEMANTIC_EVIDENCE_BODY
            && body->semantic_evidence
                != CM_MIR_SEMANTIC_EVIDENCE_EXACT_INSTANCE)
        || cm_mir_semantic_signature_query(semantic_results, admission,
            body, NULL, &signature)
                != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(signature.definition,
            body->instance.body_definition)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    if (!cm_mir_exact_body_shape_valid(context, hir, body, 1,
            admission, semantic_results)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    return CM_MIR_OK;
}

CmMirStatus cm_mir_admitted_signature(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id, CmSemanticFunctionSignatureView *out_view)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    const CmMirBody *body;
    CmHirCrateId crate_id;
    CmSemanticResultsStatus results_status;

    if (out_view == NULL) return CM_MIR_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    out_view->definition = cm_hir_def_id_none();
    if (context == NULL) return CM_MIR_INVALID_ARGUMENT;
    if (!cm_mir_admission_identity(admission, &hir, &crate_id)
        || context->admitted_crate == CM_HIR_CRATE_NONE
        || !cm_mir_context_accepts_admission(context, admission, hir,
            crate_id)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    semantic_results = cm_semantic_admission_results(admission);
    body = cm_mir_get_body(context, id);
    if (semantic_results == NULL) return CM_MIR_INVALID_ADMISSION;
    if (body == NULL) return CM_MIR_INVALID_ID;
    results_status = cm_mir_semantic_signature_query(semantic_results,
        admission, body, NULL, out_view);
    if (results_status != CM_SEMANTIC_RESULTS_OK
        || !cm_hir_def_id_equal(out_view->definition,
            body->instance.body_definition)
        || out_view->body != body->source_body) {
        memset(out_view, 0, sizeof(*out_view));
        out_view->definition = cm_hir_def_id_none();
        return CM_MIR_INVALID_ADMISSION;
    }
    return CM_MIR_OK;
}

CmMirStatus cm_mir_admitted_signature_parameter(
    const CmMirContext *context, const CmSemanticAdmission *admission,
    CmMirBodyId id, uint32_t parameter, CmSemanticTypeView *out_view)
{
    const CmHirContext *hir;
    const CmSemanticResults *semantic_results;
    const CmMirBody *body;
    CmSemanticFunctionSignatureView signature;
    CmHirCrateId crate_id;
    CmSemanticResultsStatus results_status;

    if (out_view == NULL) return CM_MIR_INVALID_ARGUMENT;
    memset(out_view, 0, sizeof(*out_view));
    if (context == NULL) return CM_MIR_INVALID_ARGUMENT;
    if (!cm_mir_admission_identity(admission, &hir, &crate_id)
        || context->admitted_crate == CM_HIR_CRATE_NONE
        || !cm_mir_context_accepts_admission(context, admission, hir,
            crate_id)) {
        return CM_MIR_INVALID_ADMISSION;
    }
    semantic_results = cm_semantic_admission_results(admission);
    body = cm_mir_get_body(context, id);
    if (semantic_results == NULL) return CM_MIR_INVALID_ADMISSION;
    if (body == NULL) return CM_MIR_INVALID_ID;
    memset(&signature, 0, sizeof(signature));
    results_status = cm_mir_semantic_signature_query(semantic_results,
        admission, body, NULL, &signature);
    if (results_status == CM_SEMANTIC_RESULTS_OK
        && cm_hir_def_id_equal(signature.definition,
            body->instance.body_definition)
        && signature.body == body->source_body
        && parameter < signature.parameter_count) {
        results_status = cm_mir_semantic_signature_parameter_query(
            semantic_results, admission, body, NULL, parameter,
            out_view);
    } else {
        results_status = CM_SEMANTIC_RESULTS_NOT_FOUND;
    }
    if (results_status != CM_SEMANTIC_RESULTS_OK) {
        memset(out_view, 0, sizeof(*out_view));
        return CM_MIR_INVALID_ADMISSION;
    }
    return CM_MIR_OK;
}

static CmMirStatus cm_mir_find_key(const CmMirContext *context,
    const CmMirInstance *instance, CmMirBodyId *out_id)
{
    size_t index;

    if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
    if (!cm_mir_context_valid(context) || out_id == NULL
        || !cm_mir_instance_valid(instance)
        || cm_mir_instance_is_empty(instance)) {
        return CM_MIR_INVALID_ARGUMENT;
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmMirBody *body;

        body = (const CmMirBody *)cm_vec_at_const(&context->bodies, index);
        if (body == NULL) return CM_MIR_INVARIANT_VIOLATION;
        if (cm_mir_instance_equal(&body->instance, instance)) {
            *out_id = (CmMirBodyId)(index + 1u);
            return CM_MIR_OK;
        }
    }
    return CM_MIR_INVALID_ID;
}

CmMirStatus cm_mir_find_instance(const CmMirContext *context,
    CmHirDefId definition, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmMirBodyId *out_id)
{
    CmMirInstance key;

    memset(&key, 0, sizeof(key));
    key.definition = definition;
    key.body_definition = definition;
    key.substitutions = (CmHirTypeId *)substitutions;
    key.substitution_count = substitution_count;
    return cm_mir_find_key(context, &key, out_id);
}

CmMirStatus cm_mir_find_canonical(const CmMirContext *context,
    const CmMirInstance *instance, CmMirBodyId *out_id)
{
    if (!cm_mir_instance_is_canonical(instance)) {
        if (out_id != NULL) *out_id = CM_MIR_BODY_NONE;
        return CM_MIR_INVALID_ARGUMENT;
    }
    return cm_mir_find_key(context, instance, out_id);
}

const CmMirBody *cm_mir_get_body(const CmMirContext *context,
    CmMirBodyId id)
{
    if (!cm_mir_context_valid(context) || id == CM_MIR_BODY_NONE
        || (size_t)id > context->bodies.len) {
        return NULL;
    }
    return (const CmMirBody *)cm_vec_at_const(&context->bodies,
        (size_t)id - 1u);
}

size_t cm_mir_body_count(const CmMirContext *context)
{
    return cm_mir_context_valid(context) ? context->bodies.len : 0u;
}

const char *cm_mir_status_name(CmMirStatus status)
{
    switch (status) {
    case CM_MIR_OK: return "ok";
    case CM_MIR_INVALID_ARGUMENT: return "invalid argument";
    case CM_MIR_INVALID_ID: return "invalid id";
    case CM_MIR_ID_EXHAUSTED: return "id exhausted";
    case CM_MIR_INVARIANT_VIOLATION: return "invariant violation";
    case CM_MIR_INVALID_ADMISSION: return "invalid admission";
    }
    return "unknown MIR status";
}
