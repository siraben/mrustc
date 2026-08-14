#include "semantic_body_internal.h"

#include "cm/hir/body.h"

#include "cm/alloc.h"

#include <string.h>

#define CM_SEMANTIC_BODY_TYPE_DEPTH ((size_t)128u)
#define CM_SEMANTIC_BODY_NORMALIZE_NODES ((size_t)4096u)
#define CM_SEMANTIC_BODY_NORMALIZE_PROJECTIONS ((size_t)256u)

typedef enum CmSemanticTypeScan {
    CM_SEMANTIC_TYPE_OK = 0,
    CM_SEMANTIC_TYPE_PROJECTION,
    CM_SEMANTIC_TYPE_INFERENCE,
    CM_SEMANTIC_TYPE_UNSUPPORTED,
    CM_SEMANTIC_TYPE_OVERFLOW,
    CM_SEMANTIC_TYPE_INVALID
} CmSemanticTypeScan;

typedef struct CmSemanticCanonicalReader {
    const CmHirContext *hir;
    CmTypeckContext *typeck;
    const unsigned char *bytes;
    size_t size;
    size_t position;
    CmSpan span;
} CmSemanticCanonicalReader;

static CmSemanticBodyResult cm_semantic_body_result(
    CmSemanticBodyStatus status, CmHirBodyId body)
{
    CmSemanticBodyResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.body = body;
    result.expression = CM_HIR_EXPR_NONE;
    result.callee = cm_hir_def_id_none();
    result.predicate_index = CM_SEMANTIC_BODY_PREDICATE_NONE;
    result.solver_kind = CM_TRAIT_SOLVER_INVALID;
    result.typeck_status = CM_TYPECK_OK;
    return result;
}

static const CmHirItem *cm_semantic_body_item(const CmHirContext *hir,
    CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL
            && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static CmSemanticTypeScan cm_semantic_scan_type(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth);

static CmSemanticTypeScan cm_semantic_scan_merge(CmSemanticTypeScan left,
    CmSemanticTypeScan right)
{
    return right > left ? right : left;
}

static CmSemanticTypeScan cm_semantic_scan_region(
    const CmHirRegion *region)
{
    if (region == NULL) return CM_SEMANTIC_TYPE_INVALID;
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_REGION_INFER:
        return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_REGION_EARLY_BOUND:
    case CM_HIR_REGION_LATE_BOUND:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    case CM_HIR_REGION_ERROR:
        return CM_SEMANTIC_TYPE_INVALID;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticTypeScan cm_semantic_scan_const(const CmHirContext *hir,
    const CmHirConstArg *constant, size_t depth)
{
    CmSemanticTypeScan result;

    if (constant == NULL) return CM_SEMANTIC_TYPE_INVALID;
    result = cm_semantic_scan_type(hir, constant->type, depth + 1u);
    if (result != CM_SEMANTIC_TYPE_OK) return result;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE: return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_CONST_INFER: return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_CONST_PARAMETER:
    case CM_HIR_CONST_UNEVALUATED:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    case CM_HIR_CONST_ERROR: return CM_SEMANTIC_TYPE_INVALID;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticTypeScan cm_semantic_scan_named(const CmHirContext *hir,
    const CmHirNamedType *named, size_t depth)
{
    CmSemanticTypeScan result;
    uint32_t index;

    if (named == NULL || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_SEMANTIC_TYPE_INVALID;
    }
    result = CM_SEMANTIC_TYPE_OK;
    for (index = 0u; index < named->argument_count; ++index) {
        CmSemanticTypeScan child;

        if (named->arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            child = cm_semantic_scan_type(hir,
                named->arguments[index].data.type, depth + 1u);
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_LIFETIME) {
            child = cm_semantic_scan_region(
                &named->arguments[index].data.lifetime);
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_CONST) {
            child = cm_semantic_scan_const(hir,
                &named->arguments[index].data.constant, depth + 1u);
        } else {
            child = CM_SEMANTIC_TYPE_INVALID;
        }
        result = cm_semantic_scan_merge(result, child);
    }
    return result;
}

static CmSemanticTypeScan cm_semantic_scan_type(const CmHirContext *hir,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;
    CmSemanticTypeScan result;
    uint32_t index;

    if (depth >= CM_SEMANTIC_BODY_TYPE_DEPTH) {
        return CM_SEMANTIC_TYPE_OVERFLOW;
    }
    type = cm_hir_get_type(hir, type_id);
    if (type == NULL) return CM_SEMANTIC_TYPE_INVALID;
    switch (type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_TYPE_INFER_KIND:
        return CM_SEMANTIC_TYPE_INFERENCE;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_semantic_scan_merge(CM_SEMANTIC_TYPE_PROJECTION,
            cm_semantic_scan_merge(
                cm_semantic_scan_type(hir,
                    type->data.projection_type.self_type, depth + 1u),
                cm_semantic_scan_merge(
                    cm_semantic_scan_named(hir,
                        &type->data.projection_type.trait_type, depth + 1u),
                    cm_semantic_scan_named(hir,
                        &type->data.projection_type.associated_type,
                        depth + 1u))));
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_semantic_scan_merge(
            cm_semantic_scan_region(&type->data.reference_type.region),
            cm_semantic_scan_type(hir, type->data.reference_type.pointee,
                depth + 1u));
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_semantic_scan_type(hir,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if ((type->data.tuple_type.element_count == 0u)
                != (type->data.tuple_type.elements == NULL)) {
            return CM_SEMANTIC_TYPE_INVALID;
        }
        result = CM_SEMANTIC_TYPE_OK;
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            result = cm_semantic_scan_merge(result,
                cm_semantic_scan_type(hir,
                    type->data.tuple_type.elements[index], depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_semantic_scan_merge(
            cm_semantic_scan_type(hir, type->data.array_type.element,
                depth + 1u),
            cm_semantic_scan_const(hir, &type->data.array_type.length,
                depth + 1u));
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_semantic_scan_type(hir, type->data.slice_type.element,
            depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if ((type->data.fn_pointer_type.parameter_count == 0u)
                != (type->data.fn_pointer_type.parameters == NULL)) {
            return CM_SEMANTIC_TYPE_INVALID;
        }
        result = cm_semantic_scan_type(hir,
            type->data.fn_pointer_type.return_type, depth + 1u);
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            result = cm_semantic_scan_merge(result,
                cm_semantic_scan_type(hir,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u));
        }
        return result;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_semantic_scan_named(hir, &type->data.named_type,
            depth + 1u);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_hir_get_generic_param(hir,
                type->data.parameter_type.parameter) == NULL
            ? CM_SEMANTIC_TYPE_INVALID : CM_SEMANTIC_TYPE_OK;
    /* Authentication and substitution of Self require the private body
     * instantiation and are therefore checked by instantiate_hir_type. */
    case CM_HIR_TYPE_SELF_KIND:
        return CM_SEMANTIC_TYPE_OK;
    case CM_HIR_TYPE_ERROR_KIND:
        return CM_SEMANTIC_TYPE_INVALID;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return CM_SEMANTIC_TYPE_UNSUPPORTED;
    }
    return CM_SEMANTIC_TYPE_INVALID;
}

static CmSemanticBodyStatus cm_semantic_scan_status(CmSemanticTypeScan scan)
{
    switch (scan) {
    case CM_SEMANTIC_TYPE_OK: return CM_SEMANTIC_BODY_OK;
    case CM_SEMANTIC_TYPE_PROJECTION:
        return CM_SEMANTIC_BODY_PENDING_PROJECTION;
    case CM_SEMANTIC_TYPE_INFERENCE:
        return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_SEMANTIC_TYPE_UNSUPPORTED:
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_SEMANTIC_TYPE_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_SEMANTIC_TYPE_INVALID: return CM_SEMANTIC_BODY_INVALID;
    }
    return CM_SEMANTIC_BODY_INVALID;
}

static CmSemanticBodyStatus cm_semantic_typeck_status(
    CmTypeckStatus status)
{
    switch (status) {
    case CM_TYPECK_OK: return CM_SEMANTIC_BODY_OK;
    case CM_TYPECK_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_TYPECK_UNRESOLVED: return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_TYPECK_UNSUPPORTED_HIR_TYPE:
    case CM_TYPECK_UNSUPPORTED_CONSTANT:
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_TYPECK_INVALID_ARGUMENT:
    case CM_TYPECK_INVALID_ID:
        return CM_SEMANTIC_BODY_INVALID;
    case CM_TYPECK_INVALID_SNAPSHOT:
    case CM_TYPECK_KIND_CONFLICT:
    case CM_TYPECK_TYPE_MISMATCH:
    case CM_TYPECK_OCCURS_CHECK:
    case CM_TYPECK_HIR_FAILURE:
        return CM_SEMANTIC_BODY_TYPECK_FAILURE;
    }
    return CM_SEMANTIC_BODY_TYPECK_FAILURE;
}

static CmSemanticBodyStatus cm_semantic_solver_status(
    CmTraitSolverResultKind kind)
{
    switch (kind) {
    case CM_TRAIT_SOLVER_PROVEN: return CM_SEMANTIC_BODY_OK;
    case CM_TRAIT_SOLVER_NEGATIVE: return CM_SEMANTIC_BODY_NEGATIVE;
    case CM_TRAIT_SOLVER_NO_SOLUTION: return CM_SEMANTIC_BODY_NO_SOLUTION;
    case CM_TRAIT_SOLVER_AMBIGUOUS: return CM_SEMANTIC_BODY_AMBIGUOUS;
    case CM_TRAIT_SOLVER_DEFERRED_INFERENCE:
        return CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
    case CM_TRAIT_SOLVER_DEFERRED_METADATA:
        return CM_SEMANTIC_BODY_DEFERRED_METADATA;
    case CM_TRAIT_SOLVER_UNSUPPORTED: return CM_SEMANTIC_BODY_UNSUPPORTED;
    case CM_TRAIT_SOLVER_OVERFLOW: return CM_SEMANTIC_BODY_OVERFLOW;
    case CM_TRAIT_SOLVER_INVALID: return CM_SEMANTIC_BODY_INVALID;
    case CM_TRAIT_SOLVER_TYPECK_FAILURE:
        return CM_SEMANTIC_BODY_TYPECK_FAILURE;
    }
    return CM_SEMANTIC_BODY_INVALID;
}

static int cm_semantic_type_only_owner(const CmHirContext *hir,
    const CmHirItem *item, uint32_t count)
{
    uint32_t index;

    if (item == NULL || item->generic_parameter_count != count
        || (count == 0u) != (item->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE)) return 0;
    if (count == 0u) return item->generic_parameter_start
        == CM_HIR_GENERIC_PARAM_NONE;
    if (count - 1u > UINT32_MAX - item->generic_parameter_start) return 0;
    for (index = 0u; index < count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(hir,
            item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                item->definition)) return 0;
    }
    return 1;
}

static void cm_semantic_body_callable_facts_clear(
    CmSemanticCheckedCallableFacts *facts);

static CmSemanticBodyResult cm_semantic_body_fail_snapshot_impl(
    CmSemanticBodyResult result, CmTypeckContext *typeck,
    CmTypeckSnapshot *snapshot, CmHirExprId *call_expressions,
    CmTypeckGenericArg *owner_arguments,
    CmTypeckGenericArg *enclosing_arguments,
    CmTypeckGenericArg *implemented_arguments,
    CmTypeckGenericArg *callee_arguments,
    CmTypeckTypeId *expression_terms, CmVec *deferred_equalities,
    CmSemanticCheckedBodyFacts *facts,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    CmTypeckStatus rollback;
    size_t call_index;

    rollback = cm_typeck_rollback(typeck, snapshot);
    if (rollback != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = rollback;
    }
    cm_free(call_expressions);
    cm_free(owner_arguments);
    cm_free(enclosing_arguments);
    cm_free(implemented_arguments);
    cm_free(callee_arguments);
    cm_free(expression_terms);
    if (facts != NULL) {
        for (call_index = 0u; call_index < facts->call_count; ++call_index) {
            cm_free((void *)facts->calls[call_index].parameter_types);
        }
        for (call_index = 0u; call_index < facts->callable_count;
             ++call_index) {
            cm_semantic_body_callable_facts_clear(
                &((CmSemanticCheckedCallableFacts *)facts->callables)[
                    call_index]);
        }
        cm_free((void *)facts->calls);
        cm_free((void *)facts->callables);
        cm_free((void *)facts->signature_parameter_types);
        cm_free((void *)facts->adjustments);
        cm_free((void *)facts->primitive_binaries);
        cm_free((void *)facts->field_selections);
        memset(facts, 0, sizeof(*facts));
    }
    cm_vec_destroy(deferred_equalities);
    if (writeback != NULL && writeback->discard != NULL) {
        writeback->discard(writeback->context);
    }
    return result;
}

#define cm_semantic_body_fail_snapshot(result, typeck, snapshot, ...) \
    cm_semantic_body_fail_snapshot_impl((result), (typeck), (snapshot), \
        call_expressions, owner_arguments, enclosing_arguments, \
        implemented_arguments, callee_arguments, expression_terms, \
        &deferred_equalities, \
        &checked_facts, writeback)

static CmSemanticBodyStatus cm_semantic_body_allocate_arguments(
    uint32_t count, CmTypeckGenericArg **out_arguments)
{
    size_t bytes;

    if (out_arguments == NULL) return CM_SEMANTIC_BODY_INVALID;
    *out_arguments = NULL;
    if (count == 0u) return CM_SEMANTIC_BODY_OK;
    if (!cm_size_mul((size_t)count, sizeof(**out_arguments), &bytes)) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    *out_arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(1u, bytes);
    return CM_SEMANTIC_BODY_OK;
}

static int cm_semantic_canonical_read(CmSemanticCanonicalReader *reader,
    void *out, size_t count)
{
    if (reader == NULL || (count != 0u && out == NULL)
        || reader->position > reader->size
        || count > reader->size - reader->position) {
        return 0;
    }
    if (count != 0u) {
        memcpy(out, reader->bytes + reader->position, count);
    }
    reader->position += count;
    return 1;
}

static int cm_semantic_canonical_u8(CmSemanticCanonicalReader *reader,
    unsigned int *out)
{
    unsigned char value;

    if (out == NULL || !cm_semantic_canonical_read(reader, &value, 1u)) {
        return 0;
    }
    *out = value;
    return 1;
}

static int cm_semantic_canonical_u32(CmSemanticCanonicalReader *reader,
    uint32_t *out)
{
    unsigned char bytes[4];

    if (out == NULL
        || !cm_semantic_canonical_read(reader, bytes, sizeof(bytes))) {
        return 0;
    }
    *out = (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8u)
        | ((uint32_t)bytes[2] << 16u)
        | ((uint32_t)bytes[3] << 24u);
    return 1;
}

static int cm_semantic_canonical_u64(CmSemanticCanonicalReader *reader,
    uint64_t *out)
{
    unsigned char bytes[8];
    unsigned int index;
    uint64_t value;

    if (out == NULL
        || !cm_semantic_canonical_read(reader, bytes, sizeof(bytes))) {
        return 0;
    }
    value = UINT64_C(0);
    for (index = 0u; index < 8u; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8u);
    }
    *out = value;
    return 1;
}

static int cm_semantic_canonical_def(CmSemanticCanonicalReader *reader,
    CmHirDefId *out)
{
    return out != NULL
        && cm_semantic_canonical_u32(reader, &out->crate_id)
        && cm_semantic_canonical_u32(reader, &out->index);
}

static const CmHirGenericParam *cm_semantic_canonical_parameter(
    const CmHirContext *hir, const CmHirItem *owner, uint32_t index)
{
    const CmHirGenericParam *parameter;

    if (hir == NULL || owner == NULL
        || index >= owner->generic_parameter_count
        || (owner->generic_parameter_count != 0u
            && index > UINT32_MAX - owner->generic_parameter_start)) {
        return NULL;
    }
    parameter = cm_hir_get_generic_param(hir,
        owner->generic_parameter_start + index);
    return parameter != NULL && parameter->index == index
            && cm_hir_def_id_equal(parameter->owner, owner->definition)
        ? parameter : NULL;
}

static int cm_semantic_canonical_region(CmSemanticCanonicalReader *reader,
    CmHirRegion *out)
{
    unsigned int kind;

    if (out == NULL || !cm_semantic_canonical_u8(reader, &kind)
        || (kind != (unsigned int)CM_HIR_REGION_STATIC
            && kind != (unsigned int)CM_HIR_REGION_ERASED)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->kind = (CmHirRegionKind)kind;
    return 1;
}

static int cm_semantic_canonical_type(CmSemanticCanonicalReader *reader,
    size_t depth, CmTypeckTypeId *out_type);

static int cm_semantic_canonical_const(CmSemanticCanonicalReader *reader,
    size_t depth, CmTypeckConst *out)
{
    unsigned int kind;

    if (reader == NULL || out == NULL
        || depth >= CM_SEMANTIC_BODY_TYPE_DEPTH
        || !cm_semantic_canonical_u8(reader, &kind)
        || kind != (unsigned int)CM_HIR_CONST_VALUE) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->kind = CM_HIR_CONST_VALUE;
    return cm_semantic_canonical_type(reader, depth + 1u, &out->type)
        && cm_semantic_canonical_u64(reader, &out->data.value.low_bits)
        && cm_semantic_canonical_u64(reader, &out->data.value.high_bits);
}

static int cm_semantic_canonical_argument(
    CmSemanticCanonicalReader *reader, CmHirGenericArgKind kind,
    size_t depth, CmTypeckGenericArg *out)
{
    if (reader == NULL || out == NULL
        || depth >= CM_SEMANTIC_BODY_TYPE_DEPTH
        || (unsigned int)kind > (unsigned int)CM_HIR_GENERIC_ARG_CONST) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    switch (kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_semantic_canonical_region(reader, &out->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_semantic_canonical_type(reader, depth + 1u,
            &out->data.type);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_semantic_canonical_const(reader, depth + 1u,
            &out->data.constant);
    }
    return 0;
}

static int cm_semantic_canonical_type(CmSemanticCanonicalReader *reader,
    size_t depth, CmTypeckTypeId *out_type)
{
    const CmHirItem *adt;
    CmTypeckType type;
    CmTypeckTypeId *children;
    CmTypeckGenericArg *arguments;
    CmHirDefId definition;
    CmInternId abi;
    uint32_t count;
    uint32_t parameter_count;
    uint32_t index;
    unsigned int tag;
    unsigned int scalar;
    size_t bytes;
    int ok;

    if (reader == NULL || out_type == NULL
        || depth >= CM_SEMANTIC_BODY_TYPE_DEPTH
        || !cm_semantic_canonical_u8(reader, &tag)) {
        return 0;
    }
    memset(&type, 0, sizeof(type));
    type.span = reader->span;
    children = NULL;
    arguments = NULL;
    ok = 0;
    switch ((CmHirTypeKind)tag) {
    case CM_HIR_TYPE_NEVER_KIND:
        type.kind = CM_TYPECK_TYPE_NEVER;
        break;
    case CM_HIR_TYPE_UNIT_KIND:
        type.kind = CM_TYPECK_TYPE_UNIT;
        break;
    case CM_HIR_TYPE_BOOL_KIND:
        type.kind = CM_TYPECK_TYPE_BOOL;
        break;
    case CM_HIR_TYPE_CHAR_KIND:
        type.kind = CM_TYPECK_TYPE_CHAR;
        break;
    case CM_HIR_TYPE_STR_KIND:
        type.kind = CM_TYPECK_TYPE_STR;
        break;
    case CM_HIR_TYPE_INTEGER_KIND:
        if (!cm_semantic_canonical_u8(reader, &scalar)
            || scalar > (unsigned int)CM_HIR_INT_USIZE) goto done;
        type.kind = CM_TYPECK_TYPE_INTEGER;
        type.data.integer_type = (CmHirIntType)scalar;
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        if (!cm_semantic_canonical_u8(reader, &scalar)
            || scalar > (unsigned int)CM_HIR_FLOAT_F128) goto done;
        type.kind = CM_TYPECK_TYPE_FLOAT;
        type.data.float_type = (CmHirFloatType)scalar;
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        if (!cm_semantic_canonical_region(reader,
                &type.data.reference_type.region)
            || !cm_semantic_canonical_u8(reader, &scalar)
            || scalar > (unsigned int)CM_HIR_MUTABLE
            || !cm_semantic_canonical_type(reader, depth + 1u,
                &type.data.reference_type.pointee)) goto done;
        type.kind = CM_TYPECK_TYPE_REFERENCE;
        type.data.reference_type.mutability = (CmHirMutability)scalar;
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        if (!cm_semantic_canonical_u8(reader, &scalar)
            || scalar > (unsigned int)CM_HIR_MUTABLE
            || !cm_semantic_canonical_type(reader, depth + 1u,
                &type.data.raw_pointer_type.pointee)) goto done;
        type.kind = CM_TYPECK_TYPE_RAW_POINTER;
        type.data.raw_pointer_type.mutability = (CmHirMutability)scalar;
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        if (!cm_semantic_canonical_u32(reader, &count)
            || (count != 0u && (!cm_size_mul((size_t)count,
                    sizeof(*children), &bytes)
                || count > reader->size - reader->position))) goto done;
        children = count == 0u ? NULL
            : (CmTypeckTypeId *)cm_alloc_zeroed(1u, bytes);
        for (index = 0u; index < count; ++index) {
            if (!cm_semantic_canonical_type(reader, depth + 1u,
                    &children[index])) goto done;
        }
        type.kind = CM_TYPECK_TYPE_TUPLE;
        type.data.tuple_type.elements = children;
        type.data.tuple_type.element_count = count;
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        if (!cm_semantic_canonical_type(reader, depth + 1u,
                &type.data.array_type.element)
            || !cm_semantic_canonical_const(reader, depth + 1u,
                &type.data.array_type.length)) goto done;
        type.kind = CM_TYPECK_TYPE_ARRAY;
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        if (!cm_semantic_canonical_type(reader, depth + 1u,
                &type.data.slice_type.element)) goto done;
        type.kind = CM_TYPECK_TYPE_SLICE;
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (!cm_semantic_canonical_u32(reader, &count)
            || (count != 0u && (!cm_size_mul((size_t)count,
                    sizeof(*children), &bytes)
                || count > reader->size - reader->position))) goto done;
        parameter_count = count;
        children = parameter_count == 0u ? NULL
            : (CmTypeckTypeId *)cm_alloc_zeroed(1u, bytes);
        for (index = 0u; index < parameter_count; ++index) {
            if (!cm_semantic_canonical_type(reader, depth + 1u,
                    &children[index])) goto done;
        }
        if (!cm_semantic_canonical_type(reader, depth + 1u,
                &type.data.fn_pointer_type.return_type)
            || !cm_semantic_canonical_u32(reader, &count)
            || reader->position > reader->size
            || count > reader->size - reader->position) goto done;
        abi = cm_interner_lookup(&reader->hir->strings,
            reader->bytes + reader->position, count);
        if (abi == CM_INTERN_ID_NONE) goto done;
        reader->position += count;
        if (!cm_semantic_canonical_u8(reader, &scalar)
            || scalar > (unsigned int)CM_HIR_UNSAFE) goto done;
        type.data.fn_pointer_type.safety = (CmHirSafety)scalar;
        if (!cm_semantic_canonical_u8(reader, &scalar) || scalar > 1u) {
            goto done;
        }
        type.kind = CM_TYPECK_TYPE_FN_POINTER;
        type.data.fn_pointer_type.parameters = children;
        type.data.fn_pointer_type.parameter_count = parameter_count;
        type.data.fn_pointer_type.abi = abi;
        type.data.fn_pointer_type.is_variadic = scalar != 0u;
        break;
    case CM_HIR_TYPE_ADT_KIND:
        if (!cm_semantic_canonical_def(reader, &definition)) goto done;
        adt = cm_semantic_body_item(reader->hir, definition);
        if (adt == NULL || (adt->kind != CM_HIR_ITEM_STRUCT
                && adt->kind != CM_HIR_ITEM_UNION
                && adt->kind != CM_HIR_ITEM_ENUM)
            || !cm_semantic_canonical_u32(reader, &count)
            || count != adt->generic_parameter_count
            || (count != 0u && (!cm_size_mul((size_t)count,
                    sizeof(*arguments), &bytes)
                || count > reader->size - reader->position))) goto done;
        arguments = count == 0u ? NULL
            : (CmTypeckGenericArg *)cm_alloc_zeroed(1u, bytes);
        for (index = 0u; index < count; ++index) {
            const CmHirGenericParam *parameter;
            CmHirGenericArgKind expected;

            parameter = cm_semantic_canonical_parameter(reader->hir, adt,
                index);
            if (parameter == NULL
                || !cm_semantic_canonical_u8(reader, &scalar)) goto done;
            expected = parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_HIR_GENERIC_ARG_LIFETIME
                : parameter->kind == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
            if (scalar != (unsigned int)expected
                || !cm_semantic_canonical_argument(reader, expected,
                    depth + 1u, &arguments[index])) goto done;
        }
        type.kind = CM_TYPECK_TYPE_ADT;
        type.data.named_type.definition = definition;
        type.data.named_type.arguments = arguments;
        type.data.named_type.argument_count = count;
        if (!cm_typeck_adt_is_valid(reader->typeck,
                &type.data.named_type)) goto done;
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        goto done;
    default:
        goto done;
    }
    ok = cm_typeck_add_type(reader->typeck, &type, out_type)
        == CM_TYPECK_OK;
done:
    cm_free(children);
    cm_free(arguments);
    return ok;
}

static CmSemanticBodyStatus cm_semantic_body_import_canonical_arguments(
    const CmHirContext *hir, CmTypeckContext *typeck, const CmHirItem *owner,
    const CmHirCanonicalArgumentPart *source, uint32_t count, CmSpan span,
    CmTypeckGenericArg **out_arguments)
{
    CmTypeckGenericArg *arguments;
    uint32_t index;

    if (hir == NULL || typeck == NULL || owner == NULL
        || out_arguments == NULL
        || owner->generic_parameter_count != count
        || (count == 0u) != (source == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_arguments = NULL;
    if (cm_semantic_body_allocate_arguments(count, &arguments)
            != CM_SEMANTIC_BODY_OK) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    for (index = 0u; index < count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected;
        CmSemanticCanonicalReader reader;

        parameter = cm_semantic_canonical_parameter(hir, owner, index);
        expected = parameter == NULL ? (CmHirGenericArgKind)-1
            : parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_HIR_GENERIC_ARG_LIFETIME
                : parameter->kind == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || source[index].kind != expected
            || source[index].bytes == NULL || source[index].size == 0u) {
            cm_free(arguments);
            return CM_SEMANTIC_BODY_INVALID;
        }
        memset(&reader, 0, sizeof(reader));
        reader.hir = hir;
        reader.typeck = typeck;
        reader.bytes = source[index].bytes;
        reader.size = source[index].size;
        reader.span = span;
        if (!cm_semantic_canonical_argument(&reader, expected, 0u,
                &arguments[index])
            || reader.position != reader.size) {
            cm_free(arguments);
            return CM_SEMANTIC_BODY_INVALID;
        }
    }
    *out_arguments = arguments;
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_import_canonical_type(
    const CmHirContext *hir, CmTypeckContext *typeck,
    const unsigned char *bytes, size_t size, CmSpan span,
    CmTypeckTypeId *out_type)
{
    CmSemanticCanonicalReader reader;

    if (hir == NULL || typeck == NULL || bytes == NULL || size == 0u
        || out_type == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    memset(&reader, 0, sizeof(reader));
    reader.hir = hir;
    reader.typeck = typeck;
    reader.bytes = bytes;
    reader.size = size;
    reader.span = span;
    return cm_semantic_canonical_type(&reader, 1u, out_type)
            && reader.position == reader.size
        ? CM_SEMANTIC_BODY_OK : CM_SEMANTIC_BODY_INVALID;
}

static CmSemanticBodyStatus cm_semantic_body_import_generic_arguments(
    const CmHirContext *hir, CmTypeckContext *typeck,
    CmTypeckStatus *out_typeck_status, const CmHirItem *owner,
    const CmHirGenericArg *source, uint32_t count,
    CmTypeckGenericArg **out_arguments)
{
    CmTypeckGenericArg *arguments;
    uint32_t index;

    if (hir == NULL || typeck == NULL || out_typeck_status == NULL
        || owner == NULL || out_arguments == NULL
        || owner->generic_parameter_count != count
        || (count == 0u) != (source == NULL)
        || !cm_semantic_type_only_owner(hir, owner, count)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_arguments = NULL;
    if (count == 0u) return CM_SEMANTIC_BODY_OK;
    if (cm_semantic_body_allocate_arguments(count, &arguments)
            != CM_SEMANTIC_BODY_OK) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    for (index = 0u; index < count; ++index) {
        CmSemanticTypeScan scan;
        CmTypeckStatus typeck_status;

        if (source[index].kind != CM_HIR_GENERIC_ARG_TYPE) {
            cm_free(arguments);
            return CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        }
        scan = cm_semantic_scan_type(hir, source[index].data.type, 0u);
        if (scan != CM_SEMANTIC_TYPE_OK) {
            cm_free(arguments);
            return cm_semantic_scan_status(scan);
        }
        arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        typeck_status = cm_typeck_import_hir_type(typeck,
            source[index].data.type, &arguments[index].data.type);
        if (typeck_status != CM_TYPECK_OK) {
            *out_typeck_status = typeck_status;
            cm_free(arguments);
            return cm_semantic_typeck_status(typeck_status);
        }
    }
    *out_arguments = arguments;
    return CM_SEMANTIC_BODY_OK;
}

static int cm_semantic_body_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
    case CM_HIR_REGION_INFER:
    case CM_HIR_REGION_ERROR:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.parameter == right->data.parameter;
    }
    return 0;
}

static void cm_semantic_body_callable_facts_clear(
    CmSemanticCheckedCallableFacts *facts)
{
    if (facts == NULL) return;
    cm_free((void *)facts->item_argument_inputs);
    cm_free((void *)facts->item_arguments);
    cm_free((void *)facts->method_argument_inputs);
    cm_free((void *)facts->method_arguments);
    cm_free((void *)facts->enclosing_impl_argument_inputs);
    cm_free((void *)facts->enclosing_impl_arguments);
    cm_free((void *)facts->implemented_trait_argument_inputs);
    cm_free((void *)facts->implemented_trait_arguments);
    cm_free((void *)facts->argument_expressions);
    cm_free((void *)facts->parameter_input_types);
    cm_free((void *)facts->parameter_types);
    memset(facts, 0, sizeof(*facts));
}

static CmSemanticBodyStatus cm_semantic_body_copy_generic_arguments(
    const CmTypeckGenericArg *source, uint32_t count,
    const CmTypeckGenericArg **out_inputs,
    const CmTypeckGenericArg **out_arguments)
{
    CmTypeckGenericArg *inputs;
    CmTypeckGenericArg *arguments;
    size_t bytes;

    if (out_inputs == NULL || out_arguments == NULL
        || (count == 0u) != (source == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_inputs = NULL;
    *out_arguments = NULL;
    if (count == 0u) return CM_SEMANTIC_BODY_OK;
    if (!cm_size_mul((size_t)count, sizeof(*source), &bytes)) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    inputs = (CmTypeckGenericArg *)cm_alloc(bytes);
    arguments = (CmTypeckGenericArg *)cm_alloc(bytes);
    memcpy(inputs, source, bytes);
    memcpy(arguments, source, bytes);
    *out_inputs = inputs;
    *out_arguments = arguments;
    return CM_SEMANTIC_BODY_OK;
}

typedef struct CmSemanticBodyConstraints CmSemanticBodyConstraints;

typedef struct CmSemanticBodyEquality {
    CmTypeckTypeId left;
    CmTypeckTypeId right;
    CmHirExprId expression;
    CmHirDefId callee;
    uint32_t predicate_index;
    int resolved;
} CmSemanticBodyEquality;

struct CmSemanticBodyConstraints {
    CmSemanticSession *session;
    CmTypeckContext *typeck;
    const CmHirContext *hir;
    const CmHirBody *body;
    CmHirBodyId body_id;
    CmTypeckScopedInstantiation *owner_instantiation;
    const CmParamEnvSubstitution *substitution;
    CmProjectionNormalizeLimits normalize_limits;
    CmVec *deferred_equalities;
    unsigned char *defined_locals;
    CmTypeckTypeId *expression_terms;
    size_t expression_term_count;
    CmSemanticCheckedBodyFacts *checked_facts;
    const CmSemanticBodyEvidenceWriteback *evidence_writeback;
    CmHirExprId failed_expression;
    CmHirDefId failed_callee;
    uint32_t failed_predicate_index;
    CmTypeckStatus typeck_status;
    CmTraitSolverResultKind solver_kind;
};

/*
 * HIR call nodes do not carry explicit trait generic arguments.  Build the
 * canonical query shape with one fresh type variable per type parameter so
 * trait selection can infer arguments from the impl header.  This deliberately
 * admits only type parameters: lifetime/const arguments need source syntax or
 * a richer inference policy and remain outside this body slice.
 */
static CmSemanticBodyStatus cm_semantic_body_trait_query(
    CmSemanticBodyConstraints *constraints, const CmHirItem *trait_item,
    CmTypeckNamedType *out_trait)
{
    uint32_t index;

    if (constraints == NULL || trait_item == NULL || out_trait == NULL
        || trait_item->kind != CM_HIR_ITEM_TRAIT) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    memset(out_trait, 0, sizeof(*out_trait));
    out_trait->definition = trait_item->definition;
    out_trait->argument_count = trait_item->generic_parameter_count;
    if (out_trait->argument_count == 0u) return CM_SEMANTIC_BODY_OK;
    out_trait->arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(
        out_trait->argument_count, sizeof(*out_trait->arguments));
    for (index = 0u; index < out_trait->argument_count; ++index) {
        const CmHirGenericParam *parameter;
        CmTypeckStatus typeck_status;

        parameter = cm_hir_get_generic_param(constraints->hir,
            trait_item->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            cm_free(out_trait->arguments);
            out_trait->arguments = NULL;
            out_trait->argument_count = 0u;
            return CM_SEMANTIC_BODY_UNSUPPORTED;
        }
        out_trait->arguments[index].kind = CM_HIR_GENERIC_ARG_TYPE;
        typeck_status = cm_typeck_new_variable(constraints->typeck,
            CM_HIR_INFER_GENERAL, parameter->span,
            &out_trait->arguments[index].data.type);
        if (typeck_status != CM_TYPECK_OK) {
            cm_free(out_trait->arguments);
            out_trait->arguments = NULL;
            out_trait->argument_count = 0u;
            constraints->typeck_status = typeck_status;
            return cm_semantic_typeck_status(typeck_status);
        }
    }
    return CM_SEMANTIC_BODY_OK;
}

/* Instantiate an impl header's trait arguments and retain both the query
 * inputs and solved arguments for semantic-results writeback. */
static CmSemanticBodyStatus cm_semantic_body_copy_trait_arguments(
    CmSemanticBodyConstraints *constraints, const CmHirItem *impl_item,
    const CmTypeckInstantiation *impl_instantiation,
    const CmTypeckNamedType *query_trait,
    const CmHirDefId expected_trait,
    const CmTypeckGenericArg **out_inputs,
    const CmTypeckGenericArg **out_arguments, uint32_t *out_count)
{
    CmTypeckInstantiationFrame frame;
    CmTypeckScopedInstantiation scoped;
    CmTypeckNamedType implemented;
    CmTypeckStatus typeck_status;
    CmSemanticBodyStatus status;

    if (constraints == NULL || impl_item == NULL || impl_instantiation == NULL
        || query_trait == NULL || out_inputs == NULL
        || out_arguments == NULL || out_count == NULL
        || impl_item->kind != CM_HIR_ITEM_IMPL
        || !impl_item->data.impl_item.has_trait
        || !cm_hir_def_id_equal(impl_item->data.impl_item.trait_type
            .definition, expected_trait)
        || query_trait->argument_count
            != impl_item->data.impl_item.trait_type.argument_count) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_inputs = NULL;
    *out_arguments = NULL;
    *out_count = 0u;
    memset(&frame, 0, sizeof(frame));
    frame.parameter_owner = impl_item->definition;
    frame.arguments = impl_instantiation->arguments;
    frame.argument_count = impl_instantiation->argument_count;
    cm_typeck_scoped_instantiation_init(constraints->typeck, &scoped);
    scoped.frames = &frame;
    scoped.frame_count = 1u;
    if (!cm_typeck_scoped_instantiation_is_valid(constraints->typeck,
            &scoped)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    memset(&implemented, 0, sizeof(implemented));
    typeck_status = cm_typeck_instantiate_hir_named_scoped(
        constraints->typeck, &impl_item->data.impl_item.trait_type,
        &scoped, &implemented);
    if (typeck_status != CM_TYPECK_OK) {
        constraints->typeck_status = typeck_status;
        return cm_semantic_typeck_status(typeck_status);
    }
    if (!cm_hir_def_id_equal(implemented.definition, expected_trait)
        || implemented.argument_count != query_trait->argument_count
        || (implemented.argument_count != 0u
            && implemented.arguments == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    if (query_trait->argument_count == 0u) {
        status = CM_SEMANTIC_BODY_OK;
    } else {
        size_t bytes;

        if (!cm_size_mul((size_t)query_trait->argument_count,
                sizeof(*query_trait->arguments), &bytes)) {
            status = CM_SEMANTIC_BODY_OVERFLOW;
        } else {
            *out_inputs = (CmTypeckGenericArg *)cm_alloc(bytes);
            *out_arguments = (CmTypeckGenericArg *)cm_alloc(bytes);
            memcpy((void *)*out_inputs, query_trait->arguments, bytes);
            memcpy((void *)*out_arguments, implemented.arguments, bytes);
            status = CM_SEMANTIC_BODY_OK;
        }
    }
    *out_count = implemented.argument_count;
    if (status != CM_SEMANTIC_BODY_OK) {
        cm_free((void *)*out_inputs);
        cm_free((void *)*out_arguments);
        *out_inputs = NULL;
        *out_arguments = NULL;
        *out_count = 0u;
    }
    return status;
}

static int cm_semantic_body_typeck_arguments_match_hir(
    CmSemanticBodyConstraints *constraints,
    const CmTypeckGenericArg *expected,
    const CmHirGenericArg *actual, uint32_t count)
{
    uint32_t index;

    if (constraints == NULL || (count == 0u) != (expected == NULL)
        || (count == 0u) != (actual == NULL)) return 0;
    for (index = 0u; index < count; ++index) {
        if (expected[index].kind != actual[index].kind) return 0;
        if (actual[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            CmTypeckTypeId actual_type;

            if (cm_typeck_import_hir_type(constraints->typeck,
                    actual[index].data.type, &actual_type) != CM_TYPECK_OK
                || cm_typeck_unify(constraints->typeck,
                    expected[index].data.type, actual_type)
                    != CM_TYPECK_OK) return 0;
        } else if (actual[index].kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_semantic_body_region_equal(
                    &expected[index].data.lifetime,
                    &actual[index].data.lifetime)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_semantic_body_typeck_arguments_match(
    CmSemanticBodyConstraints *constraints,
    const CmTypeckGenericArg *expected,
    const CmTypeckGenericArg *actual, uint32_t count)
{
    uint32_t index;

    if (constraints == NULL || (count == 0u) != (expected == NULL)
        || (count == 0u) != (actual == NULL)) return 0;
    for (index = 0u; index < count; ++index) {
        if (expected[index].kind != actual[index].kind) return 0;
        switch (actual[index].kind) {
        case CM_HIR_GENERIC_ARG_TYPE:
            if (cm_typeck_unify(constraints->typeck,
                    expected[index].data.type, actual[index].data.type)
                    != CM_TYPECK_OK) return 0;
            break;
        case CM_HIR_GENERIC_ARG_LIFETIME:
            if (!cm_semantic_body_region_equal(
                    &expected[index].data.lifetime,
                    &actual[index].data.lifetime)) return 0;
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            if (expected[index].data.constant.kind != CM_HIR_CONST_VALUE
                || actual[index].data.constant.kind != CM_HIR_CONST_VALUE
                || expected[index].data.constant.data.value.low_bits
                    != actual[index].data.constant.data.value.low_bits
                || expected[index].data.constant.data.value.high_bits
                    != actual[index].data.constant.data.value.high_bits
                || cm_typeck_unify(constraints->typeck,
                    expected[index].data.constant.type,
                    actual[index].data.constant.type) != CM_TYPECK_OK) {
                return 0;
            }
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static CmSemanticBodyStatus cm_semantic_body_normalize_status(
    CmSemanticBodyConstraints *constraints,
    const CmProjectionNormalizeResult *normalization)
{
    if (constraints == NULL || normalization == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    constraints->typeck_status = normalization->typeck_status;
    constraints->solver_kind = normalization->kind;
    return cm_semantic_solver_status(normalization->kind);
}

static CmProjectionNormalizeResult cm_semantic_body_normalize(
    CmSemanticBodyConstraints *constraints, CmTypeckTypeId type)
{
    return cm_semantic_session_normalize_type(constraints->session,
        constraints->typeck, constraints->substitution, type,
        constraints->normalize_limits);
}

static CmSemanticBodyStatus cm_semantic_body_normalize_named(
    CmSemanticBodyConstraints *constraints, CmTypeckNamedType *named)
{
    uint32_t index;

    if (constraints == NULL || named == NULL
        || cm_hir_def_id_is_none(named->definition)
        || (named->argument_count == 0u) != (named->arguments == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        CmProjectionNormalizeResult normalization;
        CmTypeckTypeId *type;

        if (named->arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            type = &named->arguments[index].data.type;
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_CONST) {
            type = &named->arguments[index].data.constant.type;
        } else if (named->arguments[index].kind
                == CM_HIR_GENERIC_ARG_LIFETIME) {
            continue;
        } else {
            return CM_SEMANTIC_BODY_INVALID;
        }
        normalization = cm_semantic_body_normalize(constraints, *type);
        if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
            return cm_semantic_body_normalize_status(constraints,
                &normalization);
        }
        *type = normalization.type;
    }
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_try_equality(
    CmSemanticBodyConstraints *constraints, CmTypeckTypeId left,
    CmTypeckTypeId right, int *out_deferred)
{
    CmTypeckSnapshot snapshot;
    CmProjectionNormalizeResult left_normalized;
    CmProjectionNormalizeResult right_normalized;
    CmSemanticBodyStatus semantic_status;
    CmTypeckStatus status;

    if (constraints == NULL || out_deferred == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_deferred = 0;
    memset(&snapshot, 0, sizeof(snapshot));
    status = cm_typeck_snapshot(constraints->typeck, &snapshot);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    left_normalized = cm_semantic_body_normalize(constraints, left);
    if (left_normalized.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE) {
        *out_deferred = 1;
        semantic_status = CM_SEMANTIC_BODY_OK;
        goto rollback;
    }
    if (left_normalized.kind != CM_TRAIT_SOLVER_PROVEN) {
        semantic_status = cm_semantic_body_normalize_status(constraints,
            &left_normalized);
        goto rollback;
    }
    right_normalized = cm_semantic_body_normalize(constraints, right);
    if (right_normalized.kind == CM_TRAIT_SOLVER_DEFERRED_INFERENCE) {
        *out_deferred = 1;
        semantic_status = CM_SEMANTIC_BODY_OK;
        goto rollback;
    }
    if (right_normalized.kind != CM_TRAIT_SOLVER_PROVEN) {
        semantic_status = cm_semantic_body_normalize_status(constraints,
            &right_normalized);
        goto rollback;
    }
    status = cm_typeck_unify(constraints->typeck, left_normalized.type,
        right_normalized.type);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        semantic_status = cm_semantic_typeck_status(status);
        goto rollback;
    }
    status = cm_typeck_commit(constraints->typeck, &snapshot);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    return CM_SEMANTIC_BODY_OK;

rollback:
    status = cm_typeck_rollback(constraints->typeck, &snapshot);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    return semantic_status;
}

static CmSemanticBodyStatus cm_semantic_body_unify_terms(
    CmSemanticBodyConstraints *constraints, CmTypeckTypeId left,
    CmTypeckTypeId right)
{
    CmSemanticBodyEquality equality;
    CmSemanticBodyStatus status;
    int deferred;

    status = cm_semantic_body_try_equality(constraints, left, right,
        &deferred);
    if (status != CM_SEMANTIC_BODY_OK || !deferred) return status;
    memset(&equality, 0, sizeof(equality));
    equality.left = left;
    equality.right = right;
    equality.expression = constraints->failed_expression;
    equality.callee = constraints->failed_callee;
    equality.predicate_index = constraints->failed_predicate_index;
    if (constraints->deferred_equalities == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    (void)cm_vec_push(constraints->deferred_equalities, &equality);
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_retry_equalities(
    CmSemanticBodyConstraints *constraints)
{
    size_t unresolved;
    size_t pass;

    if (constraints == NULL) return CM_SEMANTIC_BODY_INVALID;
    if (constraints->deferred_equalities == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    unresolved = constraints->deferred_equalities->len;
    for (pass = 0u; unresolved != 0u
            && pass < constraints->deferred_equalities->len; ++pass) {
        size_t index;
        size_t progress;

        progress = 0u;
        for (index = 0u; index < constraints->deferred_equalities->len;
             ++index) {
            CmSemanticBodyEquality *equality;
            CmSemanticBodyStatus status;
            int deferred;

            equality = (CmSemanticBodyEquality *)cm_vec_at(
                constraints->deferred_equalities, index);
            if (equality == NULL || equality->resolved) continue;
            constraints->failed_expression = equality->expression;
            constraints->failed_callee = equality->callee;
            constraints->failed_predicate_index =
                equality->predicate_index;
            status = cm_semantic_body_try_equality(constraints,
                equality->left, equality->right, &deferred);
            if (status != CM_SEMANTIC_BODY_OK) return status;
            if (!deferred) {
                equality->resolved = 1;
                --unresolved;
                ++progress;
            }
        }
        if (progress == 0u) break;
    }
    return unresolved == 0u ? CM_SEMANTIC_BODY_OK
        : CM_SEMANTIC_BODY_DEFERRED_INFERENCE;
}

static CmSemanticBodyStatus cm_semantic_body_normalize_expressions(
    CmSemanticBodyConstraints *constraints)
{
    size_t index;

    if (constraints == NULL) return CM_SEMANTIC_BODY_INVALID;
    for (index = 0u; index < constraints->expression_term_count; ++index) {
        const CmHirExpr *expression;
        CmProjectionNormalizeResult normalization;
        CmProjectionNormalizeTrace trace;
        CmTypeckTypeId input_type;

        expression = cm_hir_get_expr(constraints->hir,
            (CmHirExprId)(index + 1u));
        if (expression == NULL) return CM_SEMANTIC_BODY_INVALID;
        if (expression->owner_body != constraints->body_id) {
            if (constraints->expression_terms[index]
                    != CM_TYPECK_TYPE_NONE) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            continue;
        }
        if (constraints->expression_terms[index]
                == CM_TYPECK_TYPE_NONE) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = (CmHirExprId)(index + 1u);
        input_type = constraints->expression_terms[index];
        if (constraints->evidence_writeback != NULL
            && constraints->evidence_writeback->projection_decision != NULL) {
            cm_projection_normalize_trace_init(&trace);
            normalization = cm_semantic_session_normalize_type_traced(
                constraints->session, constraints->typeck,
                constraints->substitution, input_type,
                constraints->normalize_limits, &trace);
        } else {
            memset(&trace, 0, sizeof(trace));
            normalization = cm_semantic_body_normalize(constraints,
                input_type);
        }
        if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
            cm_projection_normalize_trace_destroy(&trace);
            return cm_semantic_body_normalize_status(constraints,
                &normalization);
        }
        constraints->expression_terms[index] = normalization.type;
        if (cm_projection_normalize_trace_count(&trace) != 0u) {
            CmSemanticBodyWritebackStatus writeback_status;

            writeback_status = constraints->evidence_writeback
                ->projection_decision(
                    constraints->evidence_writeback->context,
                    constraints->session, constraints->body_id,
                    (CmHirExprId)(index + 1u),
                    CM_SEMANTIC_PROJECTION_DECISION_EXPRESSION_TYPE, 0u,
                    input_type, normalization.type, &trace);
            cm_projection_normalize_trace_destroy(&trace);
            if (writeback_status != CM_SEMANTIC_BODY_WRITEBACK_OK) {
                return writeback_status
                        == CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW
                    ? CM_SEMANTIC_BODY_OVERFLOW
                    : writeback_status
                            == CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED
                        ? CM_SEMANTIC_BODY_UNSUPPORTED
                        : CM_SEMANTIC_BODY_INVALID;
            }
        } else {
            cm_projection_normalize_trace_destroy(&trace);
        }
    }
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_instantiate_type(
    CmSemanticBodyConstraints *constraints, CmHirTypeId hir_type,
    const CmTypeckInstantiation *instantiation, CmTypeckTypeId *out_type)
{
    CmSemanticTypeScan scan;
    CmTypeckStatus status;

    if (constraints == NULL || instantiation == NULL || out_type == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    scan = cm_semantic_scan_type(constraints->hir, hir_type, 0u);
    if (scan != CM_SEMANTIC_TYPE_OK
        && scan != CM_SEMANTIC_TYPE_PROJECTION) {
        return cm_semantic_scan_status(scan);
    }
    status = cm_typeck_instantiate_hir_type(constraints->typeck, hir_type,
        instantiation, out_type);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_instantiate_type_scoped(
    CmSemanticBodyConstraints *constraints, CmHirTypeId hir_type,
    const CmTypeckScopedInstantiation *instantiation,
    CmTypeckTypeId *out_type)
{
    CmSemanticTypeScan scan;
    CmTypeckStatus status;

    if (constraints == NULL || instantiation == NULL || out_type == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    scan = cm_semantic_scan_type(constraints->hir, hir_type, 0u);
    if (scan != CM_SEMANTIC_TYPE_OK
        && scan != CM_SEMANTIC_TYPE_PROJECTION) {
        return cm_semantic_scan_status(scan);
    }
    status = cm_typeck_instantiate_hir_type_scoped(constraints->typeck,
        hir_type, instantiation, out_type);
    if (status != CM_TYPECK_OK) {
        constraints->typeck_status = status;
        return cm_semantic_typeck_status(status);
    }
    return CM_SEMANTIC_BODY_OK;
}

static int cm_semantic_body_refresh_owner_instantiation(
    CmSemanticBodyConstraints *constraints)
{
    const CmTypeckInstantiationFrame *frames;
    uint32_t frame_count;
    CmHirDefId self_owner;
    CmTypeckTypeId self_type;

    if (constraints == NULL || constraints->owner_instantiation == NULL)
        return 0;
    frames = constraints->owner_instantiation->frames;
    frame_count = constraints->owner_instantiation->frame_count;
    self_owner = constraints->owner_instantiation->self_owner;
    self_type = constraints->owner_instantiation->self_type;
    cm_typeck_scoped_instantiation_init(constraints->typeck,
        constraints->owner_instantiation);
    constraints->owner_instantiation->frames = frames;
    constraints->owner_instantiation->frame_count = frame_count;
    constraints->owner_instantiation->self_owner = self_owner;
    constraints->owner_instantiation->self_type = self_type;
    return cm_typeck_scoped_instantiation_is_valid(constraints->typeck,
        constraints->owner_instantiation);
}

static CmSemanticBodyStatus cm_semantic_body_instantiate_owner_type(
    CmSemanticBodyConstraints *constraints, CmHirTypeId hir_type,
    CmTypeckTypeId *out_type)
{
    if (!cm_semantic_body_refresh_owner_instantiation(constraints)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    return cm_semantic_body_instantiate_type_scoped(constraints, hir_type,
        constraints->owner_instantiation, out_type);
}

static CmSemanticBodyStatus cm_semantic_body_expression_term(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression,
    CmTypeckTypeId *out_type);

static CmSemanticBodyStatus cm_semantic_body_check_call_signature(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression_id,
    const CmHirExpr *expression, const CmHirItem *callee,
    const CmTypeckInstantiation *callee_instantiation,
    CmSemanticCheckedCallFacts *facts)
{
    const CmHirFunctionSignature *signature;
    CmTypeckTypeId actual_type;
    CmTypeckTypeId declared_type;
    CmSemanticBodyStatus status;
    uint32_t index;

    if (constraints == NULL || expression == NULL
        || callee_instantiation == NULL || facts == NULL
        || ((facts->parameter_types == NULL)
            != (expression->data.call.argument_count == 0u))) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    signature = callee == NULL ? NULL : &callee->data.function_item.signature;
    if (signature == NULL
        || signature->parameter_count != expression->data.call.argument_count
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    constraints->failed_expression = expression_id;
    status = cm_semantic_body_expression_term(constraints, expression_id,
        &actual_type);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    status = cm_semantic_body_instantiate_type(constraints,
        signature->return_type, callee_instantiation, &declared_type);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    facts->expression = expression_id;
    facts->callee = callee->definition;
    facts->return_type = declared_type;
    facts->parameter_count = signature->parameter_count;
    status = cm_semantic_body_unify_terms(constraints, actual_type,
        declared_type);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    for (index = 0u; index < signature->parameter_count; ++index) {
        CmHirExprId argument_id;
        const CmHirExpr *argument;

        argument_id = expression->data.call.arguments[index];
        argument = cm_hir_get_expr(constraints->hir, argument_id);
        if (argument == NULL
            || argument->owner_body != expression->owner_body) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = argument_id;
        status = cm_semantic_body_expression_term(constraints, argument_id,
            &actual_type);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        status = cm_semantic_body_instantiate_type(constraints,
            signature->parameters[index].type, callee_instantiation,
            &declared_type);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        ((CmTypeckTypeId *)facts->parameter_types)[index] = declared_type;
        status = cm_semantic_body_unify_terms(constraints, actual_type,
            declared_type);
        if (status != CM_SEMANTIC_BODY_OK) return status;
    }
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_check_qualified_callable(
    CmSemanticBodyConstraints *constraints,
    const CmParamEnvSubstitution *environment_substitution,
    CmHirExprId expression_id, const CmHirExpr *expression,
    CmSemanticCheckedCallableFacts *facts)
{
    const CmHirItem *requested_trait_item;
    const CmHirItem *impl_item;
    const CmHirItem *declared_callable;
    const CmHirItem *selected_callable;
    const CmHirFunctionSignature *signature;
    CmTraitImplSelectionWitness witness;
    CmTraitGoal goal;
    CmTraitSelectionResult selection;
    CmTypeckInstantiation impl_instantiation;
    CmTypeckNamedType query_trait;
    CmTypeckInstantiationFrame frames[2];
    CmTypeckScopedInstantiation callable_instantiation;
    CmTypeckTypeId actual_type;
    CmTypeckTypeId declared_type;
    CmSemanticBodyStatus status;
    size_t index;
    size_t matches;

    if (constraints == NULL || environment_substitution == NULL
        || constraints->owner_instantiation == NULL || expression == NULL
        || facts == NULL
        || expression->kind != CM_HIR_EXPR_QUALIFIED_CALL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    memset(facts, 0, sizeof(*facts));
    memset(&query_trait, 0, sizeof(query_trait));
    cm_trait_impl_selection_witness_init(&witness);
    memset(&goal, 0, sizeof(goal));
    goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
    goal.data.implemented.owner = constraints->body->owner;
    requested_trait_item = cm_semantic_body_item(constraints->hir,
        expression->data.qualified_call.requested_trait);
    status = cm_semantic_body_trait_query(constraints, requested_trait_item,
        &query_trait);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    goal.data.implemented.trait_type.definition =
        expression->data.qualified_call.requested_trait;
    goal.data.implemented.trait_type.arguments = query_trait.arguments;
    goal.data.implemented.trait_type.argument_count =
        query_trait.argument_count;
    status = cm_semantic_body_instantiate_owner_type(constraints,
        expression->data.qualified_call.requested_self_type,
        &goal.data.implemented.self_type);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    selection = cm_semantic_session_solve_goal_with_impl_witness(
        constraints->session, constraints->typeck,
        environment_substitution, &goal, &witness);
    constraints->solver_kind = selection.kind;
    constraints->typeck_status = selection.typeck_status;
    status = cm_semantic_solver_status(selection.kind);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    if (selection.proof_origin != CM_TRAIT_PROOF_IMPL
        || cm_hir_def_id_is_none(selection.impl_definition)
        || selection.supported_match_count != 1u
        || selection.negative_match_count != 0u
        || selection.blocking_match_count != 0u) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    impl_item = cm_semantic_body_item(constraints->hir,
        selection.impl_definition);
    if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
        || impl_item->definition.crate_id
            != expression->data.qualified_call.requested_trait.crate_id
        || impl_item->predicate_scope_count != 0u
        || impl_item->predicate_count != 0u
        || impl_item->outlives_predicate_count != 0u
        || !cm_semantic_type_only_owner(constraints->hir, impl_item,
            impl_item->generic_parameter_count)
        || !impl_item->data.impl_item.has_trait
        || impl_item->data.impl_item.is_negative
        || impl_item->data.impl_item.trait_type.argument_count
            != query_trait.argument_count
        || (impl_item->data.impl_item.trait_type.argument_count != 0u)
            != (impl_item->data.impl_item.trait_type.arguments != NULL)
        || !cm_hir_def_id_equal(impl_item->data.impl_item.trait_type
            .definition,
            expression->data.qualified_call.requested_trait)) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    declared_callable = cm_semantic_body_item(constraints->hir,
        expression->data.qualified_call.declared_trait_callable);
    if (declared_callable == NULL
        || declared_callable->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(declared_callable->parent_definition,
            expression->data.qualified_call.requested_trait)
        || !cm_hir_def_id_is_none(declared_callable->data.function_item
            .trait_item_definition)) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    selected_callable = NULL;
    matches = 0u;
    for (index = 0u; index < constraints->hir->items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &constraints->hir->items, index);
        if (candidate != NULL && candidate->kind == CM_HIR_ITEM_FUNCTION
            && cm_hir_def_id_equal(candidate->parent_definition,
                impl_item->definition)
            && cm_hir_def_id_equal(candidate->data.function_item
                .trait_item_definition,
                expression->data.qualified_call.declared_trait_callable)) {
            selected_callable = candidate;
            ++matches;
        }
    }
    if (matches == 0u
        && declared_callable->data.function_item.body != CM_HIR_BODY_NONE) {
        selected_callable = declared_callable;
    }
    if (matches > 1u || selected_callable == NULL
        || selected_callable->generic_parameter_count != 0u
        || selected_callable->predicate_scope_count != 0u
        || selected_callable->predicate_count != 0u
        || selected_callable->outlives_predicate_count != 0u
        || selected_callable->data.function_item.body == CM_HIR_BODY_NONE) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    signature = &selected_callable->data.function_item.signature;
    if (signature->parameter_count
            != expression->data.qualified_call.argument_count
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)
        || (signature->receiver == CM_HIR_RECEIVER_NONE
                ? CM_HIR_CALLABLE_RECEIVER_NONE : 0u)
            != expression->data.qualified_call.receiver_argument) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    facts->argument_count = expression->data.qualified_call.argument_count;
    facts->parameter_count = signature->parameter_count;
    if (facts->argument_count != 0u) {
        facts->argument_expressions = (CmHirExprId *)cm_alloc_zeroed(
            facts->argument_count, sizeof(CmHirExprId));
        facts->parameter_input_types = (CmTypeckTypeId *)cm_alloc_zeroed(
            facts->parameter_count, sizeof(CmTypeckTypeId));
        facts->parameter_types = (CmTypeckTypeId *)cm_alloc_zeroed(
            facts->parameter_count, sizeof(CmTypeckTypeId));
    }
    if (!cm_trait_impl_selection_witness_instantiation(&witness,
            constraints->typeck, &impl_instantiation)
        || !cm_hir_def_id_equal(impl_instantiation.parameter_owner,
            selection.impl_definition)) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    status = cm_semantic_body_copy_generic_arguments(
        impl_instantiation.arguments, impl_instantiation.argument_count,
        &facts->enclosing_impl_argument_inputs,
        &facts->enclosing_impl_arguments);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    facts->enclosing_impl_argument_count = impl_instantiation.argument_count;
    memset(frames, 0, sizeof(frames));
    frames[0].parameter_owner = selected_callable->definition;
    frames[1].parameter_owner = impl_item->definition;
    frames[1].arguments = facts->enclosing_impl_arguments;
    frames[1].argument_count = facts->enclosing_impl_argument_count;
    cm_typeck_scoped_instantiation_init(constraints->typeck,
        &callable_instantiation);
    callable_instantiation.frames = frames;
    callable_instantiation.frame_count = 2u;
    callable_instantiation.self_owner = selected_callable
            == declared_callable
        ? expression->data.qualified_call.requested_trait
        : impl_item->definition;
    callable_instantiation.self_type = goal.data.implemented.self_type;
    if (!cm_typeck_scoped_instantiation_is_valid(constraints->typeck,
            &callable_instantiation)) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto cleanup;
    }
    status = cm_semantic_body_copy_trait_arguments(constraints, impl_item,
        &impl_instantiation, &query_trait,
        expression->data.qualified_call.requested_trait,
        &facts->implemented_trait_argument_inputs,
        &facts->implemented_trait_arguments,
        &facts->implemented_trait_argument_count);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    facts->expression = expression_id;
    facts->syntax = expression->data.qualified_call.syntax;
    facts->requested_self_type = goal.data.implemented.self_type;
    facts->requested_self_input_type = facts->requested_self_type;
    facts->requested_trait = expression->data.qualified_call.requested_trait;
    facts->declared_trait_callable =
        expression->data.qualified_call.declared_trait_callable;
    facts->selected_impl = impl_item->definition;
    facts->selected_callable = selected_callable->definition;
    facts->body_definition = selected_callable->definition;
    facts->enclosing_impl = impl_item->definition;
    facts->implemented_trait = expression->data.qualified_call.requested_trait;
    facts->self_owner = impl_item->definition;
    facts->receiver_argument =
        expression->data.qualified_call.receiver_argument;
    facts->receiver_expression = facts->receiver_argument
            == CM_HIR_CALLABLE_RECEIVER_NONE
        ? CM_HIR_EXPR_NONE
        : expression->data.qualified_call.arguments[
            facts->receiver_argument];
    constraints->failed_callee = selected_callable->definition;
    status = cm_semantic_body_expression_term(constraints, expression_id,
        &actual_type);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    status = cm_semantic_body_instantiate_type_scoped(constraints,
        signature->return_type, &callable_instantiation, &declared_type);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    facts->return_type = declared_type;
    facts->return_input_type = declared_type;
    status = cm_semantic_body_unify_terms(constraints, actual_type,
        declared_type);
    if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    for (index = 0u; index < signature->parameter_count; ++index) {
        CmHirExprId argument;

        argument = expression->data.qualified_call.arguments[index];
        ((CmHirExprId *)facts->argument_expressions)[index] = argument;
        constraints->failed_expression = argument;
        status = cm_semantic_body_expression_term(constraints, argument,
            &actual_type);
        if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
        status = cm_semantic_body_instantiate_type_scoped(constraints,
            signature->parameters[index].type, &callable_instantiation,
            &declared_type);
        if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
        ((CmTypeckTypeId *)facts->parameter_input_types)[index] =
            declared_type;
        ((CmTypeckTypeId *)facts->parameter_types)[index] = declared_type;
        status = cm_semantic_body_unify_terms(constraints, actual_type,
            declared_type);
        if (status != CM_SEMANTIC_BODY_OK) goto cleanup;
    }
    status = CM_SEMANTIC_BODY_OK;
cleanup:
    cm_free(query_trait.arguments);
    cm_trait_impl_selection_witness_destroy(&witness);
    if (status != CM_SEMANTIC_BODY_OK) {
        cm_semantic_body_callable_facts_clear(facts);
    }
    return status;
}

static int cm_semantic_body_method_receiver_supported(
    const CmHirContext *hir, const CmHirExpr *receiver)
{
    const CmHirType *type;

    type = receiver == NULL ? NULL : cm_hir_get_type(hir, receiver->type);
    return type != NULL && (type->kind == CM_HIR_TYPE_BOOL_KIND
        || type->kind == CM_HIR_TYPE_CHAR_KIND
        || type->kind == CM_HIR_TYPE_INTEGER_KIND
        || type->kind == CM_HIR_TYPE_FLOAT_KIND);
}

static int cm_semantic_body_mutable_local_receiver_supported(
    const CmSemanticBodyConstraints *constraints, const CmHirExpr *receiver)
{
    uint32_t local_index;

    if (constraints == NULL || constraints->body == NULL || receiver == NULL
        || receiver->kind != CM_HIR_EXPR_LOCAL) return 0;
    local_index = receiver->data.local.local_index;
    return local_index < constraints->body->local_count
        && constraints->body->locals[local_index].mutability
            == CM_HIR_MUTABLE;
}

static CmSemanticBodyStatus cm_semantic_body_stronger_method_failure(
    CmSemanticBodyStatus left, CmSemanticBodyStatus right)
{
    static const CmSemanticBodyStatus order[] = {
        CM_SEMANTIC_BODY_NO_SOLUTION,
        CM_SEMANTIC_BODY_NEGATIVE,
        CM_SEMANTIC_BODY_DEFERRED_INFERENCE,
        CM_SEMANTIC_BODY_DEFERRED_METADATA,
        CM_SEMANTIC_BODY_UNSUPPORTED,
        CM_SEMANTIC_BODY_AMBIGUOUS,
        CM_SEMANTIC_BODY_OVERFLOW,
        CM_SEMANTIC_BODY_TYPECK_FAILURE,
        CM_SEMANTIC_BODY_INVALID
    };
    size_t left_rank;
    size_t right_rank;

    left_rank = 0u;
    right_rank = 0u;
    while (left_rank + 1u < sizeof(order) / sizeof(order[0])
        && order[left_rank] != left) {
        ++left_rank;
    }
    while (right_rank + 1u < sizeof(order) / sizeof(order[0])
        && order[right_rank] != right) {
        ++right_rank;
    }
    return right_rank > left_rank ? right : left;
}

static void cm_semantic_body_set_method_solver_kind(
    CmSemanticBodyConstraints *constraints, CmSemanticBodyStatus status)
{
    if (constraints == NULL) return;
    switch (status) {
    case CM_SEMANTIC_BODY_NEGATIVE:
        constraints->solver_kind = CM_TRAIT_SOLVER_NEGATIVE;
        break;
    case CM_SEMANTIC_BODY_NO_SOLUTION:
        constraints->solver_kind = CM_TRAIT_SOLVER_NO_SOLUTION;
        break;
    case CM_SEMANTIC_BODY_AMBIGUOUS:
        constraints->solver_kind = CM_TRAIT_SOLVER_AMBIGUOUS;
        break;
    case CM_SEMANTIC_BODY_DEFERRED_INFERENCE:
        constraints->solver_kind = CM_TRAIT_SOLVER_DEFERRED_INFERENCE;
        break;
    case CM_SEMANTIC_BODY_DEFERRED_METADATA:
        constraints->solver_kind = CM_TRAIT_SOLVER_DEFERRED_METADATA;
        break;
    case CM_SEMANTIC_BODY_UNSUPPORTED:
        constraints->solver_kind = CM_TRAIT_SOLVER_UNSUPPORTED;
        break;
    case CM_SEMANTIC_BODY_OVERFLOW:
        constraints->solver_kind = CM_TRAIT_SOLVER_OVERFLOW;
        break;
    case CM_SEMANTIC_BODY_TYPECK_FAILURE:
        constraints->solver_kind = CM_TRAIT_SOLVER_TYPECK_FAILURE;
        break;
    case CM_SEMANTIC_BODY_INVALID:
        constraints->solver_kind = CM_TRAIT_SOLVER_INVALID;
        break;
    case CM_SEMANTIC_BODY_OK:
        constraints->solver_kind = CM_TRAIT_SOLVER_PROVEN;
        break;
    default:
        break;
    }
}

static CmSemanticBodyStatus cm_semantic_body_method_impl_callable(
    CmSemanticBodyConstraints *constraints, const CmHirItem *impl_item,
    CmHirDefId declared_callable, CmHirReceiverKind expected_receiver,
    const CmHirItem **out_callable)
{
    const CmHirItem *selected;
    size_t index;
    size_t matches;

    if (constraints == NULL || impl_item == NULL || out_callable == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_callable = NULL;
    selected = NULL;
    matches = 0u;
    for (index = 0u; index < constraints->hir->items.len; ++index) {
        const CmHirItem *candidate;

        candidate = (const CmHirItem *)cm_vec_at_const(
            &constraints->hir->items, index);
        if (candidate != NULL && candidate->kind == CM_HIR_ITEM_FUNCTION
            && cm_hir_def_id_equal(candidate->parent_definition,
                impl_item->definition)
            && cm_hir_def_id_equal(candidate->data.function_item
                .trait_item_definition, declared_callable)) {
            selected = candidate;
            ++matches;
        }
    }
    if (matches != 1u || selected == NULL
        || selected->generic_parameter_count != 0u
        || selected->predicate_scope_count != 0u
        || selected->predicate_count != 0u
        || selected->outlives_predicate_count != 0u
        || selected->data.function_item.body == CM_HIR_BODY_NONE
        || selected->data.function_item.signature.receiver
            != expected_receiver) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    *out_callable = selected;
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_check_method_callable(
    CmSemanticBodyConstraints *constraints,
    const CmParamEnvSubstitution *environment_substitution,
    CmHirExprId expression_id, const CmHirExpr *expression,
    CmSemanticCheckedCallableFacts *facts)
{
    const CmHirExpr *receiver;
    const CmHirItem *winner_trait;
    const CmHirItem *winner_declared;
    const CmHirItem *winner_impl;
    const CmHirItem *winner_callable;
    CmTypeckTypeId receiver_type;
    CmSemanticBodyStatus failure;
    CmSemanticBodyStatus blocking_failure;
    CmSemanticBodyStatus status;
    size_t trait_index;
    size_t viable_count;

    if (constraints == NULL || environment_substitution == NULL
        || expression == NULL || facts == NULL
        || expression->kind != CM_HIR_EXPR_METHOD_CALL
        || expression->data.method_call.syntax != CM_HIR_CALLABLE_DOT_METHOD
        || expression->data.method_call.method_name == CM_INTERN_ID_NONE
        || (expression->data.method_call.argument_count == 0u)
            != (expression->data.method_call.arguments == NULL)
        || (expression->data.method_call.in_scope_trait_count == 0u)
            != (expression->data.method_call.in_scope_traits == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    memset(facts, 0, sizeof(*facts));
    receiver = cm_hir_get_expr(constraints->hir,
        expression->data.method_call.receiver);
    if (receiver == NULL || receiver->owner_body != constraints->body_id) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    if (!cm_semantic_body_method_receiver_supported(constraints->hir,
            receiver)) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    status = cm_semantic_body_expression_term(constraints,
        expression->data.method_call.receiver, &receiver_type);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    winner_trait = NULL;
    winner_declared = NULL;
    winner_impl = NULL;
    winner_callable = NULL;
    viable_count = 0u;
    failure = CM_SEMANTIC_BODY_NO_SOLUTION;
    blocking_failure = CM_SEMANTIC_BODY_OK;
    for (trait_index = 0u;
         trait_index < expression->data.method_call.in_scope_trait_count;
         ++trait_index) {
        const CmHirItem *trait_item;
        const CmHirItem *declared;
        size_t item_index;
        size_t declaration_count;

        trait_item = cm_semantic_body_item(constraints->hir,
            expression->data.method_call.in_scope_traits[trait_index]);
        if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        declared = NULL;
        declaration_count = 0u;
        for (item_index = 0u; item_index < constraints->hir->items.len;
             ++item_index) {
            const CmHirItem *candidate;

            candidate = (const CmHirItem *)cm_vec_at_const(
                &constraints->hir->items, item_index);
            if (candidate != NULL
                && candidate->kind == CM_HIR_ITEM_FUNCTION
                && candidate->name
                    == expression->data.method_call.method_name
                && cm_hir_def_id_equal(candidate->parent_definition,
                    trait_item->definition)) {
                declared = candidate;
                ++declaration_count;
            }
        }
        if (declaration_count == 0u) continue;
        if (declaration_count != 1u || declared == NULL) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        if (trait_item->definition.crate_id
                != constraints->body->owner.crate_id
            || trait_item->predicate_scope_count != 0u
            || trait_item->predicate_count != 0u
            || trait_item->outlives_predicate_count != 0u
            || declared->generic_parameter_count != 0u
            || declared->predicate_scope_count != 0u
            || declared->predicate_count != 0u
            || declared->outlives_predicate_count != 0u
            || declared->data.function_item.body != CM_HIR_BODY_NONE
            || (declared->data.function_item.signature.receiver
                    != CM_HIR_RECEIVER_VALUE
                && declared->data.function_item.signature.receiver
                    != CM_HIR_RECEIVER_REF_SHARED
                && declared->data.function_item.signature.receiver
                    != CM_HIR_RECEIVER_REF_MUTABLE)
            || declared->data.function_item.signature.parameter_count
                != expression->data.method_call.argument_count + 1u) {
            blocking_failure = cm_semantic_body_stronger_method_failure(
                blocking_failure == CM_SEMANTIC_BODY_OK
                    ? CM_SEMANTIC_BODY_NO_SOLUTION : blocking_failure,
                CM_SEMANTIC_BODY_UNSUPPORTED);
            continue;
        }
        if (declared->data.function_item.signature.receiver
                == CM_HIR_RECEIVER_REF_MUTABLE
            && !cm_semantic_body_mutable_local_receiver_supported(
                constraints, receiver)) {
            blocking_failure = cm_semantic_body_stronger_method_failure(
                blocking_failure == CM_SEMANTIC_BODY_OK
                    ? CM_SEMANTIC_BODY_NO_SOLUTION : blocking_failure,
                CM_SEMANTIC_BODY_UNSUPPORTED);
            continue;
        }
        {
            CmTraitGoal goal;
            CmTraitSelectionResult selection;
            CmTypeckSnapshot probe_snapshot;
            CmTypeckStatus probe_status;
            size_t probe_type_count;
            const CmHirItem *impl_item;
            const CmHirItem *selected_callable;

            memset(&probe_snapshot, 0, sizeof(probe_snapshot));
            probe_status = cm_typeck_snapshot(constraints->typeck,
                &probe_snapshot);
            if (probe_status != CM_TYPECK_OK) {
                constraints->typeck_status = probe_status;
                return CM_SEMANTIC_BODY_TYPECK_FAILURE;
            }
            probe_type_count = cm_typeck_type_count(constraints->typeck);
            memset(&goal, 0, sizeof(goal));
            goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
            goal.data.implemented.owner = constraints->body->owner;
            goal.data.implemented.self_type = receiver_type;
            goal.data.implemented.trait_type.definition =
                trait_item->definition;
            status = cm_semantic_body_trait_query(constraints, trait_item,
                &goal.data.implemented.trait_type);
            if (status != CM_SEMANTIC_BODY_OK) {
                cm_free(goal.data.implemented.trait_type.arguments);
                (void)cm_typeck_rollback(constraints->typeck,
                    &probe_snapshot);
                failure = cm_semantic_body_stronger_method_failure(failure,
                    status);
                if (status != CM_SEMANTIC_BODY_NO_SOLUTION
                    && status != CM_SEMANTIC_BODY_NEGATIVE) {
                    blocking_failure = blocking_failure
                            == CM_SEMANTIC_BODY_OK
                        ? status
                        : cm_semantic_body_stronger_method_failure(
                            blocking_failure, status);
                }
                continue;
            }
            selection = cm_semantic_session_solve_goal(
                constraints->session, constraints->typeck,
                environment_substitution, &goal);
            probe_status = cm_typeck_rollback(constraints->typeck,
                &probe_snapshot);
            cm_free(goal.data.implemented.trait_type.arguments);
            if (probe_status != CM_TYPECK_OK
                || cm_typeck_type_count(constraints->typeck)
                    != probe_type_count) {
                constraints->typeck_status = probe_status;
                return CM_SEMANTIC_BODY_TYPECK_FAILURE;
            }
            constraints->solver_kind = selection.kind;
            constraints->typeck_status = selection.typeck_status;
            status = selection.negative_match_count != 0u
                && selection.supported_match_count == 0u
                ? CM_SEMANTIC_BODY_NEGATIVE
                : cm_semantic_solver_status(selection.kind);
            if (status != CM_SEMANTIC_BODY_OK) {
                failure = cm_semantic_body_stronger_method_failure(failure,
                    status);
                if (status != CM_SEMANTIC_BODY_NO_SOLUTION
                    && status != CM_SEMANTIC_BODY_NEGATIVE) {
                    blocking_failure = blocking_failure
                            == CM_SEMANTIC_BODY_OK
                        ? status
                        : cm_semantic_body_stronger_method_failure(
                            blocking_failure, status);
                }
                continue;
            }
            impl_item = cm_semantic_body_item(constraints->hir,
                selection.impl_definition);
            if (selection.proof_origin != CM_TRAIT_PROOF_IMPL
                || cm_hir_def_id_is_none(selection.impl_definition)
                || selection.supported_match_count != 1u
                || selection.negative_match_count != 0u
                || selection.blocking_match_count != 0u
                || impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
                || impl_item->definition.crate_id
                    != trait_item->definition.crate_id
                || impl_item->predicate_scope_count != 0u
                || impl_item->predicate_count != 0u
                || impl_item->outlives_predicate_count != 0u
                || !cm_semantic_type_only_owner(constraints->hir,
                    impl_item, impl_item->generic_parameter_count)
                || !impl_item->data.impl_item.has_trait
                || impl_item->data.impl_item.is_negative
                || impl_item->data.impl_item.trait_type.argument_count
                    != trait_item->generic_parameter_count
                || (impl_item->data.impl_item.trait_type.argument_count != 0u)
                    != (impl_item->data.impl_item.trait_type.arguments
                        != NULL)
                || !cm_hir_def_id_equal(impl_item->data.impl_item.trait_type
                    .definition, trait_item->definition)) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            status = cm_semantic_body_method_impl_callable(constraints,
                impl_item, declared->definition,
                declared->data.function_item.signature.receiver,
                &selected_callable);
            if (status != CM_SEMANTIC_BODY_OK) {
                blocking_failure = blocking_failure
                        == CM_SEMANTIC_BODY_OK
                    ? status : cm_semantic_body_stronger_method_failure(
                        blocking_failure, status);
                continue;
            }
            ++viable_count;
            if (viable_count == 1u) {
                winner_trait = trait_item;
                winner_declared = declared;
                winner_impl = impl_item;
                winner_callable = selected_callable;
            }
        }
    }
    if (viable_count > 1u) {
        cm_semantic_body_set_method_solver_kind(constraints,
            CM_SEMANTIC_BODY_AMBIGUOUS);
        return CM_SEMANTIC_BODY_AMBIGUOUS;
    }
    if (blocking_failure != CM_SEMANTIC_BODY_OK) {
        cm_semantic_body_set_method_solver_kind(constraints,
            blocking_failure);
        return blocking_failure;
    }
    if (viable_count == 0u) {
        cm_semantic_body_set_method_solver_kind(constraints, failure);
        return failure;
    }
    if (winner_trait == NULL || winner_declared == NULL
        || winner_impl == NULL || winner_callable == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    {
        const CmHirFunctionSignature *signature;
        CmTraitImplSelectionWitness witness;
        CmTraitGoal goal;
        CmTraitSelectionResult selection;
        CmTypeckInstantiation impl_instantiation;
        CmTypeckInstantiationFrame frames[2];
        CmTypeckScopedInstantiation callable_instantiation;
        CmTypeckTypeId actual_type;
        CmTypeckTypeId declared_type;
        uint32_t parameter_index;

        cm_trait_impl_selection_witness_init(&witness);
        memset(&goal, 0, sizeof(goal));
        goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
        goal.data.implemented.owner = constraints->body->owner;
        goal.data.implemented.self_type = receiver_type;
        goal.data.implemented.trait_type.definition =
            winner_trait->definition;
        status = cm_semantic_body_trait_query(constraints, winner_trait,
            &goal.data.implemented.trait_type);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        selection = cm_semantic_session_solve_goal_with_impl_witness(
            constraints->session, constraints->typeck,
            environment_substitution, &goal, &witness);
        constraints->solver_kind = selection.kind;
        constraints->typeck_status = selection.typeck_status;
        status = cm_semantic_solver_status(selection.kind);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        if (selection.proof_origin != CM_TRAIT_PROOF_IMPL
            || !cm_hir_def_id_equal(selection.impl_definition,
                winner_impl->definition)
            || selection.supported_match_count != 1u
            || selection.negative_match_count != 0u
            || selection.blocking_match_count != 0u
            || !cm_trait_impl_selection_witness_instantiation(&witness,
                constraints->typeck, &impl_instantiation)
            || !cm_hir_def_id_equal(impl_instantiation.parameter_owner,
                selection.impl_definition)) {
            status = CM_SEMANTIC_BODY_INVALID;
            goto method_cleanup;
        }
        status = cm_semantic_body_copy_generic_arguments(
            impl_instantiation.arguments, impl_instantiation.argument_count,
            &facts->enclosing_impl_argument_inputs,
            &facts->enclosing_impl_arguments);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        facts->enclosing_impl_argument_count =
            impl_instantiation.argument_count;
        status = cm_semantic_body_copy_trait_arguments(constraints,
            winner_impl, &impl_instantiation,
            &goal.data.implemented.trait_type, winner_trait->definition,
            &facts->implemented_trait_argument_inputs,
            &facts->implemented_trait_arguments,
            &facts->implemented_trait_argument_count);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        signature = &winner_callable->data.function_item.signature;
        if (signature->parameter_count
                != expression->data.method_call.argument_count + 1u
            || signature->receiver
                != winner_declared->data.function_item.signature.receiver
            || (signature->receiver != CM_HIR_RECEIVER_VALUE
                && signature->receiver != CM_HIR_RECEIVER_REF_SHARED
                && signature->receiver != CM_HIR_RECEIVER_REF_MUTABLE)) {
            status = CM_SEMANTIC_BODY_INVALID;
            goto method_cleanup;
        }
        facts->argument_count = signature->parameter_count;
        facts->parameter_count = signature->parameter_count;
        facts->argument_expressions = (CmHirExprId *)cm_alloc_zeroed(
            facts->argument_count, sizeof(CmHirExprId));
        facts->parameter_input_types = (CmTypeckTypeId *)cm_alloc_zeroed(
            facts->parameter_count, sizeof(CmTypeckTypeId));
        facts->parameter_types = (CmTypeckTypeId *)cm_alloc_zeroed(
            facts->parameter_count, sizeof(CmTypeckTypeId));
        memset(frames, 0, sizeof(frames));
        frames[0].parameter_owner = winner_callable->definition;
        frames[1].parameter_owner = winner_impl->definition;
        frames[1].arguments = facts->enclosing_impl_arguments;
        frames[1].argument_count = facts->enclosing_impl_argument_count;
        cm_typeck_scoped_instantiation_init(constraints->typeck,
            &callable_instantiation);
        callable_instantiation.frames = frames;
        callable_instantiation.frame_count = 2u;
        callable_instantiation.self_owner = winner_impl->definition;
        callable_instantiation.self_type = receiver_type;
        if (!cm_typeck_scoped_instantiation_is_valid(constraints->typeck,
                &callable_instantiation)) {
            status = CM_SEMANTIC_BODY_INVALID;
            goto method_cleanup;
        }
        facts->expression = expression_id;
        facts->syntax = CM_HIR_CALLABLE_DOT_METHOD;
        facts->requested_self_type = receiver_type;
        facts->requested_self_input_type = receiver_type;
        facts->requested_trait = winner_trait->definition;
        facts->declared_trait_callable = winner_declared->definition;
        facts->selected_impl = winner_impl->definition;
        facts->selected_callable = winner_callable->definition;
        facts->body_definition = winner_callable->definition;
        facts->enclosing_impl = winner_impl->definition;
        facts->implemented_trait = winner_trait->definition;
        facts->self_owner = winner_impl->definition;
        facts->receiver_expression = expression->data.method_call.receiver;
        facts->receiver_argument = 0u;
        ((CmHirExprId *)facts->argument_expressions)[0] =
            expression->data.method_call.receiver;
        for (parameter_index = 1u;
             parameter_index < facts->argument_count; ++parameter_index) {
            ((CmHirExprId *)facts->argument_expressions)[parameter_index] =
                expression->data.method_call.arguments[
                    parameter_index - 1u];
        }
        constraints->failed_callee = winner_callable->definition;
        status = cm_semantic_body_expression_term(constraints,
            expression_id, &actual_type);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        status = cm_semantic_body_instantiate_type_scoped(constraints,
            signature->return_type, &callable_instantiation,
            &declared_type);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        facts->return_type = declared_type;
        facts->return_input_type = declared_type;
        status = cm_semantic_body_unify_terms(constraints, actual_type,
            declared_type);
        if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        for (parameter_index = 0u;
             parameter_index < signature->parameter_count;
             ++parameter_index) {
            constraints->failed_expression =
                facts->argument_expressions[parameter_index];
            status = cm_semantic_body_expression_term(constraints,
                facts->argument_expressions[parameter_index],
                &actual_type);
            if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
            status = cm_semantic_body_instantiate_type_scoped(constraints,
                signature->parameters[parameter_index].type,
                &callable_instantiation, &declared_type);
            if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
            ((CmTypeckTypeId *)facts->parameter_input_types)[
                parameter_index] = declared_type;
            ((CmTypeckTypeId *)facts->parameter_types)[parameter_index] =
                declared_type;
            if (parameter_index == 0u
                && (signature->receiver == CM_HIR_RECEIVER_REF_SHARED
                    || signature->receiver
                        == CM_HIR_RECEIVER_REF_MUTABLE)) {
                CmTypeckTypeId resolved_type;
                const CmTypeckType *reference_type;
                CmSemanticCheckedAdjustmentFacts *adjustment;
                CmHirMutability expected_mutability;

                expected_mutability = signature->receiver
                        == CM_HIR_RECEIVER_REF_MUTABLE
                    ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE;

                constraints->typeck_status = cm_typeck_resolve(
                    constraints->typeck, declared_type, &resolved_type);
                reference_type = constraints->typeck_status == CM_TYPECK_OK
                    ? cm_typeck_get_type(constraints->typeck, resolved_type)
                    : NULL;
                if (constraints->typeck_status != CM_TYPECK_OK) {
                    status = cm_semantic_typeck_status(
                        constraints->typeck_status);
                    goto method_cleanup;
                }
                if (reference_type == NULL
                    || reference_type->kind != CM_TYPECK_TYPE_REFERENCE
                    || reference_type->data.reference_type.mutability
                        != expected_mutability
                    || reference_type->data.reference_type.region.kind
                        != CM_HIR_REGION_ERASED) {
                    status = CM_SEMANTIC_BODY_UNSUPPORTED;
                    goto method_cleanup;
                }
                status = cm_semantic_body_unify_terms(constraints,
                    actual_type,
                    reference_type->data.reference_type.pointee);
                if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
                if (constraints->checked_facts == NULL
                    || constraints->checked_facts->adjustments == NULL
                    || constraints->checked_facts->adjustment_count
                        >= constraints->expression_term_count) {
                    status = CM_SEMANTIC_BODY_INVALID;
                    goto method_cleanup;
                }
                adjustment = &((CmSemanticCheckedAdjustmentFacts *)
                    constraints->checked_facts->adjustments)[
                        constraints->checked_facts->adjustment_count++];
                memset(adjustment, 0, sizeof(*adjustment));
                adjustment->expression = facts->receiver_expression;
                adjustment->kind = signature->receiver
                        == CM_HIR_RECEIVER_REF_MUTABLE
                    ? CM_SEMANTIC_ADJUSTMENT_BORROW_MUTABLE
                    : CM_SEMANTIC_ADJUSTMENT_BORROW_SHARED;
                adjustment->source_type = actual_type;
                adjustment->target_type = declared_type;
                adjustment->selected_trait = cm_hir_def_id_none();
                adjustment->selected_method = cm_hir_def_id_none();
                adjustment->selected_impl = cm_hir_def_id_none();
            } else {
                status = cm_semantic_body_unify_terms(constraints,
                    actual_type, declared_type);
            }
            if (status != CM_SEMANTIC_BODY_OK) goto method_cleanup;
        }
        status = CM_SEMANTIC_BODY_OK;
method_cleanup:
        cm_free(goal.data.implemented.trait_type.arguments);
        cm_trait_impl_selection_witness_destroy(&witness);
        if (status != CM_SEMANTIC_BODY_OK) {
            cm_semantic_body_callable_facts_clear(facts);
            return status;
        }
    }
    cm_semantic_body_set_method_solver_kind(constraints,
        CM_SEMANTIC_BODY_OK);
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_normalize_checked_facts(
    CmSemanticBodyConstraints *constraints,
    CmSemanticCheckedBodyFacts *facts)
{
    CmProjectionNormalizeResult normalization;
    size_t call_index;
    size_t fact_index;
    uint32_t parameter_index;

#define CM_NORMALIZE_CALLABLE_SLOT(callable_, input_lvalue_, type_lvalue_, \
        kind_, index_) do { \
    CmTypeckTypeId cm_slot_input; \
    CmProjectionNormalizeTrace cm_slot_trace; \
    CmSemanticBodyWritebackStatus cm_slot_writeback; \
    int cm_slot_traced; \
    cm_slot_input = (type_lvalue_); \
    (input_lvalue_) = cm_slot_input; \
    cm_slot_traced = constraints->evidence_writeback != NULL \
        && constraints->evidence_writeback->projection_decision != NULL; \
    if (cm_slot_traced) { \
        cm_projection_normalize_trace_init(&cm_slot_trace); \
        normalization = cm_semantic_session_normalize_type_traced( \
            constraints->session, constraints->typeck, \
            constraints->substitution, cm_slot_input, \
            constraints->normalize_limits, &cm_slot_trace); \
    } else { \
        memset(&cm_slot_trace, 0, sizeof(cm_slot_trace)); \
        normalization = cm_semantic_body_normalize(constraints, \
            cm_slot_input); \
    } \
    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) { \
        if (cm_slot_traced) { \
            cm_projection_normalize_trace_destroy(&cm_slot_trace); \
        } \
        return cm_semantic_body_normalize_status(constraints, \
            &normalization); \
    } \
    (type_lvalue_) = normalization.type; \
    if (cm_slot_traced \
        && cm_projection_normalize_trace_count(&cm_slot_trace) != 0u) { \
        cm_slot_writeback = constraints->evidence_writeback \
            ->projection_decision( \
                constraints->evidence_writeback->context, \
                constraints->session, constraints->body_id, \
                (callable_)->expression, (kind_), (index_), \
                cm_slot_input, normalization.type, &cm_slot_trace); \
        cm_projection_normalize_trace_destroy(&cm_slot_trace); \
        if (cm_slot_writeback != CM_SEMANTIC_BODY_WRITEBACK_OK) { \
            return cm_slot_writeback == CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW \
                ? CM_SEMANTIC_BODY_OVERFLOW \
                : cm_slot_writeback \
                        == CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED \
                    ? CM_SEMANTIC_BODY_UNSUPPORTED \
                    : CM_SEMANTIC_BODY_INVALID; \
        } \
    } else if (cm_slot_traced) { \
        cm_projection_normalize_trace_destroy(&cm_slot_trace); \
    } \
} while (0)

#define CM_NORMALIZE_CALLABLE_ARGUMENTS(callable_, inputs_, arguments_, \
        count_, kind_) do { \
    uint32_t cm_argument_index; \
    if (((count_) == 0u) != ((inputs_) == NULL) \
        || ((count_) == 0u) != ((arguments_) == NULL)) { \
        return CM_SEMANTIC_BODY_INVALID; \
    } \
    for (cm_argument_index = 0u; cm_argument_index < (count_); \
         ++cm_argument_index) { \
        CmTypeckGenericArg *cm_input_argument; \
        CmTypeckGenericArg *cm_argument; \
        cm_input_argument = &((CmTypeckGenericArg *)(inputs_))[ \
            cm_argument_index]; \
        cm_argument = &((CmTypeckGenericArg *)(arguments_))[ \
            cm_argument_index]; \
        if (cm_input_argument->kind != cm_argument->kind) { \
            return CM_SEMANTIC_BODY_INVALID; \
        } \
        switch (cm_argument->kind) { \
        case CM_HIR_GENERIC_ARG_TYPE: \
            CM_NORMALIZE_CALLABLE_SLOT((callable_), \
                cm_input_argument->data.type, cm_argument->data.type, \
                (kind_), cm_argument_index); \
            break; \
        case CM_HIR_GENERIC_ARG_LIFETIME: \
            if (memcmp(cm_input_argument, cm_argument, \
                    sizeof(*cm_argument)) != 0) { \
                return CM_SEMANTIC_BODY_INVALID; \
            } \
            break; \
        case CM_HIR_GENERIC_ARG_CONST: \
            if (cm_input_argument->data.constant.kind \
                    != cm_argument->data.constant.kind \
                || memcmp(&cm_input_argument->data.constant.data, \
                    &cm_argument->data.constant.data, \
                    sizeof(cm_argument->data.constant.data)) != 0) { \
                return CM_SEMANTIC_BODY_INVALID; \
            } \
            CM_NORMALIZE_CALLABLE_SLOT((callable_), \
                cm_input_argument->data.constant.type, \
                cm_argument->data.constant.type, (kind_), \
                cm_argument_index); \
            break; \
        default: \
            return CM_SEMANTIC_BODY_INVALID; \
        } \
    } \
} while (0)

    if (constraints == NULL || facts == NULL
        || facts->signature_return_type == CM_TYPECK_TYPE_NONE
        || (facts->signature_parameter_count != 0u
            && facts->signature_parameter_types == NULL)
        || (facts->call_count != 0u && facts->calls == NULL)
        || (facts->adjustment_count != 0u && facts->adjustments == NULL)
        || (facts->primitive_binary_count != 0u
            && facts->primitive_binaries == NULL)
        || (facts->field_selection_count != 0u
            && facts->field_selections == NULL)) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    normalization = cm_semantic_body_normalize(constraints,
        facts->signature_return_type);
    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
        return cm_semantic_body_normalize_status(constraints,
            &normalization);
    }
    facts->signature_return_type = normalization.type;
    for (parameter_index = 0u;
         parameter_index < facts->signature_parameter_count;
         ++parameter_index) {
        CmTypeckTypeId *type;

        type = &((CmTypeckTypeId *)facts->signature_parameter_types)[
            parameter_index];
        normalization = cm_semantic_body_normalize(constraints, *type);
        if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
            return cm_semantic_body_normalize_status(constraints,
                &normalization);
        }
        *type = normalization.type;
    }
    for (call_index = 0u; call_index < facts->call_count; ++call_index) {
        CmSemanticCheckedCallFacts *call;

        call = &((CmSemanticCheckedCallFacts *)facts->calls)[call_index];
        if (call->expression == CM_HIR_EXPR_NONE
            || cm_hir_def_id_is_none(call->callee)
            || call->return_type == CM_TYPECK_TYPE_NONE
            || (call->parameter_count != 0u
                && call->parameter_types == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = call->expression;
        constraints->failed_callee = call->callee;
        normalization = cm_semantic_body_normalize(constraints,
            call->return_type);
        if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
            return cm_semantic_body_normalize_status(constraints,
                &normalization);
        }
        call->return_type = normalization.type;
        for (parameter_index = 0u;
             parameter_index < call->parameter_count; ++parameter_index) {
            CmTypeckTypeId *type;

            type = &((CmTypeckTypeId *)call->parameter_types)[
                parameter_index];
            normalization = cm_semantic_body_normalize(constraints, *type);
            if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                return cm_semantic_body_normalize_status(constraints,
                    &normalization);
            }
            *type = normalization.type;
        }
    }
    for (call_index = 0u; call_index < facts->callable_count;
         ++call_index) {
        CmSemanticCheckedCallableFacts *callable;

        callable = &((CmSemanticCheckedCallableFacts *)facts->callables)[
            call_index];
        if (callable->expression == CM_HIR_EXPR_NONE
            || cm_hir_def_id_is_none(callable->requested_trait)
            || cm_hir_def_id_is_none(callable->declared_trait_callable)
            || cm_hir_def_id_is_none(callable->selected_impl)
            || cm_hir_def_id_is_none(callable->selected_callable)
            || callable->requested_self_type == CM_TYPECK_TYPE_NONE
            || callable->requested_self_input_type == CM_TYPECK_TYPE_NONE
            || callable->return_type == CM_TYPECK_TYPE_NONE
            || callable->return_input_type == CM_TYPECK_TYPE_NONE
            || callable->argument_count != callable->parameter_count
            || (callable->argument_count != 0u
                && (callable->argument_expressions == NULL
                    || callable->parameter_input_types == NULL
                    || callable->parameter_types == NULL))) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = callable->expression;
        constraints->failed_callee = callable->selected_callable;
        CM_NORMALIZE_CALLABLE_ARGUMENTS(callable,
            callable->item_argument_inputs, callable->item_arguments,
            callable->item_argument_count,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_ITEM_ARGUMENT_TYPE);
        CM_NORMALIZE_CALLABLE_ARGUMENTS(callable,
            callable->method_argument_inputs, callable->method_arguments,
            callable->method_argument_count,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_METHOD_ARGUMENT_TYPE);
        CM_NORMALIZE_CALLABLE_ARGUMENTS(callable,
            callable->enclosing_impl_argument_inputs,
            callable->enclosing_impl_arguments,
            callable->enclosing_impl_argument_count,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_ENCLOSING_IMPL_ARGUMENT_TYPE);
        CM_NORMALIZE_CALLABLE_ARGUMENTS(callable,
            callable->implemented_trait_argument_inputs,
            callable->implemented_trait_arguments,
            callable->implemented_trait_argument_count,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_IMPLEMENTED_TRAIT_ARGUMENT_TYPE);
        CM_NORMALIZE_CALLABLE_SLOT(callable,
            callable->requested_self_input_type,
            callable->requested_self_type,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_REQUESTED_SELF_TYPE,
            0u);
        CM_NORMALIZE_CALLABLE_SLOT(callable, callable->return_input_type,
            callable->return_type,
            CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_RETURN_TYPE, 0u);
        for (parameter_index = 0u;
             parameter_index < callable->parameter_count;
             ++parameter_index) {
            CmTypeckTypeId *type;

            type = &((CmTypeckTypeId *)callable->parameter_types)[
                parameter_index];
            CM_NORMALIZE_CALLABLE_SLOT(callable,
                ((CmTypeckTypeId *)callable->parameter_input_types)[
                    parameter_index], *type,
                CM_SEMANTIC_PROJECTION_DECISION_CALLABLE_PARAMETER_TYPE,
                parameter_index);
        }
    }
    for (fact_index = 0u; fact_index < facts->adjustment_count;
         ++fact_index) {
        CmSemanticCheckedAdjustmentFacts *adjustment;
        CmTypeckTypeId *types[2];
        size_t type_index;

        adjustment = &((CmSemanticCheckedAdjustmentFacts *)
            facts->adjustments)[fact_index];
        types[0] = &adjustment->source_type;
        types[1] = &adjustment->target_type;
        constraints->failed_expression = adjustment->expression;
        for (type_index = 0u; type_index < 2u; ++type_index) {
            normalization = cm_semantic_body_normalize(constraints,
                *types[type_index]);
            if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                return cm_semantic_body_normalize_status(constraints,
                    &normalization);
            }
            *types[type_index] = normalization.type;
        }
    }
    for (fact_index = 0u; fact_index < facts->primitive_binary_count;
         ++fact_index) {
        CmSemanticCheckedPrimitiveBinaryFacts *binary;
        CmTypeckTypeId *types[3];
        size_t type_index;

        binary = &((CmSemanticCheckedPrimitiveBinaryFacts *)
            facts->primitive_binaries)[fact_index];
        types[0] = &binary->left_type;
        types[1] = &binary->right_type;
        types[2] = &binary->result_type;
        constraints->failed_expression = binary->expression;
        for (type_index = 0u; type_index < 3u; ++type_index) {
            normalization = cm_semantic_body_normalize(constraints,
                *types[type_index]);
            if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                return cm_semantic_body_normalize_status(constraints,
                    &normalization);
            }
            *types[type_index] = normalization.type;
        }
    }
    for (fact_index = 0u; fact_index < facts->field_selection_count;
         ++fact_index) {
        CmSemanticCheckedFieldSelectionFacts *field;
        CmTypeckTypeId *types[2];
        size_t type_index;

        field = &((CmSemanticCheckedFieldSelectionFacts *)
            facts->field_selections)[fact_index];
        types[0] = &field->base_type;
        types[1] = &field->field_type;
        constraints->failed_expression = field->expression;
        for (type_index = 0u; type_index < 2u; ++type_index) {
            normalization = cm_semantic_body_normalize(constraints,
                *types[type_index]);
            if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                return cm_semantic_body_normalize_status(constraints,
                    &normalization);
            }
            *types[type_index] = normalization.type;
        }
    }
    return CM_SEMANTIC_BODY_OK;
#undef CM_NORMALIZE_CALLABLE_SLOT
#undef CM_NORMALIZE_CALLABLE_ARGUMENTS
}

static CmSemanticBodyStatus cm_semantic_body_unify_owner_types(
    CmSemanticBodyConstraints *constraints, CmHirTypeId left,
    CmHirTypeId right)
{
    CmTypeckTypeId left_type;
    CmTypeckTypeId right_type;
    CmSemanticBodyStatus semantic_status;

    semantic_status = cm_semantic_body_instantiate_owner_type(constraints,
        left, &left_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    semantic_status = cm_semantic_body_instantiate_owner_type(constraints,
        right, &right_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    return cm_semantic_body_unify_terms(constraints, left_type, right_type);
}

static CmSemanticBodyStatus cm_semantic_body_expression_term(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression,
    CmTypeckTypeId *out_type)
{
    if (constraints == NULL || out_type == NULL
        || expression == CM_HIR_EXPR_NONE
        || (size_t)expression > constraints->expression_term_count
        || cm_hir_get_expr(constraints->hir, expression) == NULL
        || cm_hir_get_expr(constraints->hir, expression)->owner_body
            != constraints->body_id
        || constraints->expression_terms[(size_t)expression - 1u]
            == CM_TYPECK_TYPE_NONE) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    *out_type = constraints->expression_terms[(size_t)expression - 1u];
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_unify_expression_type(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression,
    CmHirTypeId hir_type, const CmTypeckInstantiation *instantiation)
{
    CmTypeckTypeId expression_type;
    CmTypeckTypeId declared_type;
    CmSemanticBodyStatus semantic_status;

    semantic_status = cm_semantic_body_expression_term(constraints,
        expression, &expression_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    semantic_status = cm_semantic_body_instantiate_type(constraints,
        hir_type, instantiation, &declared_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    return cm_semantic_body_unify_terms(constraints, expression_type,
        declared_type);
}

static CmSemanticBodyStatus cm_semantic_body_unify_expression_owner_type(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression,
    CmHirTypeId hir_type)
{
    CmTypeckTypeId expression_type;
    CmTypeckTypeId declared_type;
    CmSemanticBodyStatus semantic_status;

    semantic_status = cm_semantic_body_expression_term(constraints,
        expression, &expression_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    semantic_status = cm_semantic_body_instantiate_owner_type(constraints,
        hir_type, &declared_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    return cm_semantic_body_unify_terms(constraints, expression_type,
        declared_type);
}

static CmSemanticBodyStatus cm_semantic_body_unify_expressions(
    CmSemanticBodyConstraints *constraints, CmHirExprId left,
    CmHirExprId right)
{
    CmTypeckTypeId left_type;
    CmTypeckTypeId right_type;
    CmSemanticBodyStatus semantic_status;

    semantic_status = cm_semantic_body_expression_term(constraints, left,
        &left_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    semantic_status = cm_semantic_body_expression_term(constraints, right,
        &right_type);
    if (semantic_status != CM_SEMANTIC_BODY_OK) return semantic_status;
    return cm_semantic_body_unify_terms(constraints, left_type, right_type);
}

static int cm_semantic_body_integer_kind(const CmHirContext *hir,
    CmHirTypeId type_id, CmHirIntType kind)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == kind;
}

static int cm_semantic_body_bool_type(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_BOOL_KIND;
}

static CmSemanticBodyStatus cm_semantic_body_constrain_expression(
    CmSemanticBodyConstraints *constraints, CmHirExprId expression_id,
    uint32_t visible_local_count, size_t depth)
{
    const CmHirExpr *expression;
    CmSemanticBodyStatus status;
    CmTypeckTypeId *expression_term;
    uint32_t index;

    if (constraints == NULL || expression_id == CM_HIR_EXPR_NONE
        || depth >= constraints->hir->expressions.len) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    expression = cm_hir_get_expr(constraints->hir, expression_id);
    if (expression == NULL
        || expression->owner_body != constraints->body_id) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    constraints->failed_expression = expression_id;
    if ((size_t)expression_id > constraints->expression_term_count) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    expression_term = &constraints->expression_terms[
        (size_t)expression_id - 1u];
    if (*expression_term == CM_TYPECK_TYPE_NONE) {
        status = cm_semantic_body_instantiate_owner_type(constraints,
            expression->type, expression_term);
        if (status != CM_SEMANTIC_BODY_OK) return status;
    }

    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
        return cm_hir_get_type(constraints->hir, expression->type)->kind
                == CM_HIR_TYPE_INTEGER_KIND
            ? CM_SEMANTIC_BODY_OK : CM_SEMANTIC_BODY_INVALID;
    case CM_HIR_EXPR_LOCAL:
        if (expression->data.local.local_index >= visible_local_count
            || expression->data.local.local_index
                >= constraints->body->local_count) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        return cm_semantic_body_unify_expression_owner_type(constraints,
            expression_id,
            constraints->body->locals[expression->data.local.local_index]
                .type);
    case CM_HIR_EXPR_BLOCK:
    {
        uint32_t nested_visible;

        if (expression->data.block.tail_expression == CM_HIR_EXPR_NONE
            || (expression->data.block.statement_count == 0u)
                != (expression->data.block.statements == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        nested_visible = visible_local_count;
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;
            uint32_t local_index;

            statement = &expression->data.block.statements[index];
            local_index = statement->data.let_statement.local_index;
            if (statement->kind != CM_HIR_STATEMENT_LET
                || local_index != nested_visible
                || local_index >= constraints->body->local_count
                || constraints->body->locals[local_index].parameter_index
                    != CM_HIR_PARAMETER_INDEX_NONE
                || constraints->defined_locals[local_index] != 0u) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            status = cm_semantic_body_constrain_expression(constraints,
                statement->data.let_statement.initializer,
                nested_visible, depth + 1u);
            if (status != CM_SEMANTIC_BODY_OK) return status;
            constraints->failed_expression =
                statement->data.let_statement.initializer;
            status = cm_semantic_body_unify_expression_owner_type(constraints,
                statement->data.let_statement.initializer,
                constraints->body->locals[local_index].type);
            if (status != CM_SEMANTIC_BODY_OK) return status;
            constraints->defined_locals[local_index] = 1u;
            ++nested_visible;
        }
        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.block.tail_expression, nested_visible,
            depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        constraints->failed_expression = expression_id;
        return cm_semantic_body_unify_expressions(constraints,
            expression_id, expression->data.block.tail_expression);
    }
    case CM_HIR_EXPR_CALL:
        if ((expression->data.call.argument_count == 0u)
                != (expression->data.call.arguments == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            status = cm_semantic_body_constrain_expression(constraints,
                expression->data.call.arguments[index], visible_local_count,
                depth + 1u);
            if (status != CM_SEMANTIC_BODY_OK) return status;
        }
        return CM_SEMANTIC_BODY_OK;
    case CM_HIR_EXPR_METHOD_CALL:
        if ((expression->data.method_call.argument_count == 0u)
                != (expression->data.method_call.arguments == NULL)
            || expression->data.method_call.receiver
                == CM_HIR_EXPR_NONE) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.method_call.receiver, visible_local_count,
            depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            status = cm_semantic_body_constrain_expression(constraints,
                expression->data.method_call.arguments[index],
                visible_local_count, depth + 1u);
            if (status != CM_SEMANTIC_BODY_OK) return status;
        }
        return CM_SEMANTIC_BODY_OK;
    case CM_HIR_EXPR_QUALIFIED_CALL:
        if ((expression->data.qualified_call.argument_count == 0u)
                != (expression->data.qualified_call.arguments == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        for (index = 0u;
             index < expression->data.qualified_call.argument_count;
             ++index) {
            status = cm_semantic_body_constrain_expression(constraints,
                expression->data.qualified_call.arguments[index],
                visible_local_count, depth + 1u);
            if (status != CM_SEMANTIC_BODY_OK) return status;
        }
        return CM_SEMANTIC_BODY_OK;
    case CM_HIR_EXPR_BINARY:
    {
        const CmHirExpr *left;
        const CmHirExpr *right;
        int arithmetic;
        int comparison;
        int operands_u32;
        int operands_usize;

        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.binary.left, visible_local_count, depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.binary.right, visible_local_count, depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        left = cm_hir_get_expr(constraints->hir,
            expression->data.binary.left);
        right = cm_hir_get_expr(constraints->hir,
            expression->data.binary.right);
        if (left == NULL || right == NULL) return CM_SEMANTIC_BODY_INVALID;
        arithmetic = expression->data.binary.operator_kind
                == CM_HIR_BINARY_ADD
            || expression->data.binary.operator_kind
                == CM_HIR_BINARY_SUBTRACT;
        comparison = expression->data.binary.operator_kind
                == CM_HIR_BINARY_EQUAL
            || expression->data.binary.operator_kind
                == CM_HIR_BINARY_LESS;
        operands_u32 = cm_semantic_body_integer_kind(constraints->hir,
                left->type, CM_HIR_INT_U32)
            && cm_semantic_body_integer_kind(constraints->hir,
                right->type, CM_HIR_INT_U32);
        operands_usize = cm_semantic_body_integer_kind(constraints->hir,
                left->type, CM_HIR_INT_USIZE)
            && cm_semantic_body_integer_kind(constraints->hir,
                right->type, CM_HIR_INT_USIZE);
        if ((!arithmetic && !comparison)
            || (expression->data.binary.operator_kind
                    == CM_HIR_BINARY_EQUAL && !operands_u32)
            || (expression->data.binary.operator_kind
                    == CM_HIR_BINARY_LESS && !operands_usize)
            || (arithmetic && !operands_u32 && !operands_usize)
            || (comparison
                && !cm_semantic_body_bool_type(constraints->hir,
                    expression->type))) {
            constraints->failed_expression = expression_id;
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = expression_id;
        status = cm_semantic_body_unify_expressions(constraints,
            expression->data.binary.left, expression->data.binary.right);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        if (arithmetic) {
            status = cm_semantic_body_unify_expressions(constraints,
                expression_id, expression->data.binary.left);
        }
        if (status == CM_SEMANTIC_BODY_OK) {
            CmSemanticCheckedPrimitiveBinaryFacts *fact;

            if (constraints->checked_facts == NULL
                || constraints->checked_facts->primitive_binaries == NULL
                || constraints->checked_facts->primitive_binary_count
                    >= constraints->expression_term_count) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            fact = &((CmSemanticCheckedPrimitiveBinaryFacts *)
                constraints->checked_facts->primitive_binaries)[
                    constraints->checked_facts->primitive_binary_count++];
            fact->expression = expression_id;
            fact->operator_kind = expression->data.binary.operator_kind;
            fact->left_expression = expression->data.binary.left;
            fact->right_expression = expression->data.binary.right;
            status = cm_semantic_body_expression_term(constraints,
                fact->left_expression, &fact->left_type);
            if (status == CM_SEMANTIC_BODY_OK) {
                status = cm_semantic_body_expression_term(constraints,
                    fact->right_expression, &fact->right_type);
            }
            if (status == CM_SEMANTIC_BODY_OK) {
                status = cm_semantic_body_expression_term(constraints,
                    expression_id, &fact->result_type);
            }
        }
        return status;
    }
    case CM_HIR_EXPR_AGGREGATE:
    {
        const CmHirType *aggregate_type;
        const CmHirItem *aggregate;
        const CmHirModule *aggregate_module;
        CmTypeckInstantiation aggregate_instantiation;

        aggregate_type = cm_hir_get_type(constraints->hir,
            expression->type);
        aggregate = cm_semantic_body_item(constraints->hir,
            expression->data.aggregate.definition);
        aggregate_module = aggregate == NULL ? NULL
            : cm_hir_get_module(constraints->hir, aggregate->owner_module);
        if (aggregate_type == NULL
            || aggregate_type->kind != CM_HIR_TYPE_ADT_KIND
            || aggregate_type->data.named_type.argument_count != 0u
            || aggregate_type->data.named_type.arguments != NULL
            || !cm_hir_def_id_equal(
                aggregate_type->data.named_type.definition,
                expression->data.aggregate.definition)
            || aggregate == NULL || aggregate->kind != CM_HIR_ITEM_STRUCT
            || expression->data.aggregate.definition.crate_id
                != constraints->body->owner.crate_id
            || aggregate_module == NULL
            || aggregate_module->crate_id
                != expression->data.aggregate.definition.crate_id
            || !cm_hir_def_id_is_none(aggregate->parent_definition)
            || aggregate->generic_parameter_count != 0u
            || aggregate->data.aggregate_item.form
                != CM_HIR_AGGREGATE_NAMED
            || aggregate->data.aggregate_item.field_count
                != expression->data.aggregate.field_count
            || (expression->data.aggregate.field_count == 0u)
                != (expression->data.aggregate.fields == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        cm_typeck_instantiation_init(constraints->typeck,
            &aggregate_instantiation);
        aggregate_instantiation.parameter_owner = aggregate->definition;
        if (!cm_typeck_instantiation_is_valid(constraints->typeck,
                &aggregate_instantiation)) {
            return CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        }
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            const CmHirAggregateFieldValue *field;
            uint32_t prior;

            field = &expression->data.aggregate.fields[index];
            if (field->field_index
                >= aggregate->data.aggregate_item.field_count) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            for (prior = 0u; prior < index; ++prior) {
                if (expression->data.aggregate.fields[prior].field_index
                        == field->field_index) {
                    return CM_SEMANTIC_BODY_INVALID;
                }
            }
            status = cm_semantic_body_constrain_expression(constraints,
                field->value, visible_local_count, depth + 1u);
            if (status != CM_SEMANTIC_BODY_OK) return status;
            constraints->failed_expression = expression_id;
            status = cm_semantic_body_unify_expression_type(constraints,
                field->value,
                aggregate->data.aggregate_item.fields[field->field_index]
                    .type,
                &aggregate_instantiation);
            if (status != CM_SEMANTIC_BODY_OK) return status;
        }
        return CM_SEMANTIC_BODY_OK;
    }
    case CM_HIR_EXPR_FIELD:
    {
        const CmHirExpr *base;
        const CmHirType *base_type;
        const CmHirItem *aggregate;
        const CmHirModule *aggregate_module;
        CmTypeckInstantiation aggregate_instantiation;

        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.field.base, visible_local_count, depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        base = cm_hir_get_expr(constraints->hir,
            expression->data.field.base);
        base_type = base == NULL ? NULL
            : cm_hir_get_type(constraints->hir, base->type);
        aggregate = cm_semantic_body_item(constraints->hir,
            expression->data.field.definition);
        aggregate_module = aggregate == NULL ? NULL
            : cm_hir_get_module(constraints->hir, aggregate->owner_module);
        if (base_type == NULL || base_type->kind != CM_HIR_TYPE_ADT_KIND
            || base_type->data.named_type.argument_count != 0u
            || base_type->data.named_type.arguments != NULL
            || !cm_hir_def_id_equal(base_type->data.named_type.definition,
                expression->data.field.definition)
            || aggregate == NULL || aggregate->kind != CM_HIR_ITEM_STRUCT
            || expression->data.field.definition.crate_id
                != constraints->body->owner.crate_id
            || aggregate_module == NULL
            || aggregate_module->crate_id
                != expression->data.field.definition.crate_id
            || !cm_hir_def_id_is_none(aggregate->parent_definition)
            || aggregate->generic_parameter_count != 0u
            || aggregate->data.aggregate_item.form
                != CM_HIR_AGGREGATE_NAMED
            || expression->data.field.field_index
                >= aggregate->data.aggregate_item.field_count) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        cm_typeck_instantiation_init(constraints->typeck,
            &aggregate_instantiation);
        aggregate_instantiation.parameter_owner = aggregate->definition;
        if (!cm_typeck_instantiation_is_valid(constraints->typeck,
                &aggregate_instantiation)) {
            return CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        }
        constraints->failed_expression = expression_id;
        status = cm_semantic_body_unify_expression_type(constraints,
            expression_id,
            aggregate->data.aggregate_item
                .fields[expression->data.field.field_index].type,
            &aggregate_instantiation);
        if (status == CM_SEMANTIC_BODY_OK) {
            CmSemanticCheckedFieldSelectionFacts *fact;

            if (constraints->checked_facts == NULL
                || constraints->checked_facts->field_selections == NULL
                || constraints->checked_facts->field_selection_count
                    >= constraints->expression_term_count) {
                return CM_SEMANTIC_BODY_INVALID;
            }
            fact = &((CmSemanticCheckedFieldSelectionFacts *)
                constraints->checked_facts->field_selections)[
                    constraints->checked_facts->field_selection_count++];
            fact->expression = expression_id;
            fact->base_expression = expression->data.field.base;
            fact->aggregate_definition = expression->data.field.definition;
            fact->field_index = expression->data.field.field_index;
            status = cm_semantic_body_expression_term(constraints,
                fact->base_expression, &fact->base_type);
            if (status == CM_SEMANTIC_BODY_OK) {
                status = cm_semantic_body_expression_term(constraints,
                    expression_id, &fact->field_type);
            }
        }
        return status;
    }
    case CM_HIR_EXPR_IF:
    {
        const CmHirExpr *condition;
        const CmHirExpr *then_expression;
        const CmHirExpr *else_expression;

        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.if_expr.condition, visible_local_count,
            depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.if_expr.then_expression, visible_local_count,
            depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        status = cm_semantic_body_constrain_expression(constraints,
            expression->data.if_expr.else_expression, visible_local_count,
            depth + 1u);
        if (status != CM_SEMANTIC_BODY_OK) return status;
        condition = cm_hir_get_expr(constraints->hir,
            expression->data.if_expr.condition);
        then_expression = cm_hir_get_expr(constraints->hir,
            expression->data.if_expr.then_expression);
        else_expression = cm_hir_get_expr(constraints->hir,
            expression->data.if_expr.else_expression);
        if (condition == NULL || then_expression == NULL
            || else_expression == NULL
            || !cm_semantic_body_bool_type(constraints->hir,
                condition->type)
            || condition->kind != CM_HIR_EXPR_BINARY
            || then_expression->kind != CM_HIR_EXPR_BLOCK
            || else_expression->kind != CM_HIR_EXPR_BLOCK
            || then_expression->data.block.statement_count != 0u
            || then_expression->data.block.statements != NULL
            || else_expression->data.block.statement_count != 0u
            || else_expression->data.block.statements != NULL
            || (!cm_semantic_body_integer_kind(constraints->hir,
                    expression->type, CM_HIR_INT_U32)
                && !cm_semantic_body_integer_kind(constraints->hir,
                    expression->type, CM_HIR_INT_USIZE))
            || condition->data.binary.operator_kind
                != (cm_semantic_body_integer_kind(constraints->hir,
                        expression->type, CM_HIR_INT_U32)
                    ? CM_HIR_BINARY_EQUAL : CM_HIR_BINARY_LESS)) {
            constraints->failed_expression = expression_id;
            return CM_SEMANTIC_BODY_INVALID;
        }
        constraints->failed_expression = expression_id;
        status = cm_semantic_body_unify_expressions(constraints,
            expression->data.if_expr.then_expression,
            expression->data.if_expr.else_expression);
        if (status == CM_SEMANTIC_BODY_OK) {
            status = cm_semantic_body_unify_expressions(constraints,
                expression_id, expression->data.if_expr.then_expression);
        }
        return status;
    }
    case CM_HIR_EXPR_BORROW_SHARED:
    case CM_HIR_EXPR_DEREFERENCE:
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
    case CM_HIR_EXPR_CLOSURE:
        /*
         * The HIR model can represent these nodes, but body admission must
         * not accept them until it also publishes the exact adjustment and
         * place-use evidence that MIR lowering can replay.
         */
        return CM_SEMANTIC_BODY_INVALID;
    }
    return CM_SEMANTIC_BODY_INVALID;
}

static CmSemanticBodyStatus cm_semantic_body_constrain(
    CmSemanticBodyConstraints *constraints, const CmHirItem *owner_item)
{
    const CmHirExpr *root;
    CmSemanticBodyStatus status;
    uint32_t initial_local_count;
    uint32_t index;
    size_t bitmap_size;

    if (constraints == NULL || constraints->session == NULL
        || constraints->typeck == NULL || constraints->substitution == NULL
        || constraints->hir == NULL || constraints->body == NULL
        || owner_item == NULL || constraints->owner_instantiation == NULL
        || constraints->expression_terms == NULL
        || constraints->expression_term_count
            != constraints->hir->expressions.len
        || constraints->deferred_equalities == NULL) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    if ((constraints->body->local_count == 0u)
            != (constraints->body->locals == NULL)
        || constraints->body->parameter_count
            != owner_item->data.function_item.signature.parameter_count) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    if (!cm_size_mul((size_t)(constraints->body->local_count == 0u
                ? 1u : constraints->body->local_count),
            sizeof(*constraints->defined_locals),
            &bitmap_size)) {
        return CM_SEMANTIC_BODY_OVERFLOW;
    }
    constraints->defined_locals = (unsigned char *)cm_alloc_zeroed(
        1u, bitmap_size);
    initial_local_count = 0u;
    for (index = 0u;
         index < owner_item->data.function_item.signature.parameter_count;
         ++index) {
        const CmHirFunctionParameter *parameter;

        parameter = &owner_item->data.function_item.signature
            .parameters[index];
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            const CmHirLocal *local;

            if (initial_local_count >= constraints->body->local_count) {
                status = CM_SEMANTIC_BODY_INVALID;
                goto finish;
            }
            local = &constraints->body->locals[initial_local_count];
            if (local->parameter_index != index
                || local->parameter_binding_index != 0u
                || local->name != parameter->name) {
                status = CM_SEMANTIC_BODY_INVALID;
                goto finish;
            }
            status = cm_semantic_body_unify_owner_types(constraints,
                local->type, parameter->type);
            if (status != CM_SEMANTIC_BODY_OK) goto finish;
            initial_local_count += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_TUPLE_PATTERN) {
            const CmHirType *tuple_type;
            uint32_t binding_index;

            tuple_type = cm_hir_get_type(constraints->hir,
                parameter->type);
            if (tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count
                    != CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                status = CM_SEMANTIC_BODY_INVALID;
                goto finish;
            }
            for (binding_index = 0u;
                 binding_index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
                 ++binding_index) {
                const CmHirLocal *local;
                const CmHirTupleParameterBinding *binding;

                if (initial_local_count >= constraints->body->local_count) {
                    status = CM_SEMANTIC_BODY_INVALID;
                    goto finish;
                }
                local = &constraints->body->locals[initial_local_count];
                binding = &parameter->tuple_bindings[binding_index];
                if (local->parameter_index != index
                    || local->parameter_binding_index != binding_index
                    || local->name != binding->name
                    || local->mutability != CM_HIR_IMMUTABLE
                    || local->span.source != binding->span.source
                    || local->span.start != binding->span.start
                    || local->span.end != binding->span.end) {
                    status = CM_SEMANTIC_BODY_INVALID;
                    goto finish;
                }
                status = cm_semantic_body_unify_owner_types(constraints,
                    local->type,
                    tuple_type->data.tuple_type.elements[binding_index]);
                if (status != CM_SEMANTIC_BODY_OK) goto finish;
                initial_local_count += 1u;
            }
            continue;
        }
        status = CM_SEMANTIC_BODY_INVALID;
        goto finish;
    }
    for (index = initial_local_count;
         index < constraints->body->local_count; ++index) {
        if (constraints->body->locals[index].parameter_index
                != CM_HIR_PARAMETER_INDEX_NONE
            || constraints->body->locals[index].parameter_binding_index
                != 0u) {
            status = CM_SEMANTIC_BODY_INVALID;
            goto finish;
        }
    }
    root = cm_hir_get_expr(constraints->hir,
        constraints->body->root_expression);
    if (root == NULL || root->owner_body != constraints->body_id) {
        status = CM_SEMANTIC_BODY_INVALID;
        goto finish;
    }
    status = cm_semantic_body_constrain_expression(constraints,
        constraints->body->root_expression, initial_local_count, 0u);
    if (status == CM_SEMANTIC_BODY_OK) {
        for (index = initial_local_count;
             index < constraints->body->local_count;
             ++index) {
            if (constraints->defined_locals[index] == 0u) {
                constraints->failed_expression =
                    constraints->body->root_expression;
                status = CM_SEMANTIC_BODY_INVALID;
                break;
            }
        }
    }
    if (status == CM_SEMANTIC_BODY_OK) {
        constraints->failed_expression = constraints->body->root_expression;
        status = cm_semantic_body_unify_expression_owner_type(constraints,
            constraints->body->root_expression,
            constraints->body->expected_type);
    }
finish:
    cm_free(constraints->defined_locals);
    constraints->defined_locals = NULL;
    return status;
}

static CmSemanticBodyStatus cm_semantic_body_walk(
    const CmHirContext *hir, CmHirBodyId body, CmHirExprId id,
    unsigned char *seen, CmHirExprId *calls, size_t *count)
{
    const CmHirExpr *e;
    uint32_t i;
    if (id == CM_HIR_EXPR_NONE || (size_t)id > hir->expressions.len)
        return CM_SEMANTIC_BODY_INVALID;
    e = cm_hir_get_expr(hir, id);
    if (e == NULL || e->owner_body != body || cm_hir_get_type(hir, e->type) == NULL)
        return CM_SEMANTIC_BODY_INVALID;
    if (seen[(size_t)id - 1u] == 1u) return CM_SEMANTIC_BODY_INVALID;
    if (seen[(size_t)id - 1u] == 2u) return CM_SEMANTIC_BODY_OK;
    seen[(size_t)id - 1u] = 1u;
    switch (e->kind) {
    case CM_HIR_EXPR_INTEGER: case CM_HIR_EXPR_LOCAL: break;
    case CM_HIR_EXPR_BLOCK:
        if (e->data.block.tail_expression == CM_HIR_EXPR_NONE
            || (e->data.block.statement_count != 0u
                && e->data.block.statements == NULL)) return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.block.statement_count; ++i) {
            if (e->data.block.statements[i].kind != CM_HIR_STATEMENT_LET
                || cm_semantic_body_walk(hir, body,
                    e->data.block.statements[i].data.let_statement.initializer,
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        }
        if (cm_semantic_body_walk(hir, body, e->data.block.tail_expression,
                seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_CALL:
        if (e->data.call.argument_count != 0u && e->data.call.arguments == NULL)
            return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.call.argument_count; ++i)
            if (cm_semantic_body_walk(hir, body, e->data.call.arguments[i],
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        if (*count >= hir->expressions.len) return CM_SEMANTIC_BODY_OVERFLOW;
        calls[(*count)++] = id;
        break;
    case CM_HIR_EXPR_METHOD_CALL:
        if (e->data.method_call.receiver == CM_HIR_EXPR_NONE
            || (e->data.method_call.argument_count != 0u
                && e->data.method_call.arguments == NULL)) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        if (cm_semantic_body_walk(hir, body,
                e->data.method_call.receiver, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        for (i = 0u; i < e->data.method_call.argument_count; ++i) {
            if (cm_semantic_body_walk(hir, body,
                    e->data.method_call.arguments[i], seen, calls, count)
                    != CM_SEMANTIC_BODY_OK) {
                return CM_SEMANTIC_BODY_INVALID;
            }
        }
        if (*count >= hir->expressions.len) {
            return CM_SEMANTIC_BODY_OVERFLOW;
        }
        calls[(*count)++] = id;
        break;
    case CM_HIR_EXPR_QUALIFIED_CALL:
        if (e->data.qualified_call.argument_count != 0u
            && e->data.qualified_call.arguments == NULL) {
            return CM_SEMANTIC_BODY_INVALID;
        }
        for (i = 0u; i < e->data.qualified_call.argument_count; ++i) {
            if (cm_semantic_body_walk(hir, body,
                    e->data.qualified_call.arguments[i], seen, calls,
                    count) != CM_SEMANTIC_BODY_OK) {
                return CM_SEMANTIC_BODY_INVALID;
            }
        }
        if (*count >= hir->expressions.len) return CM_SEMANTIC_BODY_OVERFLOW;
        calls[(*count)++] = id;
        break;
    case CM_HIR_EXPR_BINARY:
        if (cm_semantic_body_walk(hir, body, e->data.binary.left, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.binary.right, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_AGGREGATE:
        if (e->data.aggregate.field_count != 0u && e->data.aggregate.fields == NULL)
            return CM_SEMANTIC_BODY_INVALID;
        for (i = 0u; i < e->data.aggregate.field_count; ++i)
            if (cm_semantic_body_walk(hir, body, e->data.aggregate.fields[i].value,
                    seen, calls, count) != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_FIELD:
        if (cm_semantic_body_walk(hir, body, e->data.field.base, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_IF:
        if (cm_semantic_body_walk(hir, body, e->data.if_expr.condition, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.if_expr.then_expression, seen, calls, count)
                != CM_SEMANTIC_BODY_OK
            || cm_semantic_body_walk(hir, body, e->data.if_expr.else_expression, seen, calls, count)
                != CM_SEMANTIC_BODY_OK) return CM_SEMANTIC_BODY_INVALID;
        break;
    case CM_HIR_EXPR_BORROW_SHARED:
    case CM_HIR_EXPR_DEREFERENCE:
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
    case CM_HIR_EXPR_CLOSURE:
        return CM_SEMANTIC_BODY_INVALID;
    default: return CM_SEMANTIC_BODY_INVALID;
    }
    seen[(size_t)id - 1u] = 2u;
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_collect_calls(
    const CmHirContext *hir, CmHirBodyId body, CmHirExprId root,
    CmHirExprId **out_calls, size_t *out_count)
{
    unsigned char *seen;
    CmHirExprId *calls;
    CmSemanticBodyStatus status;
    if (hir == NULL || out_calls == NULL || out_count == NULL
        || root == CM_HIR_EXPR_NONE || hir->expressions.len == 0u)
        return CM_SEMANTIC_BODY_INVALID;
    seen = (unsigned char *)cm_alloc_zeroed(hir->expressions.len, sizeof(unsigned char));
    calls = (CmHirExprId *)cm_alloc_zeroed(hir->expressions.len, sizeof(CmHirExprId));
    *out_count = 0u;
    status = cm_semantic_body_walk(hir, body, root, seen, calls, out_count);
    cm_free(seen);
    if (status != CM_SEMANTIC_BODY_OK) { cm_free(calls); return status; }
    *out_calls = calls;
    return status;
}

static int cm_semantic_body_trait_default_scalar(
    const CmHirContext *hir, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && (type->data.integer_type.kind == CM_HIR_INT_I32
            || type->data.integer_type.kind == CM_HIR_INT_U32
            || type->data.integer_type.kind == CM_HIR_INT_USIZE);
}

static CmSemanticBodyStatus cm_semantic_body_trait_default_mark(
    const CmHirContext *hir, CmHirBodyId body_id,
    CmHirExprId expression_id, unsigned char *seen, size_t depth,
    const CmHirExpr **out_expression)
{
    const CmHirExpr *expression;

    if (hir == NULL || seen == NULL || out_expression == NULL
        || expression_id == CM_HIR_EXPR_NONE
        || (size_t)expression_id > hir->expressions.len
        || depth >= hir->expressions.len) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    expression = cm_hir_get_expr(hir, expression_id);
    if (expression == NULL || expression->owner_body != body_id
        || cm_hir_get_type(hir, expression->type) == NULL
        || seen[(size_t)expression_id - 1u] != 0u) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    seen[(size_t)expression_id - 1u] = 1u;
    *out_expression = expression;
    return CM_SEMANTIC_BODY_OK;
}

static CmSemanticBodyStatus cm_semantic_body_trait_default_value(
    const CmHirContext *hir, CmHirBodyId body_id,
    CmHirExprId expression_id, unsigned char *seen, size_t depth)
{
    const CmHirExpr *expression;
    CmSemanticBodyStatus status;

    status = cm_semantic_body_trait_default_mark(hir, body_id,
        expression_id, seen, depth, &expression);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    if (!cm_semantic_body_trait_default_scalar(hir, expression->type)) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
    case CM_HIR_EXPR_LOCAL:
        return CM_SEMANTIC_BODY_OK;
    case CM_HIR_EXPR_BINARY:
        if ((expression->data.binary.operator_kind != CM_HIR_BINARY_ADD
                && expression->data.binary.operator_kind
                    != CM_HIR_BINARY_SUBTRACT)
            || (!cm_semantic_body_integer_kind(hir, expression->type,
                    CM_HIR_INT_U32)
                && !cm_semantic_body_integer_kind(hir, expression->type,
                    CM_HIR_INT_USIZE))
            || expression->data.binary.left >= expression_id
            || expression->data.binary.right >= expression_id) {
            return CM_SEMANTIC_BODY_UNSUPPORTED;
        }
        status = cm_semantic_body_trait_default_value(hir, body_id,
            expression->data.binary.left, seen, depth + 1u);
        return status != CM_SEMANTIC_BODY_OK ? status
            : cm_semantic_body_trait_default_value(hir, body_id,
                expression->data.binary.right, seen, depth + 1u);
    default:
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
}

static CmSemanticBodyStatus cm_semantic_body_trait_default_branch(
    const CmHirContext *hir, CmHirBodyId body_id,
    CmHirExprId expression_id, unsigned char *seen, size_t depth)
{
    const CmHirExpr *expression;
    CmSemanticBodyStatus status;

    status = cm_semantic_body_trait_default_mark(hir, body_id,
        expression_id, seen, depth, &expression);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    if (expression->kind != CM_HIR_EXPR_BLOCK
        || !cm_semantic_body_trait_default_scalar(hir, expression->type)
        || expression->data.block.statement_count != 0u
        || expression->data.block.statements != NULL
        || expression->data.block.tail_expression == CM_HIR_EXPR_NONE
        || expression->data.block.tail_expression >= expression_id) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    return cm_semantic_body_trait_default_value(hir, body_id,
        expression->data.block.tail_expression, seen, depth + 1u);
}

static CmSemanticBodyStatus cm_semantic_body_trait_default_if(
    const CmHirContext *hir, CmHirBodyId body_id,
    CmHirExprId expression_id, unsigned char *seen, size_t depth)
{
    const CmHirExpr *expression;
    const CmHirExpr *condition;
    const CmHirType *result_type;
    CmSemanticBodyStatus status;

    status = cm_semantic_body_trait_default_mark(hir, body_id,
        expression_id, seen, depth, &expression);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    result_type = cm_hir_get_type(hir, expression->type);
    if (expression->kind != CM_HIR_EXPR_IF || result_type == NULL
        || result_type->kind != CM_HIR_TYPE_INTEGER_KIND
        || (result_type->data.integer_type.kind != CM_HIR_INT_U32
            && result_type->data.integer_type.kind != CM_HIR_INT_USIZE)
        || expression->data.if_expr.condition >= expression_id
        || expression->data.if_expr.then_expression >= expression_id
        || expression->data.if_expr.else_expression >= expression_id) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    status = cm_semantic_body_trait_default_mark(hir, body_id,
        expression->data.if_expr.condition, seen, depth + 1u, &condition);
    if (status != CM_SEMANTIC_BODY_OK) return status;
    if (condition->kind != CM_HIR_EXPR_BINARY
        || !cm_semantic_body_bool_type(hir, condition->type)
        || condition->data.binary.operator_kind
            != (result_type->data.integer_type.kind == CM_HIR_INT_U32
                ? CM_HIR_BINARY_EQUAL : CM_HIR_BINARY_LESS)
        || condition->data.binary.left
            >= expression->data.if_expr.condition
        || condition->data.binary.right
            >= expression->data.if_expr.condition) {
        return CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    status = cm_semantic_body_trait_default_value(hir, body_id,
        condition->data.binary.left, seen, depth + 2u);
    if (status == CM_SEMANTIC_BODY_OK) {
        status = cm_semantic_body_trait_default_value(hir, body_id,
            condition->data.binary.right, seen, depth + 2u);
    }
    if (status == CM_SEMANTIC_BODY_OK) {
        status = cm_semantic_body_trait_default_branch(hir, body_id,
            expression->data.if_expr.then_expression, seen, depth + 1u);
    }
    return status != CM_SEMANTIC_BODY_OK ? status
        : cm_semantic_body_trait_default_branch(hir, body_id,
            expression->data.if_expr.else_expression, seen, depth + 1u);
}

/*
 * Reauthenticate the source-lowering capability at the typed-HIR boundary.
 * Body lowering admits only one outer block, scalar let initializers, and an
 * optional outer-tail if.  Rejecting every other already-typed graph here
 * prevents callers from bypassing the deliberately closed trait-default
 * slice through model construction APIs.
 */
static CmSemanticBodyStatus cm_semantic_body_trait_default_shape(
    const CmHirContext *hir, CmHirBodyId body_id, const CmHirBody *body)
{
    const CmHirExpr *root;
    const CmHirExpr *tail;
    unsigned char *seen;
    CmSemanticBodyStatus status;
    uint32_t index;

    if (hir == NULL || body == NULL || hir->expressions.len == 0u) {
        return CM_SEMANTIC_BODY_INVALID;
    }
    seen = (unsigned char *)cm_alloc_zeroed(hir->expressions.len,
        sizeof(*seen));
    status = cm_semantic_body_trait_default_mark(hir, body_id,
        body->root_expression, seen, 0u, &root);
    if (status == CM_SEMANTIC_BODY_OK
        && (root->kind != CM_HIR_EXPR_BLOCK
            || !cm_semantic_body_trait_default_scalar(hir, root->type)
            || root->data.block.tail_expression == CM_HIR_EXPR_NONE
            || root->data.block.tail_expression >= body->root_expression
            || (root->data.block.statement_count == 0u)
                != (root->data.block.statements == NULL))) {
        status = CM_SEMANTIC_BODY_UNSUPPORTED;
    }
    for (index = 0u; status == CM_SEMANTIC_BODY_OK
            && index < root->data.block.statement_count; ++index) {
        const CmHirStatement *statement;

        statement = &root->data.block.statements[index];
        if (statement->kind != CM_HIR_STATEMENT_LET
            || statement->data.let_statement.initializer
                >= body->root_expression) {
            status = CM_SEMANTIC_BODY_UNSUPPORTED;
        } else {
            status = cm_semantic_body_trait_default_value(hir, body_id,
                statement->data.let_statement.initializer, seen, 1u);
        }
    }
    tail = status == CM_SEMANTIC_BODY_OK ? cm_hir_get_expr(hir,
        root->data.block.tail_expression) : NULL;
    if (status == CM_SEMANTIC_BODY_OK && tail == NULL) {
        status = CM_SEMANTIC_BODY_INVALID;
    } else if (status == CM_SEMANTIC_BODY_OK
        && tail->kind == CM_HIR_EXPR_IF) {
        status = cm_semantic_body_trait_default_if(hir, body_id,
            root->data.block.tail_expression, seen, 1u);
    } else if (status == CM_SEMANTIC_BODY_OK) {
        status = cm_semantic_body_trait_default_value(hir, body_id,
            root->data.block.tail_expression, seen, 1u);
    }
    cm_free(seen);
    return status;
}

static CmSemanticBodyResult cm_semantic_body_check_calls_mode(
    CmSemanticSession *session, CmHirBodyId body_id,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count,
    const CmHirInstanceSpec *instance_spec,
    const CmHirCanonicalInstanceParts *instance_parts,
    int definition_mode,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    CmSemanticBodyResult result;
    const CmHirContext *hir;
    const CmHirBody *body;
    const CmHirItem *owner_item;
    const CmHirItem *enclosing_item;
    const CmHirItem *trait_item;
    CmHirDefId owner;
    CmHirBodyFunctionOwnerKind owner_kind;
    CmTypeckContext *typeck;
    CmTypeckSnapshot snapshot;
    CmTypeckGenericArg *owner_arguments;
    CmTypeckGenericArg *enclosing_arguments;
    CmTypeckGenericArg *implemented_arguments;
    CmTypeckGenericArg *callee_arguments;
    CmTypeckInstantiation owner_instantiation;
    CmTypeckInstantiation enclosing_instantiation;
    CmTypeckInstantiationFrame owner_frames[2];
    CmTypeckScopedInstantiation owner_scoped_instantiation;
    CmParamEnvSubstitution environment_substitution;
    CmSemanticBodyConstraints constraints;
    CmVec deferred_equalities;
    CmHirExprId *call_expressions;
    size_t call_expression_count;
    size_t call_index;
    CmTypeckStatus typeck_status;
    CmTypeckTypeId *expression_terms;
    size_t expression_term_bytes;
    uint32_t owner_argument_index;
    CmSemanticCheckedBodyFacts checked_facts;
    CmSemanticCheckedCallFacts *checked_calls;
    CmSemanticCheckedCallableFacts *checked_callables;
    CmSemanticCheckedAdjustmentFacts *checked_adjustments;
    CmSemanticCheckedPrimitiveBinaryFacts *checked_primitive_binaries;
    CmSemanticCheckedFieldSelectionFacts *checked_field_selections;
    CmTypeckTypeId *signature_parameter_types;
    size_t checked_call_bytes;
    size_t checked_callable_bytes;
    size_t checked_adjustment_bytes;
    size_t checked_primitive_binary_bytes;
    size_t checked_field_selection_bytes;
    size_t signature_parameter_bytes;

    result = cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body_id);
    memset(&checked_facts, 0, sizeof(checked_facts));
    if (session == NULL || !cm_semantic_session_is_current(session)) {
        result.status = session != NULL && session->state != NULL
            ? CM_SEMANTIC_BODY_STALE : CM_SEMANTIC_BODY_INVALID;
        return result;
    }
    hir = cm_semantic_session_hir(session);
    body = cm_hir_get_body(hir, body_id);
    owner = cm_semantic_session_exact_owner(session);
    owner_item = body == NULL ? NULL
        : cm_semantic_body_item(hir, body->owner);
    owner_kind = cm_hir_body_function_owner_kind(hir, owner_item);
    enclosing_item = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_CONCRETE_TRAIT_IMPL_METHOD
            || owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD
        ? cm_semantic_body_item(hir, owner_item->parent_definition) : NULL;
    trait_item = owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
        ? cm_semantic_body_item(hir, owner_item->parent_definition) : NULL;
    if (!definition_mode && owner_kind
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT) {
        CmHirDefId enclosing_definition;

        enclosing_definition = instance_spec != NULL
            ? instance_spec->enclosing_impl
            : instance_parts != NULL ? instance_parts->enclosing_impl
                : cm_hir_def_id_none();
        enclosing_item = cm_semantic_body_item(hir, enclosing_definition);
    }
    if (hir == NULL || body == NULL || owner_item == NULL
        || owner_item->kind != CM_HIR_ITEM_FUNCTION
        || owner_kind == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED
        || body->state != CM_HIR_BODY_TYPED
        || body->root_expression == CM_HIR_EXPR_NONE
        || owner_item->data.function_item.body != body_id
        || !cm_hir_def_id_equal(body->owner, owner)
        || (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
            ? !cm_hir_def_id_is_none(
                cm_semantic_session_enclosing_owner(session))
            : owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
                ? trait_item == NULL
                    || trait_item->kind != CM_HIR_ITEM_TRAIT
                    || !cm_hir_def_id_equal(
                        owner_item->parent_definition,
                        cm_semantic_session_enclosing_owner(session))
                : enclosing_item == NULL
                    || enclosing_item->kind != CM_HIR_ITEM_IMPL
                    || !cm_hir_def_id_equal(
                        owner_item->parent_definition,
                        cm_semantic_session_enclosing_owner(session)))
        || (owner_type_substitution_count == 0u)
            != (owner_type_substitutions == NULL)
        || (instance_spec != NULL && instance_parts != NULL)
        || (definition_mode
            && (instance_spec != NULL || instance_parts != NULL))
        || (!definition_mode && instance_spec == NULL
            && instance_parts == NULL
            && (owner_kind
                    == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD
                || owner_kind
                    == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT))
        || (definition_mode
            ? (owner_type_substitution_count != 0u
                || !cm_semantic_type_only_owner(hir, owner_item,
                    owner_item->generic_parameter_count))
            : instance_spec == NULL && instance_parts == NULL
                ? !cm_semantic_type_only_owner(hir, owner_item,
                    owner_type_substitution_count)
                : owner_type_substitution_count != 0u)) {
        return result;
    }
    for (owner_argument_index = 0u;
         owner_argument_index < owner_item->data.function_item.signature
            .parameter_count;
         ++owner_argument_index) {
        switch (owner_item->data.function_item.signature.parameters[
                owner_argument_index].binding_mode) {
        case CM_HIR_PARAMETER_BINDING_MOVE:
            break;
        case CM_HIR_PARAMETER_BINDING_REF_SHARED:
        case CM_HIR_PARAMETER_BINDING_REF_MUTABLE:
        case CM_HIR_PARAMETER_BINDING_DEREF_SHARED:
        default:
            result.status = CM_SEMANTIC_BODY_UNSUPPORTED;
            return result;
        }
    }
    if (instance_spec != NULL
        && (!cm_hir_def_id_equal(instance_spec->selected_callable, owner)
            || !cm_hir_def_id_equal(instance_spec->body_definition, owner)
            || (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                ? !cm_hir_def_id_is_none(
                        instance_spec->declared_trait_callable)
                    || !cm_hir_def_id_is_none(instance_spec->enclosing_impl)
                    || !cm_hir_def_id_is_none(
                        instance_spec->implemented_trait)
                    || !cm_hir_def_id_is_none(instance_spec->self_owner)
                    || instance_spec->self_type != CM_HIR_TYPE_NONE
                    || instance_spec->item_argument_count
                        != owner_item->generic_parameter_count
                    || (instance_spec->item_argument_count == 0u)
                        != (instance_spec->item_arguments == NULL)
                    || instance_spec->method_argument_count != 0u
                    || instance_spec->method_arguments != NULL
                    || instance_spec->enclosing_impl_argument_count != 0u
                    || instance_spec->enclosing_impl_arguments != NULL
                    || instance_spec->implemented_trait_argument_count != 0u
                    || instance_spec->implemented_trait_arguments != NULL
                : !cm_hir_def_id_equal(instance_spec->enclosing_impl,
                    enclosing_item->definition)
            || !cm_hir_def_id_equal(instance_spec->declared_trait_callable,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
                    ? owner_item->definition
                    : owner_item->data.function_item.trait_item_definition)
            || !cm_hir_def_id_equal(instance_spec->implemented_trait,
                enclosing_item->data.impl_item.trait_type.definition)
            || !cm_hir_def_id_equal(instance_spec->self_owner,
                enclosing_item->definition)
            || instance_spec->self_type == CM_HIR_TYPE_NONE
            || instance_spec->item_argument_count != 0u
            || instance_spec->item_arguments != NULL
            || instance_spec->method_argument_count
                != owner_item->generic_parameter_count
            || (instance_spec->method_argument_count == 0u)
                != (instance_spec->method_arguments == NULL)
            || instance_spec->enclosing_impl_argument_count
                != enclosing_item->generic_parameter_count
            || (instance_spec->enclosing_impl_argument_count == 0u)
                != (instance_spec->enclosing_impl_arguments == NULL)
            || instance_spec->implemented_trait_argument_count
                != enclosing_item->data.impl_item.trait_type.argument_count
            || (instance_spec->implemented_trait_argument_count == 0u)
                != (instance_spec->implemented_trait_arguments == NULL)))) {
        return result;
    }
    if (instance_parts != NULL
        && (instance_parts->selected_callable.crate_id
                != cm_semantic_session_local_crate(session)
            || !cm_hir_def_id_equal(instance_parts->selected_callable, owner)
            || !cm_hir_def_id_equal(instance_parts->body_definition, owner)
            || (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                ? !cm_hir_def_id_is_none(
                        instance_parts->declared_trait_callable)
                    || !cm_hir_def_id_is_none(instance_parts->enclosing_impl)
                    || !cm_hir_def_id_is_none(
                        instance_parts->implemented_trait)
                    || !cm_hir_def_id_is_none(instance_parts->self_owner)
                    || instance_parts->self_type != NULL
                    || instance_parts->self_type_size != 0u
                    || instance_parts->item_argument_count
                        != owner_item->generic_parameter_count
                    || (instance_parts->item_argument_count == 0u)
                        != (instance_parts->item_arguments == NULL)
                    || instance_parts->method_argument_count != 0u
                    || instance_parts->method_arguments != NULL
                    || instance_parts->enclosing_impl_argument_count != 0u
                    || instance_parts->enclosing_impl_arguments != NULL
                    || instance_parts->implemented_trait_argument_count != 0u
                    || instance_parts->implemented_trait_arguments != NULL
                : !cm_hir_def_id_equal(instance_parts->enclosing_impl,
                    enclosing_item->definition)
            || !cm_hir_def_id_equal(
                instance_parts->declared_trait_callable,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
                    ? owner_item->definition
                    : owner_item->data.function_item.trait_item_definition)
            || !cm_hir_def_id_equal(instance_parts->implemented_trait,
                enclosing_item->data.impl_item.trait_type.definition)
            || !cm_hir_def_id_equal(instance_parts->self_owner,
                enclosing_item->definition)
            || instance_parts->self_type == NULL
            || instance_parts->self_type_size == 0u
            || instance_parts->item_argument_count != 0u
            || instance_parts->item_arguments != NULL
            || instance_parts->method_argument_count
                != owner_item->generic_parameter_count
            || (instance_parts->method_argument_count == 0u)
                != (instance_parts->method_arguments == NULL)
            || instance_parts->enclosing_impl_argument_count
                != enclosing_item->generic_parameter_count
            || (instance_parts->enclosing_impl_argument_count == 0u)
                != (instance_parts->enclosing_impl_arguments == NULL)
            || instance_parts->implemented_trait_argument_count
                != enclosing_item->data.impl_item.trait_type.argument_count
            || (instance_parts->implemented_trait_argument_count == 0u)
                != (instance_parts->implemented_trait_arguments == NULL)))) {
        return result;
    }
    typeck = cm_semantic_session_typeck(session);
    if (typeck == NULL) {
        result.status = CM_SEMANTIC_BODY_STALE;
        return result;
    }
    owner_arguments = NULL;
    enclosing_arguments = NULL;
    implemented_arguments = NULL;
    callee_arguments = NULL;
    expression_terms = NULL;
    checked_calls = NULL;
    checked_callables = NULL;
    checked_adjustments = NULL;
    checked_primitive_binaries = NULL;
    checked_field_selections = NULL;
    signature_parameter_types = NULL;
    result.status = cm_semantic_body_collect_calls(hir, body_id,
        body->root_expression, &call_expressions, &call_expression_count);
    if (result.status != CM_SEMANTIC_BODY_OK) return result;
    if (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT) {
        result.status = cm_semantic_body_trait_default_shape(hir, body_id,
            body);
    }
    if (result.status != CM_SEMANTIC_BODY_OK
        || (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            && call_expression_count != 0u)) {
        cm_free(call_expressions);
        if (result.status == CM_SEMANTIC_BODY_OK) {
            result.status = CM_SEMANTIC_BODY_UNSUPPORTED;
        }
        return result;
    }
    if (!cm_size_mul(hir->expressions.len, sizeof(*expression_terms),
            &expression_term_bytes)) {
        result.status = CM_SEMANTIC_BODY_OVERFLOW;
        cm_free(call_expressions);
        return result;
    }
    expression_terms = (CmTypeckTypeId *)cm_alloc_zeroed(1u,
        expression_term_bytes);
    if (!cm_size_mul(call_expression_count, sizeof(*checked_calls),
            &checked_call_bytes)
        || !cm_size_mul(call_expression_count,
            sizeof(*checked_callables), &checked_callable_bytes)
        || !cm_size_mul(hir->expressions.len,
            sizeof(*checked_adjustments), &checked_adjustment_bytes)
        || !cm_size_mul(hir->expressions.len,
            sizeof(*checked_primitive_binaries),
            &checked_primitive_binary_bytes)
        || !cm_size_mul(hir->expressions.len,
            sizeof(*checked_field_selections),
            &checked_field_selection_bytes)
        || !cm_size_mul((size_t)owner_item->data.function_item.signature
                .parameter_count, sizeof(*signature_parameter_types),
            &signature_parameter_bytes)) {
        result.status = CM_SEMANTIC_BODY_OVERFLOW;
        cm_free(call_expressions);
        cm_free(expression_terms);
        return result;
    }
    checked_calls = checked_call_bytes == 0u ? NULL
        : (CmSemanticCheckedCallFacts *)cm_alloc_zeroed(1u,
            checked_call_bytes);
    checked_callables = checked_callable_bytes == 0u ? NULL
        : (CmSemanticCheckedCallableFacts *)cm_alloc_zeroed(1u,
            checked_callable_bytes);
    checked_adjustments = checked_adjustment_bytes == 0u ? NULL
        : (CmSemanticCheckedAdjustmentFacts *)cm_alloc_zeroed(1u,
            checked_adjustment_bytes);
    checked_primitive_binaries = checked_primitive_binary_bytes == 0u ? NULL
        : (CmSemanticCheckedPrimitiveBinaryFacts *)cm_alloc_zeroed(1u,
            checked_primitive_binary_bytes);
    checked_field_selections = checked_field_selection_bytes == 0u ? NULL
        : (CmSemanticCheckedFieldSelectionFacts *)cm_alloc_zeroed(1u,
            checked_field_selection_bytes);
    signature_parameter_types = signature_parameter_bytes == 0u ? NULL
        : (CmTypeckTypeId *)cm_alloc_zeroed(1u,
            signature_parameter_bytes);
    checked_facts.expression_terms = expression_terms;
    checked_facts.expression_term_count = hir->expressions.len;
    checked_facts.signature_parameter_types = signature_parameter_types;
    checked_facts.signature_parameter_count =
        owner_item->data.function_item.signature.parameter_count;
    checked_facts.calls = checked_calls;
    checked_facts.call_count = 0u;
    checked_facts.callables = checked_callables;
    checked_facts.callable_count = 0u;
    checked_facts.adjustments = checked_adjustments;
    checked_facts.adjustment_count = 0u;
    checked_facts.primitive_binaries = checked_primitive_binaries;
    checked_facts.primitive_binary_count = 0u;
    checked_facts.field_selections = checked_field_selections;
    checked_facts.field_selection_count = 0u;
    memset(&snapshot, 0, sizeof(snapshot));
    typeck_status = cm_typeck_snapshot(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = cm_semantic_typeck_status(typeck_status);
        result.typeck_status = typeck_status;
        cm_free(call_expressions);
        cm_free(expression_terms);
        cm_free(checked_calls);
        cm_free(checked_callables);
        cm_free(checked_adjustments);
        cm_free(checked_primitive_binaries);
        cm_free(checked_field_selections);
        cm_free(signature_parameter_types);
        return result;
    }
    cm_vec_init(&deferred_equalities, sizeof(CmSemanticBodyEquality));
    memset(&constraints, 0, sizeof(constraints));
    constraints.session = session;
    constraints.typeck = typeck;
    constraints.hir = hir;
    constraints.body = body;
    constraints.body_id = body_id;
    constraints.normalize_limits.max_nodes =
        CM_SEMANTIC_BODY_NORMALIZE_NODES;
    constraints.normalize_limits.max_projection_steps =
        CM_SEMANTIC_BODY_NORMALIZE_PROJECTIONS;
    constraints.deferred_equalities = &deferred_equalities;
    constraints.expression_terms = expression_terms;
    constraints.expression_term_count = hir->expressions.len;
    constraints.checked_facts = &checked_facts;
    constraints.evidence_writeback = writeback;
    constraints.failed_expression = body->root_expression;
    constraints.failed_callee = cm_hir_def_id_none();
    constraints.failed_predicate_index =
        CM_SEMANTIC_BODY_PREDICATE_NONE;
    constraints.typeck_status = CM_TYPECK_OK;
    constraints.solver_kind = CM_TRAIT_SOLVER_INVALID;

    cm_typeck_instantiation_init(typeck, &owner_instantiation);
    cm_typeck_instantiation_init(typeck, &enclosing_instantiation);
    memset(owner_frames, 0, sizeof(owner_frames));
    cm_typeck_scoped_instantiation_init(typeck,
        &owner_scoped_instantiation);
    owner_instantiation.parameter_owner = owner;
    if (enclosing_item != NULL) {
        CmSemanticTypeScan scan;
        CmTypeckInstantiationFrame enclosing_frame;
        CmTypeckScopedInstantiation enclosing_scoped;

        scan = cm_semantic_scan_merge(
            cm_semantic_scan_type(hir,
                enclosing_item->data.impl_item.self_type, 0u),
            cm_semantic_scan_named(hir,
                &enclosing_item->data.impl_item.trait_type, 0u));
        result.status = cm_semantic_scan_status(scan);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (!definition_mode) {
            result.status = instance_parts == NULL
                ? cm_semantic_body_import_generic_arguments(hir, typeck,
                    &result.typeck_status, enclosing_item,
                    instance_spec->enclosing_impl_arguments,
                    instance_spec->enclosing_impl_argument_count,
                    &enclosing_arguments)
                : cm_semantic_body_import_canonical_arguments(hir, typeck,
                    enclosing_item,
                    instance_parts->enclosing_impl_arguments,
                    instance_parts->enclosing_impl_argument_count,
                    body->span, &enclosing_arguments);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
        } else {
            result.status = cm_semantic_body_allocate_arguments(
                enclosing_item->generic_parameter_count,
                &enclosing_arguments);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            for (owner_argument_index = 0u;
                 owner_argument_index
                    < enclosing_item->generic_parameter_count;
                 ++owner_argument_index) {
                const CmHirGenericParam *parameter;
                CmTypeckType rigid_type;

                parameter = cm_hir_get_generic_param(hir,
                    enclosing_item->generic_parameter_start
                        + owner_argument_index);
                if (parameter == NULL
                    || parameter->kind != CM_HIR_GENERIC_TYPE
                    || parameter->index != owner_argument_index
                    || !cm_hir_def_id_equal(parameter->owner,
                        enclosing_item->definition)) {
                    result.status = CM_SEMANTIC_BODY_INVALID;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                enclosing_arguments[owner_argument_index].kind =
                    CM_HIR_GENERIC_ARG_TYPE;
                memset(&rigid_type, 0, sizeof(rigid_type));
                rigid_type.kind = CM_TYPECK_TYPE_PARAMETER;
                rigid_type.span = parameter->span;
                rigid_type.data.parameter_type.parameter =
                    enclosing_item->generic_parameter_start
                        + owner_argument_index;
                typeck_status = cm_typeck_add_type(typeck, &rigid_type,
                    &enclosing_arguments[owner_argument_index].data.type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
            }
        }
        memset(&enclosing_frame, 0, sizeof(enclosing_frame));
        enclosing_frame.parameter_owner = enclosing_item->definition;
        enclosing_frame.arguments = enclosing_arguments;
        enclosing_frame.argument_count =
            enclosing_item->generic_parameter_count;
        cm_typeck_scoped_instantiation_init(typeck, &enclosing_scoped);
        enclosing_scoped.frames = &enclosing_frame;
        enclosing_scoped.frame_count = 1u;
        typeck_status = cm_typeck_instantiate_hir_type_scoped(typeck,
            enclosing_item->data.impl_item.self_type, &enclosing_scoped,
            &owner_instantiation.self_type);
        if (typeck_status != CM_TYPECK_OK) {
            result.status = cm_semantic_typeck_status(typeck_status);
            result.typeck_status = typeck_status;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (!definition_mode && instance_spec != NULL) {
            CmTypeckTypeId requested_self_type;
            CmTypeckNamedType implemented_trait;

            typeck_status = cm_typeck_import_hir_type(typeck,
                instance_spec->self_type, &requested_self_type);
            if (typeck_status == CM_TYPECK_OK) {
                typeck_status = cm_typeck_unify(typeck,
                    owner_instantiation.self_type, requested_self_type);
            }
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            memset(&implemented_trait, 0, sizeof(implemented_trait));
            typeck_status = cm_typeck_instantiate_hir_named_scoped(typeck,
                &enclosing_item->data.impl_item.trait_type,
                &enclosing_scoped, &implemented_trait);
            if (typeck_status != CM_TYPECK_OK
                || implemented_trait.argument_count
                    != instance_spec->implemented_trait_argument_count
                || !cm_semantic_body_typeck_arguments_match_hir(
                    &constraints, implemented_trait.arguments,
                    instance_spec->implemented_trait_arguments,
                    implemented_trait.argument_count)) {
                result.status = typeck_status == CM_TYPECK_OK
                    ? CM_SEMANTIC_BODY_INVALID
                    : cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
        }
        if (!definition_mode && instance_parts != NULL) {
            CmTypeckTypeId requested_self_type;
            CmTypeckNamedType implemented_trait;

            typeck_status = CM_TYPECK_OK;
            result.status = cm_semantic_body_import_canonical_type(hir,
                typeck, instance_parts->self_type,
                instance_parts->self_type_size, body->span,
                &requested_self_type);
            if (result.status == CM_SEMANTIC_BODY_OK) {
                typeck_status = cm_typeck_unify(typeck,
                    owner_instantiation.self_type, requested_self_type);
                result.status = cm_semantic_typeck_status(typeck_status);
            }
            if (result.status != CM_SEMANTIC_BODY_OK) {
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            typeck_status = CM_TYPECK_OK;
            result.status = cm_semantic_body_import_canonical_arguments(hir,
                typeck, cm_semantic_body_item(hir,
                    instance_parts->implemented_trait),
                instance_parts->implemented_trait_arguments,
                instance_parts->implemented_trait_argument_count,
                body->span, &implemented_arguments);
            memset(&implemented_trait, 0, sizeof(implemented_trait));
            if (result.status == CM_SEMANTIC_BODY_OK) {
                typeck_status = cm_typeck_instantiate_hir_named_scoped(
                    typeck, &enclosing_item->data.impl_item.trait_type,
                    &enclosing_scoped, &implemented_trait);
                result.status = cm_semantic_typeck_status(typeck_status);
            }
            if (result.status != CM_SEMANTIC_BODY_OK
                || implemented_trait.argument_count
                    != instance_parts->implemented_trait_argument_count
                || !cm_semantic_body_typeck_arguments_match(&constraints,
                    implemented_trait.arguments, implemented_arguments,
                    implemented_trait.argument_count)) {
                if (result.status == CM_SEMANTIC_BODY_OK) {
                    result.status = CM_SEMANTIC_BODY_INVALID;
                }
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
        }
        owner_instantiation.self_owner = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            ? trait_item->definition : enclosing_item->definition;
        enclosing_instantiation.parameter_owner = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            ? trait_item->definition : enclosing_item->definition;
        enclosing_instantiation.arguments = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            ? implemented_arguments : enclosing_arguments;
        enclosing_instantiation.argument_count = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            ? trait_item->generic_parameter_count
            : enclosing_item->generic_parameter_count;
        enclosing_instantiation.self_owner = owner_kind
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT
            ? trait_item->definition : enclosing_item->definition;
        enclosing_instantiation.self_type = owner_instantiation.self_type;
    } else if (trait_item != NULL) {
        /*
         * A trait default's parameter environment has two authenticated
         * owners even in the closed zero-generic slice: the method is exact
         * and the trait is enclosing.  Keep the trait frame explicit so
         * normalization validates the same environment that the session
         * published.  There is intentionally no Self substitution here;
         * signatures requiring one are rejected by body-owner admission.
         */
        enclosing_instantiation.parameter_owner = trait_item->definition;
    }
    result.status = instance_spec == NULL && instance_parts == NULL
        ? cm_semantic_body_allocate_arguments(
            owner_item->generic_parameter_count, &owner_arguments)
        : instance_parts == NULL
            ? cm_semantic_body_import_generic_arguments(hir, typeck,
                &result.typeck_status, owner_item,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                    ? instance_spec->item_arguments
                    : instance_spec->method_arguments,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                    ? instance_spec->item_argument_count
                    : instance_spec->method_argument_count,
                &owner_arguments)
            : cm_semantic_body_import_canonical_arguments(hir, typeck,
                owner_item,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                    ? instance_parts->item_arguments
                    : instance_parts->method_arguments,
                owner_kind == CM_HIR_BODY_FUNCTION_OWNER_FREE
                    ? instance_parts->item_argument_count
                    : instance_parts->method_argument_count,
                body->span, &owner_arguments);
    if (result.status != CM_SEMANTIC_BODY_OK) {
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    if (owner_item->generic_parameter_count != 0u) {
        for (owner_argument_index = 0u;
             owner_argument_index < owner_item->generic_parameter_count;
             ++owner_argument_index) {
            const CmHirGenericParam *parameter;

            parameter = cm_hir_get_generic_param(hir,
                owner_item->generic_parameter_start + owner_argument_index);
            if (parameter == NULL
                || (instance_parts == NULL
                    && parameter->kind != CM_HIR_GENERIC_TYPE)
                || parameter->index != owner_argument_index
                || !cm_hir_def_id_equal(parameter->owner, owner)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                    call_expressions);
            }
            if (instance_parts != NULL) {
                typeck_status = CM_TYPECK_OK;
            } else {
                owner_arguments[owner_argument_index].kind =
                    CM_HIR_GENERIC_ARG_TYPE;
            }
            if (instance_spec != NULL || instance_parts != NULL) {
                typeck_status = CM_TYPECK_OK;
            } else if (!definition_mode) {
                CmSemanticTypeScan scan;

                scan = cm_semantic_scan_type(hir,
                    owner_type_substitutions[owner_argument_index], 0u);
                result.status = cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                typeck_status = cm_typeck_import_hir_type(typeck,
                    owner_type_substitutions[owner_argument_index],
                    &owner_arguments[owner_argument_index].data.type);
            } else {
                CmTypeckType rigid_type;

                memset(&rigid_type, 0, sizeof(rigid_type));
                rigid_type.kind = CM_TYPECK_TYPE_PARAMETER;
                rigid_type.span = parameter->span;
                rigid_type.data.parameter_type.parameter =
                    owner_item->generic_parameter_start
                        + owner_argument_index;
                typeck_status = cm_typeck_add_type(typeck, &rigid_type,
                    &owner_arguments[owner_argument_index].data.type);
            }
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
        }
        owner_instantiation.arguments = owner_arguments;
        owner_instantiation.argument_count =
            owner_item->generic_parameter_count;
    }
    if (!cm_typeck_instantiation_is_valid(typeck, &owner_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    if ((enclosing_item != NULL || trait_item != NULL)
        && !cm_typeck_instantiation_is_valid(typeck,
            &enclosing_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    owner_frames[0].parameter_owner = owner;
    owner_frames[0].arguments = owner_arguments;
    owner_frames[0].argument_count = owner_item->generic_parameter_count;
    owner_scoped_instantiation.frames = owner_frames;
    owner_scoped_instantiation.frame_count = 1u;
    owner_scoped_instantiation.self_owner = owner_instantiation.self_owner;
    owner_scoped_instantiation.self_type = owner_instantiation.self_type;
    if (enclosing_item != NULL) {
        owner_frames[1].parameter_owner = enclosing_item->definition;
        owner_frames[1].arguments = enclosing_arguments;
        owner_frames[1].argument_count =
            enclosing_item->generic_parameter_count;
        owner_scoped_instantiation.frame_count = 2u;
        if (owner_kind == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT) {
            owner_frames[1].parameter_owner = trait_item->definition;
            owner_frames[1].arguments = implemented_arguments;
            owner_frames[1].argument_count = trait_item->generic_parameter_count;
        }
    } else if (trait_item != NULL) {
        owner_frames[1].parameter_owner = trait_item->definition;
        owner_scoped_instantiation.frame_count = 2u;
    }
    if (!cm_typeck_scoped_instantiation_is_valid(typeck,
            &owner_scoped_instantiation)) {
        result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    memset(&environment_substitution, 0,
        sizeof(environment_substitution));
    environment_substitution.exact = &owner_instantiation;
    if (enclosing_item != NULL || trait_item != NULL) {
        environment_substitution.enclosing = &enclosing_instantiation;
    }
    constraints.owner_instantiation = &owner_scoped_instantiation;
    constraints.substitution = &environment_substitution;

    result.status = cm_semantic_body_instantiate_owner_type(&constraints,
        owner_item->data.function_item.signature.return_type,
        &checked_facts.signature_return_type);
    if (result.status != CM_SEMANTIC_BODY_OK) {
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    for (owner_argument_index = 0u;
         owner_argument_index < checked_facts.signature_parameter_count;
         ++owner_argument_index) {
        result.status = cm_semantic_body_instantiate_owner_type(&constraints,
            owner_item->data.function_item.signature.parameters[
                owner_argument_index].type,
            &signature_parameter_types[owner_argument_index]);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
    }

    result.status = cm_semantic_body_constrain(&constraints, owner_item);
    if (result.status != CM_SEMANTIC_BODY_OK) {
        result.expression = constraints.failed_expression;
        result.callee = constraints.failed_callee;
        result.predicate_index = constraints.failed_predicate_index;
        result.typeck_status = constraints.typeck_status;
        result.solver_kind = constraints.solver_kind;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }

    for (call_index = 0u; call_index < call_expression_count; ++call_index) {
        const CmHirExpr *expression;
        CmHirExprId expression_id;
        const CmHirItem *callee;
        CmSemanticCheckedCallFacts *checked_call;
        CmTypeckInstantiation callee_instantiation;
        uint32_t predicate_index;
        uint32_t callee_argument_index;

        expression_id = call_expressions[call_index];
        expression = cm_hir_get_expr(hir, expression_id);
        if (expression == NULL || expression->owner_body != body_id
            || (expression->kind != CM_HIR_EXPR_CALL
                && expression->kind != CM_HIR_EXPR_METHOD_CALL
                && expression->kind != CM_HIR_EXPR_QUALIFIED_CALL)) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                call_expressions);
        }
        if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
            result.expression = expression_id;
            result.callee = expression->data.qualified_call
                .declared_trait_callable;
            constraints.failed_expression = expression_id;
            constraints.failed_callee = result.callee;
            constraints.failed_predicate_index =
                CM_SEMANTIC_BODY_PREDICATE_NONE;
            result.status = cm_semantic_body_check_qualified_callable(
                &constraints, &environment_substitution, expression_id,
                expression,
                &checked_callables[checked_facts.callable_count]);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                result.expression = constraints.failed_expression;
                result.callee = constraints.failed_callee;
                result.typeck_status = constraints.typeck_status;
                result.solver_kind = constraints.solver_kind;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            ++checked_facts.callable_count;
            continue;
        }
        if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
            result.expression = expression_id;
            constraints.failed_expression = expression_id;
            constraints.failed_callee = cm_hir_def_id_none();
            constraints.failed_predicate_index =
                CM_SEMANTIC_BODY_PREDICATE_NONE;
            result.status = cm_semantic_body_check_method_callable(
                &constraints, &environment_substitution, expression_id,
                expression,
                &checked_callables[checked_facts.callable_count]);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                result.expression = constraints.failed_expression;
                result.callee = constraints.failed_callee;
                result.typeck_status = constraints.typeck_status;
                result.solver_kind = constraints.solver_kind;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            result.callee = checked_callables[
                checked_facts.callable_count].selected_callable;
            ++checked_facts.callable_count;
            continue;
        }
        result.expression = expression_id;
        result.callee = expression->data.call.callee;
        constraints.failed_expression = expression_id;
        constraints.failed_callee = expression->data.call.callee;
        constraints.failed_predicate_index =
            CM_SEMANTIC_BODY_PREDICATE_NONE;
        callee = cm_semantic_body_item(hir, expression->data.call.callee);
        if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                call_expressions);
        }
        if (!cm_hir_def_id_is_none(callee->parent_definition)
            || (expression->data.call.type_substitution_count != 0u
                && expression->data.call.type_substitutions == NULL)
            || (expression->data.call.argument_count != 0u
                && expression->data.call.arguments == NULL)
            || !cm_semantic_type_only_owner(hir, callee,
                expression->data.call.type_substitution_count)) {
            result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if ((callee->predicate_scope_count == 0u)
                != (callee->predicate_scopes == NULL)
            || (callee->predicate_count == 0u)
                != (callee->predicates == NULL)
            || (callee->outlives_predicate_count == 0u)
                != (callee->outlives_predicates == NULL)) {
            result.status = CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (callee->predicate_scope_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (callee->outlives_predicate_count != 0u) {
            result.status = CM_SEMANTIC_BODY_PENDING_OUTLIVES;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        cm_free(callee_arguments);
        callee_arguments = NULL;
        cm_typeck_instantiation_init(typeck, &callee_instantiation);
        callee_instantiation.parameter_owner = callee->definition;
        result.status = cm_semantic_body_allocate_arguments(
            callee->generic_parameter_count, &callee_arguments);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        if (expression->data.call.type_substitution_count != 0u) {
            for (callee_argument_index = 0u;
                 callee_argument_index
                    < expression->data.call.type_substitution_count;
                 ++callee_argument_index) {
                CmSemanticTypeScan scan;

                scan = cm_semantic_scan_type(hir,
                    expression->data.call.type_substitutions[
                        callee_argument_index], 0u);
                result.status = scan == CM_SEMANTIC_TYPE_PROJECTION
                    ? CM_SEMANTIC_BODY_OK : cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                callee_arguments[callee_argument_index].kind =
                    CM_HIR_GENERIC_ARG_TYPE;
                typeck_status = cm_typeck_instantiate_hir_type(typeck,
                    expression->data.call.type_substitutions[
                        callee_argument_index], &owner_instantiation,
                    &callee_arguments[callee_argument_index].data.type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                {
                    CmProjectionNormalizeResult normalization;

                    normalization = cm_semantic_body_normalize(&constraints,
                        callee_arguments[callee_argument_index].data.type);
                    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                        result.status = cm_semantic_body_normalize_status(
                            &constraints, &normalization);
                        result.typeck_status = constraints.typeck_status;
                        result.solver_kind = constraints.solver_kind;
                        return cm_semantic_body_fail_snapshot(result, typeck,
                            &snapshot, call_expressions);
                    }
                    callee_arguments[callee_argument_index].data.type =
                        normalization.type;
                }
            }
            callee_instantiation.arguments = callee_arguments;
            callee_instantiation.argument_count =
                expression->data.call.type_substitution_count;
        }
        if (!cm_typeck_instantiation_is_valid(typeck,
                &callee_instantiation)) {
            result.status = CM_SEMANTIC_BODY_PENDING_SUBSTITUTION;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        checked_call = &checked_calls[checked_facts.call_count];
        checked_call->parameter_count = expression->data.call.argument_count;
        if (expression->data.call.argument_count != 0u) {
            checked_call->parameter_types =
                (CmTypeckTypeId *)cm_alloc_zeroed(
                    expression->data.call.argument_count,
                    sizeof(CmTypeckTypeId));
        }
        ++checked_facts.call_count;
        result.status = cm_semantic_body_check_call_signature(&constraints,
            expression_id, expression, callee, &callee_instantiation,
            checked_call);
        if (result.status != CM_SEMANTIC_BODY_OK) {
            result.expression = constraints.failed_expression;
            result.callee = constraints.failed_callee;
            result.predicate_index = constraints.failed_predicate_index;
            result.typeck_status = constraints.typeck_status;
            result.solver_kind = constraints.solver_kind;
            return cm_semantic_body_fail_snapshot(result, typeck,
                &snapshot, call_expressions);
        }
        for (predicate_index = 0u;
             predicate_index < callee->predicate_count;
             ++predicate_index) {
            const CmHirTraitPredicate *predicate;
            CmSemanticTypeScan scan;
            CmTraitGoal goal;
            CmTraitSelectionResult selection;
            CmTypeckTypeId implemented_self;
            CmTypeckNamedType implemented_trait;
            uint32_t equality_index;

            predicate = &callee->predicates[predicate_index];
            result.predicate_index = predicate_index;
            constraints.failed_expression = expression_id;
            constraints.failed_callee = callee->definition;
            constraints.failed_predicate_index = predicate_index;
            if ((predicate->binder.lifetime_count == 0u)
                    != (predicate->binder.lifetimes == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u) {
                result.status = CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if (predicate->modifier != CM_HIR_PREDICATE_REQUIRED) {
                result.status = CM_SEMANTIC_BODY_PENDING_MODIFIER;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            if ((predicate->equality_count == 0u)
                    != (predicate->equalities == NULL)) {
                result.status = CM_SEMANTIC_BODY_INVALID;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            scan = cm_semantic_scan_merge(
                cm_semantic_scan_type(hir, predicate->subject, 0u),
                cm_semantic_scan_named(hir, &predicate->trait_type, 0u));
            result.status = scan == CM_SEMANTIC_TYPE_PROJECTION
                ? CM_SEMANTIC_BODY_OK : cm_semantic_scan_status(scan);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            memset(&goal, 0, sizeof(goal));
            goal.kind = CM_TRAIT_GOAL_IMPLEMENTED;
            goal.data.implemented.owner = owner;
            typeck_status = cm_typeck_instantiate_hir_type(typeck,
                predicate->subject, &callee_instantiation,
                &goal.data.implemented.self_type);
            if (typeck_status == CM_TYPECK_OK) {
                typeck_status = cm_typeck_instantiate_hir_named(typeck,
                    &predicate->trait_type, &callee_instantiation,
                    &goal.data.implemented.trait_type);
            }
            if (typeck_status != CM_TYPECK_OK) {
                result.status = cm_semantic_typeck_status(typeck_status);
                result.typeck_status = typeck_status;
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            {
                CmProjectionNormalizeResult normalization;

                normalization = cm_semantic_body_normalize(&constraints,
                    goal.data.implemented.self_type);
                if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                    result.status = cm_semantic_body_normalize_status(
                        &constraints, &normalization);
                    result.typeck_status = constraints.typeck_status;
                    result.solver_kind = constraints.solver_kind;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                goal.data.implemented.self_type = normalization.type;
                result.status = cm_semantic_body_normalize_named(
                    &constraints, &goal.data.implemented.trait_type);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    result.typeck_status = constraints.typeck_status;
                    result.solver_kind = constraints.solver_kind;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
            }
            selection = cm_semantic_session_solve_goal(session,
                typeck, &environment_substitution, &goal);
            result.solver_kind = selection.kind;
            result.typeck_status = selection.typeck_status;
            result.status = cm_semantic_solver_status(selection.kind);
            if (result.status != CM_SEMANTIC_BODY_OK) {
                return cm_semantic_body_fail_snapshot(result, typeck,
                    &snapshot, call_expressions);
            }
            implemented_self = goal.data.implemented.self_type;
            implemented_trait = goal.data.implemented.trait_type;
            for (equality_index = 0u;
                 equality_index < predicate->equality_count;
                 ++equality_index) {
                const CmHirAssociatedTypeEquality *equality;
                CmTypeckType projection;
                CmTypeckTypeId projection_type;
                CmTypeckTypeId expected_type;

                equality = &predicate->equalities[equality_index];
                scan = cm_semantic_scan_type(hir, equality->value, 0u);
                result.status = scan == CM_SEMANTIC_TYPE_PROJECTION
                    ? CM_SEMANTIC_BODY_OK : cm_semantic_scan_status(scan);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                typeck_status = cm_typeck_instantiate_hir_type(typeck,
                    equality->value, &callee_instantiation,
                    &expected_type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                {
                    CmProjectionNormalizeResult normalization;

                    normalization = cm_semantic_body_normalize(&constraints,
                        expected_type);
                    if (normalization.kind != CM_TRAIT_SOLVER_PROVEN) {
                        result.status = cm_semantic_body_normalize_status(
                            &constraints, &normalization);
                        result.typeck_status = constraints.typeck_status;
                        result.solver_kind = constraints.solver_kind;
                        return cm_semantic_body_fail_snapshot(result, typeck,
                            &snapshot, call_expressions);
                    }
                    expected_type = normalization.type;
                }
                memset(&projection, 0, sizeof(projection));
                projection.kind = CM_TYPECK_TYPE_PROJECTION;
                projection.span = equality->span;
                projection.data.projection_type.self_type =
                    implemented_self;
                projection.data.projection_type.trait_type =
                    implemented_trait;
                projection.data.projection_type.associated_type.definition =
                    equality->associated_type;
                typeck_status = cm_typeck_add_type(typeck, &projection,
                    &projection_type);
                if (typeck_status != CM_TYPECK_OK) {
                    result.status = cm_semantic_typeck_status(typeck_status);
                    result.typeck_status = typeck_status;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                memset(&goal, 0, sizeof(goal));
                goal.kind = CM_TRAIT_GOAL_PROJECTION_EQUALITY;
                goal.data.projection_equality.owner = owner;
                goal.data.projection_equality.projection_type =
                    projection_type;
                goal.data.projection_equality.expected_type = expected_type;
                selection = cm_semantic_session_solve_goal(session, typeck,
                    &environment_substitution, &goal);
                result.solver_kind = selection.kind;
                result.typeck_status = selection.typeck_status;
                result.status = cm_semantic_solver_status(selection.kind);
                if (result.status != CM_SEMANTIC_BODY_OK) {
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
                if ((selection.proof_origin == CM_TRAIT_PROOF_IMPL
                        && cm_hir_def_id_is_none(
                            selection.impl_associated_definition))
                    || (selection.proof_origin == CM_TRAIT_PROOF_PARAM_ENV
                        && (selection.param_env_fact_index
                                == CM_TRAIT_PROOF_FACT_NONE
                            || selection.param_env_equality_index
                                == CM_TRAIT_PROOF_EQUALITY_NONE))
                    || (selection.proof_origin != CM_TRAIT_PROOF_IMPL
                        && selection.proof_origin
                            != CM_TRAIT_PROOF_PARAM_ENV)) {
                    result.status = CM_SEMANTIC_BODY_INVALID;
                    result.solver_kind = CM_TRAIT_SOLVER_INVALID;
                    return cm_semantic_body_fail_snapshot(result, typeck,
                        &snapshot, call_expressions);
                }
            }
        }
    }
    result.status = cm_semantic_body_retry_equalities(&constraints);
    if (result.status == CM_SEMANTIC_BODY_OK) {
        result.status = cm_semantic_body_normalize_expressions(&constraints);
    }
    if (result.status == CM_SEMANTIC_BODY_OK) {
        result.status = cm_semantic_body_normalize_checked_facts(
            &constraints, &checked_facts);
    }
    if (result.status != CM_SEMANTIC_BODY_OK) {
        result.expression = constraints.failed_expression;
        result.callee = constraints.failed_callee;
        result.predicate_index = constraints.failed_predicate_index;
        result.typeck_status = constraints.typeck_status;
        result.solver_kind = constraints.solver_kind;
        return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
            call_expressions);
    }
    if (writeback != NULL && writeback->checked_body != NULL) {
        CmSemanticBodyWritebackStatus writeback_status;

        writeback_status = writeback->checked_body(writeback->context,
            session, body_id, &checked_facts);
        if (writeback_status != CM_SEMANTIC_BODY_WRITEBACK_OK) {
            result.status = writeback_status
                    == CM_SEMANTIC_BODY_WRITEBACK_OVERFLOW
                ? CM_SEMANTIC_BODY_OVERFLOW
                : writeback_status
                        == CM_SEMANTIC_BODY_WRITEBACK_DEFERRED_INFERENCE
                    ? CM_SEMANTIC_BODY_DEFERRED_INFERENCE
                : writeback_status
                        == CM_SEMANTIC_BODY_WRITEBACK_PENDING_PROJECTION
                    ? CM_SEMANTIC_BODY_PENDING_PROJECTION
                : writeback_status == CM_SEMANTIC_BODY_WRITEBACK_UNSUPPORTED
                    ? CM_SEMANTIC_BODY_UNSUPPORTED
                    : CM_SEMANTIC_BODY_INVALID;
            return cm_semantic_body_fail_snapshot(result, typeck, &snapshot,
                call_expressions);
        }
    }
    typeck_status = cm_typeck_commit(typeck, &snapshot);
    if (typeck_status != CM_TYPECK_OK) {
        result.status = CM_SEMANTIC_BODY_TYPECK_FAILURE;
        result.typeck_status = typeck_status;
        (void)cm_typeck_rollback(typeck, &snapshot);
        if (writeback != NULL && writeback->discard != NULL) {
            writeback->discard(writeback->context);
        }
        cm_free(call_expressions);
        cm_free(owner_arguments);
        cm_free(enclosing_arguments);
        cm_free(implemented_arguments);
        cm_free(callee_arguments);
        cm_free(expression_terms);
        for (call_index = 0u; call_index < checked_facts.call_count;
             ++call_index) {
            cm_free((void *)checked_facts.calls[call_index].parameter_types);
        }
        for (call_index = 0u; call_index < checked_facts.callable_count;
             ++call_index) {
            cm_semantic_body_callable_facts_clear(
                &checked_callables[call_index]);
        }
        cm_free(checked_calls);
        cm_free(checked_callables);
        cm_free(checked_adjustments);
        cm_free(checked_primitive_binaries);
        cm_free(checked_field_selections);
        cm_free(signature_parameter_types);
        cm_vec_destroy(&deferred_equalities);
        return result;
    }
    cm_free(call_expressions);
    cm_free(owner_arguments);
    cm_free(enclosing_arguments);
    cm_free(implemented_arguments);
    cm_free(callee_arguments);
    cm_free(expression_terms);
    for (call_index = 0u; call_index < checked_facts.call_count;
         ++call_index) {
        cm_free((void *)checked_facts.calls[call_index].parameter_types);
    }
    for (call_index = 0u; call_index < checked_facts.callable_count;
         ++call_index) {
        cm_semantic_body_callable_facts_clear(
            &checked_callables[call_index]);
    }
    cm_free(checked_calls);
    cm_free(checked_callables);
    cm_free(checked_adjustments);
    cm_free(checked_primitive_binaries);
    cm_free(checked_field_selections);
    cm_free(signature_parameter_types);
    cm_vec_destroy(&deferred_equalities);
    result = cm_semantic_body_result(CM_SEMANTIC_BODY_OK, body_id);
    result.solver_kind = CM_TRAIT_SOLVER_PROVEN;
    return result;
}

CmSemanticBodyResult cm_semantic_body_check_calls(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count)
{
    return cm_semantic_body_check_calls_mode(session, body,
        owner_type_substitutions, owner_type_substitution_count, NULL, NULL,
        0, NULL);
}

CmSemanticBodyResult cm_semantic_body_check_definition(
    CmSemanticSession *session, CmHirBodyId body)
{
    return cm_semantic_body_check_calls_mode(session, body, NULL, 0u, NULL,
        NULL, 1, NULL);
}

CmSemanticBodyResult cm_semantic_body_check_definition_with_writeback(
    CmSemanticSession *session, CmHirBodyId body,
    CmSemanticBodyWritebackFn writeback, void *writeback_context)
{
    if (writeback == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    {
        CmSemanticBodyEvidenceWriteback evidence;

        memset(&evidence, 0, sizeof(evidence));
        evidence.context = writeback_context;
        evidence.checked_body = writeback;
        return cm_semantic_body_check_calls_mode(session, body, NULL, 0u,
            NULL, NULL, 1, &evidence);
    }
}

CmSemanticBodyResult cm_semantic_body_check_instance_with_writeback(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count,
    CmSemanticBodyWritebackFn writeback, void *writeback_context)
{
    if (writeback == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    {
        CmSemanticBodyEvidenceWriteback evidence;

        memset(&evidence, 0, sizeof(evidence));
        evidence.context = writeback_context;
        evidence.checked_body = writeback;
        return cm_semantic_body_check_calls_mode(session, body,
            owner_type_substitutions, owner_type_substitution_count, NULL,
            NULL, 0, &evidence);
    }
}

CmSemanticBodyResult cm_semantic_body_check_definition_with_evidence(
    CmSemanticSession *session, CmHirBodyId body,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    if (writeback == NULL || writeback->checked_body == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    return cm_semantic_body_check_calls_mode(session, body, NULL, 0u, NULL,
        NULL, 1, writeback);
}

CmSemanticBodyResult cm_semantic_body_check_instance_with_evidence(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirTypeId *owner_type_substitutions,
    uint32_t owner_type_substitution_count,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    if (writeback == NULL || writeback->checked_body == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    return cm_semantic_body_check_calls_mode(session, body,
        owner_type_substitutions, owner_type_substitution_count, NULL, NULL,
        0, writeback);
}

CmSemanticBodyResult cm_semantic_body_check_instance_spec_with_evidence(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirInstanceSpec *spec,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    if (spec == NULL || writeback == NULL
        || writeback->checked_body == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    return cm_semantic_body_check_calls_mode(session, body, NULL, 0u, spec,
        NULL, 0, writeback);
}

CmSemanticBodyResult cm_semantic_body_check_instance_parts_with_evidence(
    CmSemanticSession *session, CmHirBodyId body,
    const CmHirCanonicalInstanceParts *parts,
    const CmSemanticBodyEvidenceWriteback *writeback)
{
    if (parts == NULL || writeback == NULL
        || writeback->checked_body == NULL) {
        return cm_semantic_body_result(CM_SEMANTIC_BODY_INVALID, body);
    }
    return cm_semantic_body_check_calls_mode(session, body, NULL, 0u, NULL,
        parts, 0, writeback);
}

const char *cm_semantic_body_status_name(CmSemanticBodyStatus status)
{
    switch (status) {
    case CM_SEMANTIC_BODY_OK: return "ok";
    case CM_SEMANTIC_BODY_PENDING_HIGHER_RANKED:
        return "pending-higher-ranked";
    case CM_SEMANTIC_BODY_PENDING_OUTLIVES: return "pending-outlives";
    case CM_SEMANTIC_BODY_PENDING_PROJECTION: return "pending-projection";
    case CM_SEMANTIC_BODY_PENDING_MODIFIER: return "pending-modifier";
    case CM_SEMANTIC_BODY_PENDING_SUBSTITUTION:
        return "pending-substitution";
    case CM_SEMANTIC_BODY_DEFERRED_INFERENCE: return "deferred-inference";
    case CM_SEMANTIC_BODY_DEFERRED_METADATA: return "deferred-metadata";
    case CM_SEMANTIC_BODY_AMBIGUOUS: return "ambiguous";
    case CM_SEMANTIC_BODY_NO_SOLUTION: return "no-solution";
    case CM_SEMANTIC_BODY_NEGATIVE: return "negative";
    case CM_SEMANTIC_BODY_UNSUPPORTED: return "unsupported";
    case CM_SEMANTIC_BODY_OVERFLOW: return "overflow";
    case CM_SEMANTIC_BODY_TYPECK_FAILURE: return "typeck-failure";
    case CM_SEMANTIC_BODY_STALE: return "stale";
    case CM_SEMANTIC_BODY_INVALID: return "invalid";
    }
    return "unknown";
}
