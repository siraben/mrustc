#include "cm/hir/typeck.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

#define CM_TYPECK_MAX_RECURSION 256u

typedef struct CmTypeckVariable {
    uint32_t parent;
    uint32_t canonical;
    uint32_t rank;
    CmTypeckTypeId term;
    CmTypeckTypeId binding;
    CmHirInferenceKind class_kind;
} CmTypeckVariable;

typedef struct CmTypeckStoredType {
    CmTypeckType type;
    CmHirTypeId source_hir_type;
    uint32_t late_bound_requirement;
} CmTypeckStoredType;

typedef struct CmTypeckTrailEntry {
    uint32_t variable;
    CmTypeckVariable old_value;
    CmHirInferenceKind old_term_class;
} CmTypeckTrailEntry;

typedef struct CmTypeckImportEntry {
    CmHirTypeId hir_type;
    CmTypeckTypeId type;
} CmTypeckImportEntry;

typedef struct CmTypeckSnapshotRecord {
    uint64_t snapshot_id;
    size_t type_count;
    size_t variable_count;
    size_t trail_count;
    size_t import_count;
    CmArenaMark storage_mark;
} CmTypeckSnapshotRecord;

typedef struct CmTypeckState {
    const CmHirContext *hir;
    CmArena storage;
    CmVec types;
    CmVec variables;
    CmVec trail;
    CmVec imports;
    CmVec snapshots;
    uint64_t lifetime_id;
    uint64_t state_revision;
    uint64_t rollback_generation;
    uint64_t next_snapshot_id;
    uint64_t hir_storage_lifetime_id;
    uint64_t hir_semantic_generation;
    uint64_t hir_rewind_generation;
    int track_hir_semantic_generation;
    size_t import_depth;
} CmTypeckState;

static uint64_t cm_typeck_lifetime_counter;
static uint64_t cm_typeck_state_revision_counter;

static uint64_t cm_typeck_new_lifetime_id(void)
{
    if (cm_typeck_lifetime_counter == UINT64_MAX) abort();
    cm_typeck_lifetime_counter += UINT64_C(1);
    return cm_typeck_lifetime_counter;
}

static void cm_typeck_record_state_mutation(CmTypeckState *state)
{
    if (state == NULL) return;
    if (cm_typeck_state_revision_counter == UINT64_MAX) abort();
    cm_typeck_state_revision_counter += UINT64_C(1);
    state->state_revision = cm_typeck_state_revision_counter;
}

static CmTypeckState *cm_typeck_state(CmTypeckContext *context)
{
    return context == NULL ? NULL : (CmTypeckState *)context->state;
}

static const CmTypeckState *cm_typeck_state_const(
    const CmTypeckContext *context)
{
    return context == NULL ? NULL : (const CmTypeckState *)context->state;
}

static int cm_typeck_state_is_current(const CmTypeckState *state)
{
    return state != NULL && state->hir != NULL
        && state->hir_storage_lifetime_id == state->hir->storage.lifetime_id
        && (!state->track_hir_semantic_generation
            || state->hir_semantic_generation
                == state->hir->semantic_generation)
        && state->hir_rewind_generation == state->hir->rewind_generation;
}

static int cm_typeck_span_valid(CmSpan span)
{
    return span.start <= span.end;
}

static const CmTypeckStoredType *cm_typeck_stored_type_const(
    const CmTypeckState *state, CmTypeckTypeId id)
{
    if (state == NULL || id == CM_TYPECK_TYPE_NONE) return NULL;
    return (const CmTypeckStoredType *)cm_vec_at_const(&state->types,
        (size_t)id - 1u);
}

static CmTypeckStoredType *cm_typeck_stored_type(CmTypeckState *state,
    CmTypeckTypeId id)
{
    if (state == NULL || id == CM_TYPECK_TYPE_NONE) return NULL;
    return (CmTypeckStoredType *)cm_vec_at(&state->types, (size_t)id - 1u);
}

static CmTypeckVariable *cm_typeck_variable(CmTypeckState *state,
    uint32_t id)
{
    if (state == NULL || id == 0u) return NULL;
    return (CmTypeckVariable *)cm_vec_at(&state->variables,
        (size_t)id - 1u);
}

static const CmTypeckVariable *cm_typeck_variable_const(
    const CmTypeckState *state, uint32_t id)
{
    if (state == NULL || id == 0u) return NULL;
    return (const CmTypeckVariable *)cm_vec_at_const(&state->variables,
        (size_t)id - 1u);
}

static int cm_typeck_region_valid(const CmTypeckState *state,
    const CmHirRegion *region)
{
    const CmHirGenericParam *parameter;

    if (region == NULL) return 0;
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        parameter = cm_hir_get_generic_param(state->hir,
            region->data.parameter);
        return parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_LIFETIME;
    case CM_HIR_REGION_LATE_BOUND:
        return 1;
    case CM_HIR_REGION_INFER:
    case CM_HIR_REGION_ERROR:
        return 0;
    }
    return 0;
}

static int cm_typeck_type_id_valid(const CmTypeckState *state,
    CmTypeckTypeId id)
{
    return cm_typeck_stored_type_const(state, id) != NULL;
}

static int cm_typeck_const_valid(const CmTypeckState *state,
    const CmTypeckConst *constant)
{
    const CmHirGenericParam *parameter;

    if (constant == NULL
        || !cm_typeck_type_id_valid(state, constant->type)) return 0;
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
        return 1;
    case CM_HIR_CONST_PARAMETER:
        parameter = cm_hir_get_generic_param(state->hir,
            constant->data.parameter);
        return parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST;
    case CM_HIR_CONST_UNEVALUATED:
    case CM_HIR_CONST_INFER:
    case CM_HIR_CONST_ERROR:
        return 0;
    }
    return 0;
}

static int cm_typeck_generic_args_valid(const CmTypeckState *state,
    const CmTypeckGenericArg *arguments, uint32_t count)
{
    uint32_t index;

    if ((count == 0u) != (arguments == NULL)) return 0;
    for (index = 0u; index < count; ++index) {
        switch (arguments[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            if (!cm_typeck_region_valid(state,
                    &arguments[index].data.lifetime)) return 0;
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            if (!cm_typeck_type_id_valid(state,
                    arguments[index].data.type)) return 0;
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            if (!cm_typeck_const_valid(state,
                    &arguments[index].data.constant)) return 0;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int cm_typeck_named_valid(const CmTypeckState *state,
    const CmTypeckNamedType *named)
{
    return named != NULL && !cm_hir_def_id_is_none(named->definition)
        && cm_typeck_generic_args_valid(state, named->arguments,
            named->argument_count);
}

static void *cm_typeck_copy_array(CmTypeckState *state, const void *items,
    size_t count, size_t item_size)
{
    void *copy;
    size_t bytes;

    if (count == 0u) return NULL;
    if (!cm_size_mul(count, item_size, &bytes)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    copy = cm_arena_alloc(&state->storage, bytes, 16u);
    memcpy(copy, items, bytes);
    return copy;
}

static int cm_typeck_requirement_merge_region(const CmHirRegion *region,
    uint32_t *requirement)
{
    uint32_t needed;

    if (region->kind != CM_HIR_REGION_LATE_BOUND) return 1;
    if (region->data.binder_index == UINT32_MAX) return 0;
    needed = region->data.binder_index + 1u;
    if (needed > *requirement) *requirement = needed;
    return 1;
}

static int cm_typeck_requirement_merge_type(const CmTypeckState *state,
    CmTypeckTypeId type, uint32_t *requirement)
{
    const CmTypeckStoredType *child;

    child = cm_typeck_stored_type_const(state, type);
    if (child == NULL) return 0;
    if (child->late_bound_requirement > *requirement) {
        *requirement = child->late_bound_requirement;
    }
    return 1;
}

static int cm_typeck_requirement_merge_named(const CmTypeckState *state,
    const CmTypeckNamedType *named, uint32_t *requirement)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        const CmTypeckGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            if (!cm_typeck_requirement_merge_region(
                    &argument->data.lifetime, requirement)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
            if (!cm_typeck_requirement_merge_type(state,
                    argument->data.type, requirement)) return 0;
        } else if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
            if (!cm_typeck_requirement_merge_type(state,
                    argument->data.constant.type, requirement)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int cm_typeck_type_requirement(const CmTypeckState *state,
    const CmTypeckType *type, uint32_t *out_requirement)
{
    uint32_t requirement;
    uint32_t index;

    requirement = 0u;
#define CM_TYPECK_REQUIRE_TYPE(child_id) do { \
        if (!cm_typeck_requirement_merge_type(state, (child_id), \
                &requirement)) return 0; \
    } while (0)
    switch (type->kind) {
    case CM_TYPECK_TYPE_REFERENCE:
        if (!cm_typeck_requirement_merge_region(
                &type->data.reference_type.region, &requirement)) return 0;
        CM_TYPECK_REQUIRE_TYPE(type->data.reference_type.pointee);
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        CM_TYPECK_REQUIRE_TYPE(type->data.raw_pointer_type.pointee);
        break;
    case CM_TYPECK_TYPE_TUPLE:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            CM_TYPECK_REQUIRE_TYPE(type->data.tuple_type.elements[index]);
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        CM_TYPECK_REQUIRE_TYPE(type->data.array_type.element);
        CM_TYPECK_REQUIRE_TYPE(type->data.array_type.length.type);
        break;
    case CM_TYPECK_TYPE_SLICE:
        CM_TYPECK_REQUIRE_TYPE(type->data.slice_type.element);
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            const CmTypeckStoredType *child = cm_typeck_stored_type_const(
                state, type->data.fn_pointer_type.parameters[index]);
            if (child == NULL || child->late_bound_requirement != 0u)
                return 0;
        }
        {
            const CmTypeckStoredType *child = cm_typeck_stored_type_const(
                state, type->data.fn_pointer_type.return_type);
            if (child == NULL || child->late_bound_requirement != 0u)
                return 0;
        }
        break;
    case CM_TYPECK_TYPE_ADT:
        if (!cm_typeck_requirement_merge_named(state,
                &type->data.named_type, &requirement)) return 0;
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        CM_TYPECK_REQUIRE_TYPE(type->data.projection_type.self_type);
        if (!cm_typeck_requirement_merge_named(state,
                &type->data.projection_type.trait_type, &requirement)
            || !cm_typeck_requirement_merge_named(state,
                &type->data.projection_type.associated_type, &requirement))
            return 0;
        break;
    default:
        break;
    }
#undef CM_TYPECK_REQUIRE_TYPE
    *out_requirement = requirement;
    return 1;
}

static CmTypeckStatus cm_typeck_add_stored(CmTypeckState *state,
    const CmTypeckType *type, CmHirTypeId source_hir_type,
    CmTypeckTypeId *out_type)
{
    CmTypeckStoredType stored;
    uint32_t late_bound_requirement;
    uint32_t index;
    int valid;

    if (!cm_typeck_state_is_current(state)
        || type == NULL || out_type == NULL
        || !cm_typeck_span_valid(type->span)
        || state->types.len >= (size_t)UINT32_MAX) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    *out_type = CM_TYPECK_TYPE_NONE;
    valid = 1;
    switch (type->kind) {
    case CM_TYPECK_TYPE_VARIABLE:
        return CM_TYPECK_INVALID_ARGUMENT;
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
        break;
    case CM_TYPECK_TYPE_INTEGER:
        valid = (unsigned int)type->data.integer_type
            <= (unsigned int)CM_HIR_INT_USIZE;
        break;
    case CM_TYPECK_TYPE_FLOAT:
        valid = (unsigned int)type->data.float_type
            <= (unsigned int)CM_HIR_FLOAT_F128;
        break;
    case CM_TYPECK_TYPE_REFERENCE:
        valid = cm_typeck_type_id_valid(state,
                type->data.reference_type.pointee)
            && cm_typeck_region_valid(state,
                &type->data.reference_type.region)
            && (unsigned int)type->data.reference_type.mutability
                <= (unsigned int)CM_HIR_MUTABLE;
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        valid = cm_typeck_type_id_valid(state,
                type->data.raw_pointer_type.pointee)
            && (unsigned int)type->data.raw_pointer_type.mutability
                <= (unsigned int)CM_HIR_MUTABLE;
        break;
    case CM_TYPECK_TYPE_TUPLE:
        valid = (type->data.tuple_type.element_count == 0u)
            == (type->data.tuple_type.elements == NULL);
        for (index = 0u; valid
             && index < type->data.tuple_type.element_count; ++index) {
            valid = cm_typeck_type_id_valid(state,
                type->data.tuple_type.elements[index]);
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        valid = cm_typeck_type_id_valid(state,
                type->data.array_type.element)
            && cm_typeck_const_valid(state, &type->data.array_type.length);
        break;
    case CM_TYPECK_TYPE_SLICE:
        valid = cm_typeck_type_id_valid(state,
            type->data.slice_type.element);
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        if (type->data.fn_pointer_type.binder_lifetime_count != 0u) {
            return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        }
        valid = (type->data.fn_pointer_type.parameter_count == 0u)
                == (type->data.fn_pointer_type.parameters == NULL)
            && cm_typeck_type_id_valid(state,
                type->data.fn_pointer_type.return_type)
            && cm_interner_get(&state->hir->strings,
                type->data.fn_pointer_type.abi) != NULL
            && (unsigned int)type->data.fn_pointer_type.safety
                <= (unsigned int)CM_HIR_UNSAFE
            && (type->data.fn_pointer_type.is_variadic == 0
                || type->data.fn_pointer_type.is_variadic == 1);
        for (index = 0u; valid
             && index < type->data.fn_pointer_type.parameter_count; ++index) {
            valid = cm_typeck_type_id_valid(state,
                type->data.fn_pointer_type.parameters[index]);
        }
        break;
    case CM_TYPECK_TYPE_ADT:
        valid = cm_typeck_named_valid(state, &type->data.named_type);
        break;
    case CM_TYPECK_TYPE_PARAMETER:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(state->hir,
            type->data.parameter_type.parameter);
        valid = parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE;
        break;
    }
    case CM_TYPECK_TYPE_PROJECTION:
        valid = cm_typeck_type_id_valid(state,
                type->data.projection_type.self_type)
            && cm_typeck_named_valid(state,
                &type->data.projection_type.trait_type)
            && cm_typeck_named_valid(state,
                &type->data.projection_type.associated_type);
        break;
    default:
        valid = 0;
        break;
    }
    if (!valid || !cm_typeck_type_requirement(state, type,
            &late_bound_requirement)) return CM_TYPECK_INVALID_ARGUMENT;

    memset(&stored, 0, sizeof(stored));
    stored.type = *type;
    stored.source_hir_type = source_hir_type;
    stored.late_bound_requirement = late_bound_requirement;
    if (type->kind == CM_TYPECK_TYPE_TUPLE) {
        stored.type.data.tuple_type.elements =
            (CmTypeckTypeId *)cm_typeck_copy_array(state,
                type->data.tuple_type.elements,
                type->data.tuple_type.element_count,
                sizeof(CmTypeckTypeId));
    } else if (type->kind == CM_TYPECK_TYPE_FN_POINTER) {
        stored.type.data.fn_pointer_type.parameters =
            (CmTypeckTypeId *)cm_typeck_copy_array(state,
                type->data.fn_pointer_type.parameters,
                type->data.fn_pointer_type.parameter_count,
                sizeof(CmTypeckTypeId));
    } else if (type->kind == CM_TYPECK_TYPE_ADT) {
        stored.type.data.named_type.arguments =
            (CmTypeckGenericArg *)cm_typeck_copy_array(state,
                type->data.named_type.arguments,
                type->data.named_type.argument_count,
                sizeof(CmTypeckGenericArg));
    } else if (type->kind == CM_TYPECK_TYPE_PROJECTION) {
        stored.type.data.projection_type.trait_type.arguments =
            (CmTypeckGenericArg *)cm_typeck_copy_array(state,
                type->data.projection_type.trait_type.arguments,
                type->data.projection_type.trait_type.argument_count,
                sizeof(CmTypeckGenericArg));
        stored.type.data.projection_type.associated_type.arguments =
            (CmTypeckGenericArg *)cm_typeck_copy_array(state,
                type->data.projection_type.associated_type.arguments,
                type->data.projection_type.associated_type.argument_count,
                sizeof(CmTypeckGenericArg));
    }
    (void)cm_vec_push(&state->types, &stored);
    *out_type = (CmTypeckTypeId)state->types.len;
    return CM_TYPECK_OK;
}

void cm_typeck_context_init(CmTypeckContext *context,
    const CmHirContext *hir)
{
    CmTypeckState *state;

    if (context == NULL) return;
    context->state = NULL;
    if (hir == NULL) return;
    state = (CmTypeckState *)cm_alloc_zeroed(1u, sizeof(CmTypeckState));
    state->hir = hir;
    cm_arena_init(&state->storage, 4096u);
    cm_vec_init(&state->types, sizeof(CmTypeckStoredType));
    cm_vec_init(&state->variables, sizeof(CmTypeckVariable));
    cm_vec_init(&state->trail, sizeof(CmTypeckTrailEntry));
    cm_vec_init(&state->imports, sizeof(CmTypeckImportEntry));
    cm_vec_init(&state->snapshots, sizeof(CmTypeckSnapshotRecord));
    state->lifetime_id = cm_typeck_new_lifetime_id();
    state->rollback_generation = UINT64_C(1);
    cm_typeck_record_state_mutation(state);
    state->next_snapshot_id = UINT64_C(1);
    state->hir_storage_lifetime_id = hir->storage.lifetime_id;
    state->hir_semantic_generation = hir->semantic_generation;
    state->hir_rewind_generation = hir->rewind_generation;
    context->state = state;
}

void cm_typeck_context_track_hir_semantic_generation(
    CmTypeckContext *context)
{
    CmTypeckState *state;

    state = cm_typeck_state(context);
    if (state == NULL || !cm_typeck_state_is_current(state)) return;
    state->hir_semantic_generation = state->hir->semantic_generation;
    state->track_hir_semantic_generation = 1;
}

void cm_typeck_context_destroy(CmTypeckContext *context)
{
    CmTypeckState *state;

    state = cm_typeck_state(context);
    if (state == NULL) return;
    cm_vec_destroy(&state->snapshots);
    cm_vec_destroy(&state->imports);
    cm_vec_destroy(&state->trail);
    cm_vec_destroy(&state->variables);
    cm_vec_destroy(&state->types);
    cm_arena_destroy(&state->storage);
    memset(state, 0, sizeof(*state));
    cm_free(state);
    context->state = NULL;
}

void cm_typeck_instantiation_init(const CmTypeckContext *context,
    CmTypeckInstantiation *instantiation)
{
    const CmTypeckState *state;

    if (instantiation == NULL) return;
    memset(instantiation, 0, sizeof(*instantiation));
    state = cm_typeck_state_const(context);
    if (!cm_typeck_state_is_current(state)) return;
    instantiation->typeck_state = state;
    instantiation->typeck_lifetime_id = state->lifetime_id;
}

void cm_typeck_scoped_instantiation_init(const CmTypeckContext *context,
    CmTypeckScopedInstantiation *instantiation)
{
    const CmTypeckState *state;

    if (instantiation == NULL) return;
    memset(instantiation, 0, sizeof(*instantiation));
    state = cm_typeck_state_const(context);
    if (!cm_typeck_state_is_current(state)) return;
    instantiation->typeck_state = state;
    instantiation->typeck_lifetime_id = state->lifetime_id;
    instantiation->typeck_rollback_generation = state->rollback_generation;
}

CmTypeckStatus cm_typeck_snapshot(CmTypeckContext *context,
    CmTypeckSnapshot *out_snapshot)
{
    CmTypeckState *state;
    CmTypeckSnapshotRecord record;

    state = cm_typeck_state(context);
    if (!cm_typeck_state_is_current(state) || out_snapshot == NULL) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    memset(&record, 0, sizeof(record));
    record.snapshot_id = state->next_snapshot_id++;
    if (record.snapshot_id == 0u) record.snapshot_id =
        state->next_snapshot_id++;
    record.type_count = state->types.len;
    record.variable_count = state->variables.len;
    record.trail_count = state->trail.len;
    record.import_count = state->imports.len;
    record.storage_mark = cm_arena_mark(&state->storage);
    (void)cm_vec_push(&state->snapshots, &record);
    out_snapshot->owner = context;
    out_snapshot->lifetime_id = state->lifetime_id;
    out_snapshot->snapshot_id = record.snapshot_id;
    out_snapshot->active = 1;
    return CM_TYPECK_OK;
}

static CmTypeckSnapshotRecord *cm_typeck_snapshot_record(
    CmTypeckState *state, const CmTypeckContext *context,
    const CmTypeckSnapshot *snapshot)
{
    CmTypeckSnapshotRecord *record;

    if (state == NULL || snapshot == NULL || !snapshot->active
        || snapshot->owner != context
        || snapshot->lifetime_id != state->lifetime_id
        || state->snapshots.len == 0u) return NULL;
    record = (CmTypeckSnapshotRecord *)cm_vec_at(&state->snapshots,
        state->snapshots.len - 1u);
    return record != NULL
            && record->snapshot_id == snapshot->snapshot_id
            && cm_arena_mark_is_valid(&state->storage,
                record->storage_mark)
        ? record : NULL;
}

CmTypeckStatus cm_typeck_rollback(CmTypeckContext *context,
    CmTypeckSnapshot *snapshot)
{
    CmTypeckState *state;
    CmTypeckSnapshotRecord *record;
    CmTypeckSnapshotRecord stable;

    state = cm_typeck_state(context);
    record = cm_typeck_snapshot_record(state, context, snapshot);
    if (record == NULL) return CM_TYPECK_INVALID_SNAPSHOT;
    stable = *record;
    while (state->trail.len > stable.trail_count) {
        CmTypeckTrailEntry entry;
        CmTypeckVariable *variable;

        (void)cm_vec_pop(&state->trail, &entry);
        variable = cm_typeck_variable(state, entry.variable);
        if (variable != NULL) {
            CmTypeckStoredType *term;

            *variable = entry.old_value;
            term = cm_typeck_stored_type(state, variable->term);
            if (term != NULL
                && term->type.kind == CM_TYPECK_TYPE_VARIABLE) {
                term->type.data.variable.class_kind = entry.old_term_class;
            }
        }
    }
    cm_vec_resize(&state->imports, stable.import_count);
    cm_vec_resize(&state->variables, stable.variable_count);
    cm_vec_resize(&state->types, stable.type_count);
    cm_arena_rewind(&state->storage, stable.storage_mark);
    cm_arena_discard_mark(&state->storage, stable.storage_mark);
    cm_vec_resize(&state->snapshots, state->snapshots.len - 1u);
    snapshot->active = 0;
    snapshot->owner = NULL;
    if (state->rollback_generation == UINT64_MAX) abort();
    state->rollback_generation += UINT64_C(1);
    cm_typeck_record_state_mutation(state);
    return CM_TYPECK_OK;
}

CmTypeckStatus cm_typeck_commit(CmTypeckContext *context,
    CmTypeckSnapshot *snapshot)
{
    CmTypeckState *state;
    CmTypeckSnapshotRecord *record;
    CmArenaMark mark;

    state = cm_typeck_state(context);
    record = cm_typeck_snapshot_record(state, context, snapshot);
    if (record == NULL) return CM_TYPECK_INVALID_SNAPSHOT;
    mark = record->storage_mark;
    cm_arena_discard_mark(&state->storage, mark);
    cm_vec_resize(&state->snapshots, state->snapshots.len - 1u);
    if (state->snapshots.len == 0u) cm_vec_clear(&state->trail);
    snapshot->active = 0;
    snapshot->owner = NULL;
    return CM_TYPECK_OK;
}

CmTypeckStatus cm_typeck_add_type(CmTypeckContext *context,
    const CmTypeckType *type, CmTypeckTypeId *out_type)
{
    return cm_typeck_add_stored(cm_typeck_state(context), type,
        CM_HIR_TYPE_NONE, out_type);
}

CmTypeckStatus cm_typeck_new_variable(CmTypeckContext *context,
    CmHirInferenceKind class_kind, CmSpan span,
    CmTypeckTypeId *out_type)
{
    CmTypeckState *state;
    CmTypeckStoredType stored;
    CmTypeckVariable variable;
    uint32_t variable_id;

    state = cm_typeck_state(context);
    if (!cm_typeck_state_is_current(state)
        || out_type == NULL || !cm_typeck_span_valid(span)
        || (unsigned int)class_kind > (unsigned int)CM_HIR_INFER_FLOAT
        || state->types.len >= (size_t)UINT32_MAX
        || state->variables.len >= (size_t)UINT32_MAX) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    *out_type = CM_TYPECK_TYPE_NONE;
    variable_id = (uint32_t)state->variables.len + 1u;
    memset(&stored, 0, sizeof(stored));
    stored.type.kind = CM_TYPECK_TYPE_VARIABLE;
    stored.type.span = span;
    stored.type.data.variable.variable = variable_id;
    stored.type.data.variable.class_kind = class_kind;
    (void)cm_vec_push(&state->types, &stored);
    memset(&variable, 0, sizeof(variable));
    variable.parent = variable_id;
    variable.canonical = variable_id;
    variable.term = (CmTypeckTypeId)state->types.len;
    variable.class_kind = class_kind;
    (void)cm_vec_push(&state->variables, &variable);
    *out_type = variable.term;
    return CM_TYPECK_OK;
}

static uint32_t cm_typeck_root_variable(const CmTypeckState *state,
    uint32_t variable)
{
    size_t depth;

    for (depth = 0u; depth <= state->variables.len; ++depth) {
        const CmTypeckVariable *value;

        value = cm_typeck_variable_const(state, variable);
        if (value == NULL) return 0u;
        if (value->parent == variable) return variable;
        variable = value->parent;
    }
    return 0u;
}

static CmTypeckStatus cm_typeck_resolve_internal(
    const CmTypeckState *state, CmTypeckTypeId type,
    CmTypeckTypeId *out_type, uint32_t *out_variable)
{
    size_t depth;

    if (out_type != NULL) *out_type = CM_TYPECK_TYPE_NONE;
    if (out_variable != NULL) *out_variable = 0u;
    for (depth = 0u; depth <= state->types.len + state->variables.len;
         ++depth) {
        const CmTypeckStoredType *stored;

        stored = cm_typeck_stored_type_const(state, type);
        if (stored == NULL) return CM_TYPECK_INVALID_ID;
        if (stored->type.kind != CM_TYPECK_TYPE_VARIABLE) {
            if (out_type != NULL) *out_type = type;
            return CM_TYPECK_OK;
        }
        {
            uint32_t root;
            const CmTypeckVariable *variable;

            root = cm_typeck_root_variable(state,
                stored->type.data.variable.variable);
            variable = cm_typeck_variable_const(state, root);
            if (variable == NULL) return CM_TYPECK_INVALID_ID;
            if (variable->binding == CM_TYPECK_TYPE_NONE) {
                const CmTypeckVariable *canonical;

                canonical = cm_typeck_variable_const(state,
                    variable->canonical);
                if (canonical == NULL) return CM_TYPECK_INVALID_ID;
                if (out_type != NULL) *out_type = canonical->term;
                if (out_variable != NULL) *out_variable = root;
                return CM_TYPECK_OK;
            }
            type = variable->binding;
        }
    }
    return CM_TYPECK_OCCURS_CHECK;
}

CmTypeckStatus cm_typeck_resolve(const CmTypeckContext *context,
    CmTypeckTypeId type, CmTypeckTypeId *out_type)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    if (!cm_typeck_state_is_current(state) || out_type == NULL) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    return cm_typeck_resolve_internal(state, type, out_type, NULL);
}

const CmTypeckType *cm_typeck_get_type(const CmTypeckContext *context,
    CmTypeckTypeId type)
{
    const CmTypeckStoredType *stored;

    if (!cm_typeck_state_is_current(cm_typeck_state_const(context))) {
        return NULL;
    }
    stored = cm_typeck_stored_type_const(cm_typeck_state_const(context), type);
    return stored == NULL ? NULL : &stored->type;
}

size_t cm_typeck_type_count(const CmTypeckContext *context)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return !cm_typeck_state_is_current(state) ? 0u : state->types.len;
}

uint64_t cm_typeck_lifetime_id(const CmTypeckContext *context)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return !cm_typeck_state_is_current(state) ? 0u : state->lifetime_id;
}

uint64_t cm_typeck_state_revision(const CmTypeckContext *context)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return !cm_typeck_state_is_current(state) ? 0u : state->state_revision;
}

const CmHirContext *cm_typeck_hir_context(const CmTypeckContext *context)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return !cm_typeck_state_is_current(state) ? NULL : state->hir;
}

int cm_typeck_adt_is_valid(const CmTypeckContext *context,
    const CmTypeckNamedType *adt)
{
    const CmTypeckState *state;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    uint32_t index;

    state = cm_typeck_state_const(context);
    if (!cm_typeck_state_is_current(state)
        || !cm_typeck_named_valid(state, adt)) return 0;
    definition = cm_hir_lookup_definition(state->hir, adt->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM) {
        return 0;
    }
    item = cm_hir_get_item(state->hir, definition->entity.item_id);
    if (item == NULL || (item->kind != CM_HIR_ITEM_STRUCT
            && item->kind != CM_HIR_ITEM_ENUM
            && item->kind != CM_HIR_ITEM_UNION)
        || item->generic_parameter_count != adt->argument_count) return 0;
    for (index = 0u; index < adt->argument_count; ++index) {
        const CmHirGenericParam *parameter;
        CmHirGenericArgKind expected;

        parameter = cm_hir_get_generic_param(state->hir,
            item->generic_parameter_start + index);
        expected = parameter == NULL ? (CmHirGenericArgKind)-1
            : parameter->kind == CM_HIR_GENERIC_LIFETIME
                ? CM_HIR_GENERIC_ARG_LIFETIME
                : parameter->kind == CM_HIR_GENERIC_TYPE
                    ? CM_HIR_GENERIC_ARG_TYPE : CM_HIR_GENERIC_ARG_CONST;
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || adt->arguments[index].kind != expected) return 0;
    }
    return 1;
}

static CmTypeckStatus cm_typeck_import_type_inner_impl(CmTypeckState *state,
    CmHirTypeId hir_type, CmTypeckTypeId *out_type);

static CmTypeckStatus cm_typeck_import_type_inner(CmTypeckState *state,
    CmHirTypeId hir_type, CmTypeckTypeId *out_type)
{
    CmTypeckStatus status;

    if (state->import_depth >= CM_TYPECK_MAX_RECURSION) {
        return CM_TYPECK_OVERFLOW;
    }
    state->import_depth += 1u;
    status = cm_typeck_import_type_inner_impl(state, hir_type, out_type);
    state->import_depth -= 1u;
    return status;
}

static CmTypeckStatus cm_typeck_import_const(CmTypeckState *state,
    const CmHirConstArg *source, CmTypeckConst *out)
{
    CmTypeckStatus status;

    memset(out, 0, sizeof(*out));
    if (source->kind != CM_HIR_CONST_VALUE
        && source->kind != CM_HIR_CONST_PARAMETER) {
        return CM_TYPECK_UNSUPPORTED_CONSTANT;
    }
    status = cm_typeck_import_type_inner(state, source->type, &out->type);
    if (status != CM_TYPECK_OK) return status;
    out->kind = source->kind;
    if (source->kind == CM_HIR_CONST_VALUE) {
        out->data.value.low_bits = source->data.value.low_bits;
        out->data.value.high_bits = source->data.value.high_bits;
    } else {
        out->data.parameter = source->data.parameter;
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_import_args(CmTypeckState *state,
    const CmHirGenericArg *source, uint32_t count,
    CmTypeckGenericArg **out_arguments)
{
    CmTypeckGenericArg *arguments;
    CmTypeckStatus status;
    uint32_t index;

    *out_arguments = NULL;
    if (count == 0u) return source == NULL
        ? CM_TYPECK_OK : CM_TYPECK_INVALID_ARGUMENT;
    if (source == NULL) return CM_TYPECK_INVALID_ARGUMENT;
    arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(count,
        sizeof(CmTypeckGenericArg));
    status = CM_TYPECK_OK;
    for (index = 0u; index < count; ++index) {
        arguments[index].kind = source[index].kind;
        switch (source[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            if (!cm_typeck_region_valid(state,
                    &source[index].data.lifetime)) {
                status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
            } else {
                arguments[index].data.lifetime =
                    source[index].data.lifetime;
            }
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            status = cm_typeck_import_type_inner(state,
                source[index].data.type, &arguments[index].data.type);
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            status = cm_typeck_import_const(state,
                &source[index].data.constant,
                &arguments[index].data.constant);
            break;
        default:
            status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
            break;
        }
        if (status != CM_TYPECK_OK) break;
    }
    if (status != CM_TYPECK_OK) {
        cm_free(arguments);
        return status;
    }
    *out_arguments = arguments;
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_import_named(CmTypeckState *state,
    const CmHirNamedType *source, CmTypeckNamedType *out)
{
    CmTypeckStatus status;

    memset(out, 0, sizeof(*out));
    out->definition = source->definition;
    out->argument_count = source->argument_count;
    status = cm_typeck_import_args(state, source->arguments,
        source->argument_count, &out->arguments);
    return status;
}

static void cm_typeck_free_temp_type(CmTypeckType *type)
{
    if (type->kind == CM_TYPECK_TYPE_TUPLE) {
        cm_free(type->data.tuple_type.elements);
    } else if (type->kind == CM_TYPECK_TYPE_FN_POINTER) {
        cm_free(type->data.fn_pointer_type.parameters);
    } else if (type->kind == CM_TYPECK_TYPE_ADT) {
        cm_free(type->data.named_type.arguments);
    } else if (type->kind == CM_TYPECK_TYPE_PROJECTION) {
        cm_free(type->data.projection_type.trait_type.arguments);
        cm_free(type->data.projection_type.associated_type.arguments);
    }
}

static CmTypeckStatus cm_typeck_import_type_inner_impl(CmTypeckState *state,
    CmHirTypeId hir_type, CmTypeckTypeId *out_type)
{
    const CmHirType *source;
    CmTypeckType type;
    CmTypeckStatus status;
    CmTypeckImportEntry entry;
    size_t cache_index;
    uint32_t index;

    for (cache_index = 0u; cache_index < state->imports.len;
         ++cache_index) {
        const CmTypeckImportEntry *cached;

        cached = (const CmTypeckImportEntry *)cm_vec_at_const(
            &state->imports, cache_index);
        if (cached->hir_type == hir_type) {
            *out_type = cached->type;
            return CM_TYPECK_OK;
        }
    }
    source = cm_hir_get_type(state->hir, hir_type);
    if (source == NULL) return CM_TYPECK_INVALID_ID;
    memset(&type, 0, sizeof(type));
    type.span = source->span;
    status = CM_TYPECK_OK;
    switch (source->kind) {
    case CM_HIR_TYPE_NEVER_KIND: type.kind = CM_TYPECK_TYPE_NEVER; break;
    case CM_HIR_TYPE_UNIT_KIND: type.kind = CM_TYPECK_TYPE_UNIT; break;
    case CM_HIR_TYPE_BOOL_KIND: type.kind = CM_TYPECK_TYPE_BOOL; break;
    case CM_HIR_TYPE_CHAR_KIND: type.kind = CM_TYPECK_TYPE_CHAR; break;
    case CM_HIR_TYPE_STR_KIND: type.kind = CM_TYPECK_TYPE_STR; break;
    case CM_HIR_TYPE_INTEGER_KIND:
        type.kind = CM_TYPECK_TYPE_INTEGER;
        type.data.integer_type = source->data.integer_type.kind;
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        type.kind = CM_TYPECK_TYPE_FLOAT;
        type.data.float_type = source->data.float_type.kind;
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        type.kind = CM_TYPECK_TYPE_REFERENCE;
        if (!cm_typeck_region_valid(state,
                &source->data.reference_type.region)) {
            status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
            break;
        }
        type.data.reference_type.region = source->data.reference_type.region;
        type.data.reference_type.mutability =
            source->data.reference_type.mutability;
        status = cm_typeck_import_type_inner(state,
            source->data.reference_type.pointee,
            &type.data.reference_type.pointee);
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        type.kind = CM_TYPECK_TYPE_RAW_POINTER;
        type.data.raw_pointer_type.mutability =
            source->data.raw_pointer_type.mutability;
        status = cm_typeck_import_type_inner(state,
            source->data.raw_pointer_type.pointee,
            &type.data.raw_pointer_type.pointee);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        type.kind = CM_TYPECK_TYPE_TUPLE;
        type.data.tuple_type.element_count =
            source->data.tuple_type.element_count;
        if (type.data.tuple_type.element_count != 0u) {
            type.data.tuple_type.elements =
                (CmTypeckTypeId *)cm_alloc_zeroed(
                    type.data.tuple_type.element_count,
                    sizeof(CmTypeckTypeId));
        }
        for (index = 0u; index < type.data.tuple_type.element_count;
             ++index) {
            status = cm_typeck_import_type_inner(state,
                source->data.tuple_type.elements[index],
                &type.data.tuple_type.elements[index]);
            if (status != CM_TYPECK_OK) break;
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        type.kind = CM_TYPECK_TYPE_ARRAY;
        status = cm_typeck_import_type_inner(state,
            source->data.array_type.element, &type.data.array_type.element);
        if (status == CM_TYPECK_OK) status = cm_typeck_import_const(state,
            &source->data.array_type.length, &type.data.array_type.length);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        type.kind = CM_TYPECK_TYPE_SLICE;
        status = cm_typeck_import_type_inner(state,
            source->data.slice_type.element, &type.data.slice_type.element);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (source->data.fn_pointer_type.binder.lifetime_count != 0u) {
            status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
            break;
        }
        type.kind = CM_TYPECK_TYPE_FN_POINTER;
        type.data.fn_pointer_type.parameter_count =
            source->data.fn_pointer_type.parameter_count;
        if (type.data.fn_pointer_type.parameter_count != 0u) {
            type.data.fn_pointer_type.parameters =
                (CmTypeckTypeId *)cm_alloc_zeroed(
                    type.data.fn_pointer_type.parameter_count,
                    sizeof(CmTypeckTypeId));
        }
        for (index = 0u;
             index < type.data.fn_pointer_type.parameter_count; ++index) {
            status = cm_typeck_import_type_inner(state,
                source->data.fn_pointer_type.parameters[index],
                &type.data.fn_pointer_type.parameters[index]);
            if (status != CM_TYPECK_OK) break;
        }
        if (status == CM_TYPECK_OK) status = cm_typeck_import_type_inner(
            state, source->data.fn_pointer_type.return_type,
            &type.data.fn_pointer_type.return_type);
        type.data.fn_pointer_type.abi = source->data.fn_pointer_type.abi;
        type.data.fn_pointer_type.safety =
            source->data.fn_pointer_type.safety;
        type.data.fn_pointer_type.is_variadic =
            source->data.fn_pointer_type.is_variadic;
        break;
    case CM_HIR_TYPE_ADT_KIND:
        type.kind = CM_TYPECK_TYPE_ADT;
        status = cm_typeck_import_named(state, &source->data.named_type,
            &type.data.named_type);
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
        type.kind = CM_TYPECK_TYPE_PARAMETER;
        type.data.parameter_type.parameter =
            source->data.parameter_type.parameter;
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        type.kind = CM_TYPECK_TYPE_PROJECTION;
        status = cm_typeck_import_type_inner(state,
            source->data.projection_type.self_type,
            &type.data.projection_type.self_type);
        if (status == CM_TYPECK_OK) status = cm_typeck_import_named(state,
            &source->data.projection_type.trait_type,
            &type.data.projection_type.trait_type);
        if (status == CM_TYPECK_OK) status = cm_typeck_import_named(state,
            &source->data.projection_type.associated_type,
            &type.data.projection_type.associated_type);
        break;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    default:
        status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        break;
    }
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_add_stored(state, &type, hir_type, out_type);
    }
    cm_typeck_free_temp_type(&type);
    if (status != CM_TYPECK_OK) return status;
    entry.hir_type = hir_type;
    entry.type = *out_type;
    (void)cm_vec_push(&state->imports, &entry);
    return CM_TYPECK_OK;
}

CmTypeckStatus cm_typeck_import_hir_type(CmTypeckContext *context,
    CmHirTypeId hir_type, CmTypeckTypeId *out_type)
{
    CmTypeckState *state;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus status;

    state = cm_typeck_state(context);
    if (!cm_typeck_state_is_current(state) || out_type == NULL) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    *out_type = CM_TYPECK_TYPE_NONE;
    status = cm_typeck_snapshot(context, &snapshot);
    if (status != CM_TYPECK_OK) return status;
    status = cm_typeck_import_type_inner(state, hir_type, out_type);
    if (status == CM_TYPECK_OK) {
        (void)cm_typeck_commit(context, &snapshot);
    } else {
        (void)cm_typeck_rollback(context, &snapshot);
        *out_type = CM_TYPECK_TYPE_NONE;
    }
    return status;
}

typedef struct CmTypeckInstantiationState {
    CmTypeckState *types;
    const CmTypeckScopedInstantiation *instantiation;
    CmVec memo;
    size_t depth;
} CmTypeckInstantiationState;

static const CmHirGenericParam *cm_typeck_instantiation_parameter(
    const CmTypeckState *state, CmHirDefId owner, uint32_t index)
{
    const CmHirGenericParam *matched;
    size_t parameter_index;

    matched = NULL;
    for (parameter_index = 0u;
         parameter_index < state->hir->generic_parameters.len;
         ++parameter_index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &state->hir->generic_parameters, parameter_index);
        if (parameter != NULL && parameter->index == index
            && cm_hir_def_id_equal(parameter->owner, owner)) {
            if (matched != NULL) return NULL;
            matched = parameter;
        }
    }
    return matched;
}

static int cm_typeck_instantiation_arg_matches(
    const CmHirGenericParam *parameter,
    const CmTypeckGenericArg *argument)
{
    if (parameter == NULL || argument == NULL) return 0;
    switch (parameter->kind) {
    case CM_HIR_GENERIC_LIFETIME:
        return argument->kind == CM_HIR_GENERIC_ARG_LIFETIME;
    case CM_HIR_GENERIC_TYPE:
        return argument->kind == CM_HIR_GENERIC_ARG_TYPE;
    case CM_HIR_GENERIC_CONST:
        return argument->kind == CM_HIR_GENERIC_ARG_CONST;
    }
    return 0;
}

static int cm_typeck_instantiation_frame_valid(const CmTypeckState *state,
    const CmTypeckInstantiationFrame *frame)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    uint32_t index;

    if (frame == NULL || cm_hir_def_id_is_none(frame->parameter_owner)
        || (frame->argument_count == 0u) != (frame->arguments == NULL)
        || !cm_typeck_generic_args_valid(state, frame->arguments,
            frame->argument_count)) return 0;
    definition = cm_hir_lookup_definition(state->hir,
        frame->parameter_owner);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM) {
        return 0;
    }
    item = definition->state == CM_HIR_DEFINITION_BOUND
        ? cm_hir_get_item(state->hir, definition->entity.item_id) : NULL;
    if (item != NULL) {
        if (!cm_hir_def_id_equal(item->definition,
                frame->parameter_owner)
            || item->generic_parameter_count != frame->argument_count
            || (item->generic_parameter_count == 0u)
                != (item->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE)) return 0;
    } else if (definition->state != CM_HIR_DEFINITION_RESERVED) {
        return 0;
    }
    for (index = 0u; index < frame->argument_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = item == NULL
            ? cm_typeck_instantiation_parameter(state,
                frame->parameter_owner, index)
            : cm_hir_get_generic_param(state->hir,
                item->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                frame->parameter_owner)) return 0;
        if (!cm_typeck_instantiation_arg_matches(
                parameter, &frame->arguments[index])) return 0;
    }
    if (item == NULL) {
        size_t parameter_index;
        size_t owned_count;

        owned_count = 0u;
        for (parameter_index = 0u;
             parameter_index < state->hir->generic_parameters.len;
             ++parameter_index) {
            const CmHirGenericParam *parameter;

            parameter = (const CmHirGenericParam *)cm_vec_at_const(
                &state->hir->generic_parameters, parameter_index);
            if (parameter != NULL && cm_hir_def_id_equal(parameter->owner,
                    frame->parameter_owner)) owned_count += 1u;
        }
        if (owned_count != (size_t)frame->argument_count) return 0;
    }
    return 1;
}

static int cm_typeck_scoped_instantiation_valid_inner(
    const CmTypeckState *state,
    const CmTypeckScopedInstantiation *instantiation)
{
    const CmHirDefinition *definition;
    uint32_t frame_index;

    if (instantiation == NULL || instantiation->typeck_state != state
        || instantiation->typeck_lifetime_id != state->lifetime_id
        || instantiation->typeck_rollback_generation
            != state->rollback_generation
        || (instantiation->frame_count == 0u)
            != (instantiation->frames == NULL)
        || (size_t)instantiation->frame_count
            > state->hir->definitions.len) return 0;
    for (frame_index = 0u; frame_index < instantiation->frame_count;
         ++frame_index) {
        uint32_t prior_index;

        if (!cm_typeck_instantiation_frame_valid(state,
                &instantiation->frames[frame_index])) return 0;
        for (prior_index = 0u; prior_index < frame_index; ++prior_index) {
            if (cm_hir_def_id_equal(
                    instantiation->frames[prior_index].parameter_owner,
                    instantiation->frames[frame_index].parameter_owner)) {
                return 0;
            }
        }
    }
    if (instantiation->self_type == CM_TYPECK_TYPE_NONE) {
        return cm_hir_def_id_is_none(instantiation->self_owner);
    }
    definition = cm_hir_lookup_definition(state->hir,
        instantiation->self_owner);
    return !cm_hir_def_id_is_none(instantiation->self_owner)
        && definition != NULL && definition->kind == CM_HIR_DEFINITION_ITEM
        && cm_typeck_type_id_valid(state, instantiation->self_type);
}

static int cm_typeck_instantiation_valid_inner(const CmTypeckState *state,
    const CmTypeckInstantiation *instantiation)
{
    CmTypeckInstantiationFrame frame;
    CmTypeckScopedInstantiation scoped;

    if (instantiation == NULL
        || cm_hir_def_id_is_none(instantiation->parameter_owner)) return 0;
    memset(&frame, 0, sizeof(frame));
    frame.parameter_owner = instantiation->parameter_owner;
    frame.arguments = instantiation->arguments;
    frame.argument_count = instantiation->argument_count;
    memset(&scoped, 0, sizeof(scoped));
    scoped.typeck_state = instantiation->typeck_state;
    scoped.typeck_lifetime_id = instantiation->typeck_lifetime_id;
    scoped.typeck_rollback_generation = state->rollback_generation;
    scoped.frames = &frame;
    scoped.frame_count = 1u;
    scoped.self_owner = instantiation->self_owner;
    scoped.self_type = instantiation->self_type;
    return cm_typeck_scoped_instantiation_valid_inner(state, &scoped);
}

int cm_typeck_instantiation_is_valid(const CmTypeckContext *context,
    const CmTypeckInstantiation *instantiation)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return cm_typeck_state_is_current(state)
        && cm_typeck_instantiation_valid_inner(state, instantiation);
}

int cm_typeck_scoped_instantiation_is_valid(
    const CmTypeckContext *context,
    const CmTypeckScopedInstantiation *instantiation)
{
    const CmTypeckState *state;

    state = cm_typeck_state_const(context);
    return cm_typeck_state_is_current(state)
        && cm_typeck_scoped_instantiation_valid_inner(state,
            instantiation);
}

static const CmTypeckInstantiationFrame *cm_typeck_instantiation_frame(
    const CmTypeckInstantiationState *instantiate, CmHirDefId owner)
{
    uint32_t index;

    for (index = 0u; index < instantiate->instantiation->frame_count;
         ++index) {
        const CmTypeckInstantiationFrame *frame;

        frame = &instantiate->instantiation->frames[index];
        if (cm_hir_def_id_equal(frame->parameter_owner, owner)) {
            return frame;
        }
    }
    return NULL;
}

static CmTypeckStatus cm_typeck_instantiate_type_inner(
    CmTypeckInstantiationState *instantiate, CmHirTypeId hir_type,
    CmTypeckTypeId *out_type);

static CmTypeckStatus cm_typeck_instantiate_region(
    CmTypeckInstantiationState *instantiate, const CmHirRegion *source,
    CmHirRegion *out)
{
    const CmTypeckInstantiationFrame *frame;
    const CmHirGenericParam *parameter;

    if (!cm_typeck_region_valid(instantiate->types, source)) {
        return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
    }
    *out = *source;
    if (source->kind != CM_HIR_REGION_EARLY_BOUND) return CM_TYPECK_OK;
    parameter = cm_hir_get_generic_param(instantiate->types->hir,
        source->data.parameter);
    if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_LIFETIME) {
        return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
    }
    frame = cm_typeck_instantiation_frame(instantiate, parameter->owner);
    if (frame != NULL) {
        if (parameter->index >= frame->argument_count) {
            return CM_TYPECK_INVALID_ARGUMENT;
        }
        *out = frame->arguments[parameter->index]
            .data.lifetime;
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_instantiate_const(
    CmTypeckInstantiationState *instantiate, const CmHirConstArg *source,
    CmTypeckConst *out)
{
    const CmTypeckInstantiationFrame *frame;
    const CmHirGenericParam *parameter;
    CmTypeckStatus status;

    memset(out, 0, sizeof(*out));
    if (source->kind != CM_HIR_CONST_VALUE
        && source->kind != CM_HIR_CONST_PARAMETER) {
        return CM_TYPECK_UNSUPPORTED_CONSTANT;
    }
    if (source->kind == CM_HIR_CONST_PARAMETER) {
        parameter = cm_hir_get_generic_param(instantiate->types->hir,
            source->data.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_CONST) {
            return CM_TYPECK_UNSUPPORTED_CONSTANT;
        }
        frame = cm_typeck_instantiation_frame(instantiate,
            parameter->owner);
        if (frame != NULL) {
            if (parameter->index >= frame->argument_count) {
                return CM_TYPECK_INVALID_ARGUMENT;
            }
            *out = frame->arguments[parameter->index]
                .data.constant;
            return CM_TYPECK_OK;
        }
    }
    status = cm_typeck_instantiate_type_inner(instantiate, source->type,
        &out->type);
    if (status != CM_TYPECK_OK) return status;
    out->kind = source->kind;
    if (source->kind == CM_HIR_CONST_VALUE) {
        out->data.value.low_bits = source->data.value.low_bits;
        out->data.value.high_bits = source->data.value.high_bits;
    } else {
        out->data.parameter = source->data.parameter;
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_instantiate_args(
    CmTypeckInstantiationState *instantiate,
    const CmHirGenericArg *source, uint32_t count,
    CmTypeckGenericArg **out_arguments)
{
    CmTypeckGenericArg *arguments;
    CmTypeckStatus status;
    uint32_t index;

    *out_arguments = NULL;
    if (count == 0u) return source == NULL
        ? CM_TYPECK_OK : CM_TYPECK_INVALID_ARGUMENT;
    if (source == NULL) return CM_TYPECK_INVALID_ARGUMENT;
    arguments = (CmTypeckGenericArg *)cm_alloc_zeroed(count,
        sizeof(CmTypeckGenericArg));
    status = CM_TYPECK_OK;
    for (index = 0u; index < count; ++index) {
        arguments[index].kind = source[index].kind;
        switch (source[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            status = cm_typeck_instantiate_region(instantiate,
                &source[index].data.lifetime,
                &arguments[index].data.lifetime);
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            status = cm_typeck_instantiate_type_inner(instantiate,
                source[index].data.type, &arguments[index].data.type);
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            status = cm_typeck_instantiate_const(instantiate,
                &source[index].data.constant,
                &arguments[index].data.constant);
            break;
        default:
            status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
            break;
        }
        if (status != CM_TYPECK_OK) break;
    }
    if (status != CM_TYPECK_OK) {
        cm_free(arguments);
        return status;
    }
    *out_arguments = arguments;
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_instantiate_named_inner(
    CmTypeckInstantiationState *instantiate, const CmHirNamedType *source,
    CmTypeckNamedType *out)
{
    const CmHirDefinition *definition;

    memset(out, 0, sizeof(*out));
    definition = source == NULL ? NULL : cm_hir_lookup_definition(
        instantiate->types->hir, source->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM) {
        return CM_TYPECK_INVALID_ID;
    }
    out->definition = source->definition;
    out->argument_count = source->argument_count;
    return cm_typeck_instantiate_args(instantiate, source->arguments,
        source->argument_count, &out->arguments);
}

static CmTypeckStatus cm_typeck_instantiate_type_inner_impl(
    CmTypeckInstantiationState *instantiate, CmHirTypeId hir_type,
    CmTypeckTypeId *out_type)
{
    const CmTypeckInstantiationFrame *frame;
    const CmHirType *source;
    const CmHirGenericParam *parameter;
    CmTypeckType type;
    CmTypeckStatus status;
    uint32_t index;

    source = cm_hir_get_type(instantiate->types->hir, hir_type);
    if (source == NULL) return CM_TYPECK_INVALID_ID;
    memset(&type, 0, sizeof(type));
    type.span = source->span;
    status = CM_TYPECK_OK;
    switch (source->kind) {
    case CM_HIR_TYPE_NEVER_KIND: type.kind = CM_TYPECK_TYPE_NEVER; break;
    case CM_HIR_TYPE_UNIT_KIND: type.kind = CM_TYPECK_TYPE_UNIT; break;
    case CM_HIR_TYPE_BOOL_KIND: type.kind = CM_TYPECK_TYPE_BOOL; break;
    case CM_HIR_TYPE_CHAR_KIND: type.kind = CM_TYPECK_TYPE_CHAR; break;
    case CM_HIR_TYPE_STR_KIND: type.kind = CM_TYPECK_TYPE_STR; break;
    case CM_HIR_TYPE_INTEGER_KIND:
        type.kind = CM_TYPECK_TYPE_INTEGER;
        type.data.integer_type = source->data.integer_type.kind;
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        type.kind = CM_TYPECK_TYPE_FLOAT;
        type.data.float_type = source->data.float_type.kind;
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        type.kind = CM_TYPECK_TYPE_REFERENCE;
        status = cm_typeck_instantiate_region(instantiate,
            &source->data.reference_type.region,
            &type.data.reference_type.region);
        type.data.reference_type.mutability =
            source->data.reference_type.mutability;
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_type_inner(instantiate,
                source->data.reference_type.pointee,
                &type.data.reference_type.pointee);
        }
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        type.kind = CM_TYPECK_TYPE_RAW_POINTER;
        type.data.raw_pointer_type.mutability =
            source->data.raw_pointer_type.mutability;
        status = cm_typeck_instantiate_type_inner(instantiate,
            source->data.raw_pointer_type.pointee,
            &type.data.raw_pointer_type.pointee);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        type.kind = CM_TYPECK_TYPE_TUPLE;
        type.data.tuple_type.element_count =
            source->data.tuple_type.element_count;
        if (type.data.tuple_type.element_count != 0u) {
            type.data.tuple_type.elements =
                (CmTypeckTypeId *)cm_alloc_zeroed(
                    type.data.tuple_type.element_count,
                    sizeof(CmTypeckTypeId));
        }
        for (index = 0u; index < type.data.tuple_type.element_count;
             ++index) {
            status = cm_typeck_instantiate_type_inner(instantiate,
                source->data.tuple_type.elements[index],
                &type.data.tuple_type.elements[index]);
            if (status != CM_TYPECK_OK) break;
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        type.kind = CM_TYPECK_TYPE_ARRAY;
        status = cm_typeck_instantiate_type_inner(instantiate,
            source->data.array_type.element, &type.data.array_type.element);
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_const(instantiate,
                &source->data.array_type.length,
                &type.data.array_type.length);
        }
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        type.kind = CM_TYPECK_TYPE_SLICE;
        status = cm_typeck_instantiate_type_inner(instantiate,
            source->data.slice_type.element,
            &type.data.slice_type.element);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (source->data.fn_pointer_type.binder.lifetime_count != 0u) {
            return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        }
        type.kind = CM_TYPECK_TYPE_FN_POINTER;
        type.data.fn_pointer_type.parameter_count =
            source->data.fn_pointer_type.parameter_count;
        if (type.data.fn_pointer_type.parameter_count != 0u) {
            type.data.fn_pointer_type.parameters =
                (CmTypeckTypeId *)cm_alloc_zeroed(
                    type.data.fn_pointer_type.parameter_count,
                    sizeof(CmTypeckTypeId));
        }
        for (index = 0u;
             index < type.data.fn_pointer_type.parameter_count; ++index) {
            status = cm_typeck_instantiate_type_inner(instantiate,
                source->data.fn_pointer_type.parameters[index],
                &type.data.fn_pointer_type.parameters[index]);
            if (status != CM_TYPECK_OK) break;
        }
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_type_inner(instantiate,
                source->data.fn_pointer_type.return_type,
                &type.data.fn_pointer_type.return_type);
        }
        type.data.fn_pointer_type.abi = source->data.fn_pointer_type.abi;
        type.data.fn_pointer_type.safety =
            source->data.fn_pointer_type.safety;
        type.data.fn_pointer_type.is_variadic =
            source->data.fn_pointer_type.is_variadic;
        break;
    case CM_HIR_TYPE_ADT_KIND:
        type.kind = CM_TYPECK_TYPE_ADT;
        status = cm_typeck_instantiate_named_inner(instantiate,
            &source->data.named_type, &type.data.named_type);
        break;
    case CM_HIR_TYPE_SELF_KIND:
        if (instantiate->instantiation->self_type == CM_TYPECK_TYPE_NONE
            || !cm_hir_def_id_equal(source->data.self_type.owner,
                instantiate->instantiation->self_owner)) {
            return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        }
        *out_type = instantiate->instantiation->self_type;
        return CM_TYPECK_OK;
    case CM_HIR_TYPE_PARAMETER_KIND:
        parameter = cm_hir_get_generic_param(instantiate->types->hir,
            source->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE) {
            return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        }
        frame = cm_typeck_instantiation_frame(instantiate,
            parameter->owner);
        if (frame != NULL) {
            if (parameter->index >= frame->argument_count) {
                return CM_TYPECK_INVALID_ARGUMENT;
            }
            *out_type = frame->arguments[parameter->index].data.type;
            return CM_TYPECK_OK;
        }
        type.kind = CM_TYPECK_TYPE_PARAMETER;
        type.data.parameter_type.parameter =
            source->data.parameter_type.parameter;
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        type.kind = CM_TYPECK_TYPE_PROJECTION;
        status = cm_typeck_instantiate_type_inner(instantiate,
            source->data.projection_type.self_type,
            &type.data.projection_type.self_type);
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_named_inner(instantiate,
                &source->data.projection_type.trait_type,
                &type.data.projection_type.trait_type);
        }
        if (status == CM_TYPECK_OK) {
            status = cm_typeck_instantiate_named_inner(instantiate,
                &source->data.projection_type.associated_type,
                &type.data.projection_type.associated_type);
        }
        break;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    default:
        status = CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        break;
    }
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_add_stored(instantiate->types, &type,
            CM_HIR_TYPE_NONE, out_type);
    }
    cm_typeck_free_temp_type(&type);
    return status;
}

static CmTypeckStatus cm_typeck_instantiate_type_inner(
    CmTypeckInstantiationState *instantiate, CmHirTypeId hir_type,
    CmTypeckTypeId *out_type)
{
    CmTypeckStatus status;
    CmTypeckImportEntry entry;
    size_t index;

    *out_type = CM_TYPECK_TYPE_NONE;
    for (index = 0u; index < instantiate->memo.len; ++index) {
        const CmTypeckImportEntry *cached;

        cached = (const CmTypeckImportEntry *)cm_vec_at_const(
            &instantiate->memo, index);
        if (cached->hir_type == hir_type) {
            *out_type = cached->type;
            return CM_TYPECK_OK;
        }
    }
    if (instantiate->depth >= CM_TYPECK_MAX_RECURSION) {
        return CM_TYPECK_OVERFLOW;
    }
    instantiate->depth += 1u;
    status = cm_typeck_instantiate_type_inner_impl(instantiate, hir_type,
        out_type);
    instantiate->depth -= 1u;
    if (status == CM_TYPECK_OK) {
        entry.hir_type = hir_type;
        entry.type = *out_type;
        (void)cm_vec_push(&instantiate->memo, &entry);
    }
    return status;
}

static CmTypeckStatus cm_typeck_validate_instantiation_const_types(
    CmTypeckContext *context, CmTypeckInstantiationState *instantiate)
{
    CmHirDefId previous_owner;
    uint32_t frame_ordinal;

    previous_owner = cm_hir_def_id_none();
    for (frame_ordinal = 0u;
         frame_ordinal < instantiate->instantiation->frame_count;
         ++frame_ordinal) {
        const CmTypeckInstantiationFrame *frame;
        uint32_t frame_index;
        uint32_t index;

        frame = NULL;
        for (frame_index = 0u;
             frame_index < instantiate->instantiation->frame_count;
             ++frame_index) {
            const CmTypeckInstantiationFrame *candidate;
            int after_previous;

            candidate = &instantiate->instantiation->frames[frame_index];
            after_previous = cm_hir_def_id_is_none(previous_owner)
                || candidate->parameter_owner.crate_id
                    > previous_owner.crate_id
                || (candidate->parameter_owner.crate_id
                        == previous_owner.crate_id
                    && candidate->parameter_owner.index
                        > previous_owner.index);
            if (!after_previous) continue;
            if (frame == NULL
                || candidate->parameter_owner.crate_id
                    < frame->parameter_owner.crate_id
                || (candidate->parameter_owner.crate_id
                        == frame->parameter_owner.crate_id
                    && candidate->parameter_owner.index
                        < frame->parameter_owner.index)) {
                frame = candidate;
            }
        }
        if (frame == NULL) return CM_TYPECK_INVALID_ARGUMENT;
        previous_owner = frame->parameter_owner;
        for (index = 0u; index < frame->argument_count; ++index) {
            const CmHirGenericParam *parameter;
            CmTypeckTypeId declared_type;
            CmTypeckStatus status;

            parameter = cm_typeck_instantiation_parameter(
                instantiate->types, frame->parameter_owner, index);
            if (parameter == NULL) return CM_TYPECK_INVALID_ARGUMENT;
            if (parameter->kind != CM_HIR_GENERIC_CONST) continue;
            status = cm_typeck_instantiate_type_inner(instantiate,
                parameter->declared_type, &declared_type);
            if (status != CM_TYPECK_OK) return status;
            status = cm_typeck_unify(context, declared_type,
                frame->arguments[index].data.constant.type);
            if (status != CM_TYPECK_OK) return status;
        }
    }
    return CM_TYPECK_OK;
}

CmTypeckStatus cm_typeck_instantiate_hir_type_scoped(
    CmTypeckContext *context, CmHirTypeId hir_type,
    const CmTypeckScopedInstantiation *instantiation,
    CmTypeckTypeId *out_type)
{
    CmTypeckState *state;
    CmTypeckInstantiationState instantiate;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus status;

    state = cm_typeck_state(context);
    if (out_type != NULL) *out_type = CM_TYPECK_TYPE_NONE;
    if (!cm_typeck_state_is_current(state) || out_type == NULL
        || !cm_typeck_scoped_instantiation_valid_inner(state,
            instantiation)) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    status = cm_typeck_snapshot(context, &snapshot);
    if (status != CM_TYPECK_OK) return status;
    memset(&instantiate, 0, sizeof(instantiate));
    instantiate.types = state;
    instantiate.instantiation = instantiation;
    cm_vec_init(&instantiate.memo, sizeof(CmTypeckImportEntry));
    status = cm_typeck_validate_instantiation_const_types(context,
        &instantiate);
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_instantiate_type_inner(&instantiate, hir_type,
            out_type);
    }
    cm_vec_destroy(&instantiate.memo);
    if (status == CM_TYPECK_OK) {
        (void)cm_typeck_commit(context, &snapshot);
    } else {
        (void)cm_typeck_rollback(context, &snapshot);
        *out_type = CM_TYPECK_TYPE_NONE;
    }
    return status;
}

CmTypeckStatus cm_typeck_instantiate_hir_named_scoped(
    CmTypeckContext *context, const CmHirNamedType *named,
    const CmTypeckScopedInstantiation *instantiation,
    CmTypeckNamedType *out_named)
{
    CmTypeckState *state;
    CmTypeckInstantiationState instantiate;
    CmTypeckSnapshot snapshot;
    CmTypeckNamedType temporary;
    CmTypeckStatus status;

    state = cm_typeck_state(context);
    if (out_named != NULL) {
        memset(out_named, 0, sizeof(*out_named));
        out_named->definition = cm_hir_def_id_none();
    }
    if (!cm_typeck_state_is_current(state) || out_named == NULL
        || !cm_typeck_scoped_instantiation_valid_inner(state,
            instantiation)) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    status = cm_typeck_snapshot(context, &snapshot);
    if (status != CM_TYPECK_OK) return status;
    memset(&instantiate, 0, sizeof(instantiate));
    instantiate.types = state;
    instantiate.instantiation = instantiation;
    cm_vec_init(&instantiate.memo, sizeof(CmTypeckImportEntry));
    status = cm_typeck_validate_instantiation_const_types(context,
        &instantiate);
    if (status == CM_TYPECK_OK) {
        instantiate.depth += 1u;
        status = cm_typeck_instantiate_named_inner(&instantiate, named,
            &temporary);
        instantiate.depth -= 1u;
    }
    cm_vec_destroy(&instantiate.memo);
    if (status == CM_TYPECK_OK) {
        *out_named = temporary;
        out_named->arguments = (CmTypeckGenericArg *)cm_typeck_copy_array(
            state, temporary.arguments, temporary.argument_count,
            sizeof(CmTypeckGenericArg));
        cm_free(temporary.arguments);
        (void)cm_typeck_commit(context, &snapshot);
    } else {
        (void)cm_typeck_rollback(context, &snapshot);
        memset(out_named, 0, sizeof(*out_named));
        out_named->definition = cm_hir_def_id_none();
    }
    return status;
}

static void cm_typeck_scoped_from_instantiation(
    const CmTypeckState *state,
    const CmTypeckInstantiation *instantiation,
    CmTypeckInstantiationFrame *frame,
    CmTypeckScopedInstantiation *scoped)
{
    memset(frame, 0, sizeof(*frame));
    memset(scoped, 0, sizeof(*scoped));
    if (instantiation == NULL) return;
    frame->parameter_owner = instantiation->parameter_owner;
    frame->arguments = instantiation->arguments;
    frame->argument_count = instantiation->argument_count;
    scoped->typeck_state = instantiation->typeck_state;
    scoped->typeck_lifetime_id = instantiation->typeck_lifetime_id;
    scoped->typeck_rollback_generation = state == NULL
        ? 0u : state->rollback_generation;
    scoped->frames = frame;
    scoped->frame_count = 1u;
    scoped->self_owner = instantiation->self_owner;
    scoped->self_type = instantiation->self_type;
}

CmTypeckStatus cm_typeck_instantiate_hir_type(CmTypeckContext *context,
    CmHirTypeId hir_type, const CmTypeckInstantiation *instantiation,
    CmTypeckTypeId *out_type)
{
    CmTypeckInstantiationFrame frame;
    CmTypeckScopedInstantiation scoped;
    const CmTypeckState *state;

    if (out_type != NULL) *out_type = CM_TYPECK_TYPE_NONE;
    if (instantiation == NULL) return CM_TYPECK_INVALID_ARGUMENT;
    state = cm_typeck_state_const(context);
    cm_typeck_scoped_from_instantiation(state, instantiation, &frame,
        &scoped);
    return cm_typeck_instantiate_hir_type_scoped(context, hir_type,
        &scoped, out_type);
}

CmTypeckStatus cm_typeck_instantiate_hir_named(CmTypeckContext *context,
    const CmHirNamedType *named,
    const CmTypeckInstantiation *instantiation,
    CmTypeckNamedType *out_named)
{
    CmTypeckInstantiationFrame frame;
    CmTypeckScopedInstantiation scoped;
    const CmTypeckState *state;

    if (out_named != NULL) {
        memset(out_named, 0, sizeof(*out_named));
        out_named->definition = cm_hir_def_id_none();
    }
    if (instantiation == NULL) return CM_TYPECK_INVALID_ARGUMENT;
    state = cm_typeck_state_const(context);
    cm_typeck_scoped_from_instantiation(state, instantiation, &frame,
        &scoped);
    return cm_typeck_instantiate_hir_named_scoped(context, named,
        &scoped, out_named);
}

static void cm_typeck_write_variable(CmTypeckState *state, uint32_t id,
    const CmTypeckVariable *new_value)
{
    CmTypeckVariable *variable;
    CmTypeckStoredType *term;
    CmTypeckTrailEntry trail;

    variable = cm_typeck_variable(state, id);
    term = cm_typeck_stored_type(state, variable->term);
    trail.variable = id;
    trail.old_value = *variable;
    trail.old_term_class = term->type.data.variable.class_kind;
    (void)cm_vec_push(&state->trail, &trail);
    *variable = *new_value;
    term->type.data.variable.class_kind = new_value->class_kind;
    cm_typeck_record_state_mutation(state);
}

static int cm_typeck_class_merge(CmHirInferenceKind left,
    CmHirInferenceKind right, CmHirInferenceKind *out)
{
    if (left == right) {
        *out = left;
        return 1;
    }
    if (left == CM_HIR_INFER_GENERAL) {
        *out = right;
        return 1;
    }
    if (right == CM_HIR_INFER_GENERAL) {
        *out = left;
        return 1;
    }
    return 0;
}

static int cm_typeck_class_accepts(const CmTypeckState *state,
    CmHirInferenceKind class_kind, CmTypeckTypeId type)
{
    CmTypeckTypeId resolved;
    uint32_t variable;
    const CmTypeckStoredType *stored;
    const CmTypeckVariable *value;
    CmHirInferenceKind ignored;

    if (cm_typeck_resolve_internal(state, type, &resolved, &variable)
            != CM_TYPECK_OK) return 0;
    if (variable != 0u) {
        value = cm_typeck_variable_const(state, variable);
        return value != NULL && cm_typeck_class_merge(class_kind,
            value->class_kind, &ignored);
    }
    stored = cm_typeck_stored_type_const(state, resolved);
    if (class_kind == CM_HIR_INFER_GENERAL) return stored != NULL;
    if (class_kind == CM_HIR_INFER_INTEGER) {
        return stored != NULL && stored->type.kind
            == CM_TYPECK_TYPE_INTEGER;
    }
    return stored != NULL && stored->type.kind == CM_TYPECK_TYPE_FLOAT;
}

static void cm_typeck_occurs_push_args(CmVec *pending,
    const CmTypeckGenericArg *arguments, uint32_t count)
{
    uint32_t index;

    for (index = 0u; index < count; ++index) {
        CmTypeckTypeId child;

        child = CM_TYPECK_TYPE_NONE;
        if (arguments[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            child = arguments[index].data.type;
        } else if (arguments[index].kind == CM_HIR_GENERIC_ARG_CONST) {
            child = arguments[index].data.constant.type;
        }
        if (child != CM_TYPECK_TYPE_NONE) (void)cm_vec_push(pending, &child);
    }
}

static int cm_typeck_occurs(const CmTypeckState *state,
    uint32_t target_variable, CmTypeckTypeId type)
{
    unsigned char *seen;
    CmVec pending;
    int found;

    seen = (unsigned char *)cm_alloc_zeroed(state->types.len,
        sizeof(unsigned char));
    cm_vec_init(&pending, sizeof(CmTypeckTypeId));
    (void)cm_vec_push(&pending, &type);
    found = 0;
    while (pending.len != 0u && !found) {
        CmTypeckTypeId current;
        CmTypeckTypeId resolved;
        uint32_t variable;
        const CmTypeckType *value;
        uint32_t index;

        (void)cm_vec_pop(&pending, &current);
        if (cm_typeck_resolve_internal(state, current, &resolved, &variable)
                != CM_TYPECK_OK) {
            found = 1;
            break;
        }
        if (variable != 0u) {
            found = variable == target_variable;
            continue;
        }
        if (seen[(size_t)resolved - 1u]) continue;
        seen[(size_t)resolved - 1u] = 1u;
        value = &cm_typeck_stored_type_const(state, resolved)->type;
        switch (value->kind) {
        case CM_TYPECK_TYPE_REFERENCE:
            current = value->data.reference_type.pointee;
            (void)cm_vec_push(&pending, &current);
            break;
        case CM_TYPECK_TYPE_RAW_POINTER:
            current = value->data.raw_pointer_type.pointee;
            (void)cm_vec_push(&pending, &current);
            break;
        case CM_TYPECK_TYPE_TUPLE:
            for (index = 0u; index < value->data.tuple_type.element_count;
                 ++index) {
                current = value->data.tuple_type.elements[index];
                (void)cm_vec_push(&pending, &current);
            }
            break;
        case CM_TYPECK_TYPE_ARRAY:
            current = value->data.array_type.element;
            (void)cm_vec_push(&pending, &current);
            current = value->data.array_type.length.type;
            (void)cm_vec_push(&pending, &current);
            break;
        case CM_TYPECK_TYPE_SLICE:
            current = value->data.slice_type.element;
            (void)cm_vec_push(&pending, &current);
            break;
        case CM_TYPECK_TYPE_FN_POINTER:
            for (index = 0u;
                 index < value->data.fn_pointer_type.parameter_count;
                 ++index) {
                current = value->data.fn_pointer_type.parameters[index];
                (void)cm_vec_push(&pending, &current);
            }
            current = value->data.fn_pointer_type.return_type;
            (void)cm_vec_push(&pending, &current);
            break;
        case CM_TYPECK_TYPE_ADT:
            cm_typeck_occurs_push_args(&pending,
                value->data.named_type.arguments,
                value->data.named_type.argument_count);
            break;
        case CM_TYPECK_TYPE_PROJECTION:
            current = value->data.projection_type.self_type;
            (void)cm_vec_push(&pending, &current);
            cm_typeck_occurs_push_args(&pending,
                value->data.projection_type.trait_type.arguments,
                value->data.projection_type.trait_type.argument_count);
            cm_typeck_occurs_push_args(&pending,
                value->data.projection_type.associated_type.arguments,
                value->data.projection_type.associated_type.argument_count);
            break;
        default:
            break;
        }
    }
    cm_vec_destroy(&pending);
    cm_free(seen);
    return found;
}

typedef struct CmTypeckUnifyPair {
    CmTypeckTypeId left;
    CmTypeckTypeId right;
} CmTypeckUnifyPair;

typedef struct CmTypeckUnifyState {
    CmTypeckState *types;
    CmVec visited_pairs;
    size_t depth;
} CmTypeckUnifyState;

static int cm_typeck_unify_pair_seen(CmTypeckUnifyState *unify,
    CmTypeckTypeId left, CmTypeckTypeId right)
{
    CmTypeckUnifyPair pair;
    size_t index;

    pair.left = left < right ? left : right;
    pair.right = left < right ? right : left;
    for (index = 0u; index < unify->visited_pairs.len; ++index) {
        const CmTypeckUnifyPair *seen;

        seen = (const CmTypeckUnifyPair *)cm_vec_at_const(
            &unify->visited_pairs, index);
        if (seen->left == pair.left && seen->right == pair.right) return 1;
    }
    (void)cm_vec_push(&unify->visited_pairs, &pair);
    return 0;
}

static CmTypeckStatus cm_typeck_unify_inner_impl(CmTypeckUnifyState *unify,
    CmTypeckTypeId left, CmTypeckTypeId right);

static CmTypeckStatus cm_typeck_unify_inner(CmTypeckUnifyState *unify,
    CmTypeckTypeId left, CmTypeckTypeId right)
{
    CmTypeckStatus status;

    if (unify->depth >= CM_TYPECK_MAX_RECURSION) {
        return CM_TYPECK_OVERFLOW;
    }
    unify->depth += 1u;
    status = cm_typeck_unify_inner_impl(unify, left, right);
    unify->depth -= 1u;
    return status;
}

static CmTypeckStatus cm_typeck_unify_const(CmTypeckUnifyState *unify,
    const CmTypeckConst *left, const CmTypeckConst *right)
{
    CmTypeckStatus status;

    if (left->kind != right->kind) return CM_TYPECK_TYPE_MISMATCH;
    status = cm_typeck_unify_inner(unify, left->type, right->type);
    if (status != CM_TYPECK_OK) return status;
    if (left->kind == CM_HIR_CONST_VALUE) {
        return left->data.value.low_bits == right->data.value.low_bits
                && left->data.value.high_bits
                    == right->data.value.high_bits
            ? CM_TYPECK_OK : CM_TYPECK_TYPE_MISMATCH;
    }
    if (left->kind == CM_HIR_CONST_PARAMETER) {
        return left->data.parameter == right->data.parameter
            ? CM_TYPECK_OK : CM_TYPECK_TYPE_MISMATCH;
    }
    return CM_TYPECK_UNSUPPORTED_CONSTANT;
}

static CmTypeckStatus cm_typeck_unify_args(CmTypeckUnifyState *unify,
    const CmTypeckGenericArg *left, uint32_t left_count,
    const CmTypeckGenericArg *right, uint32_t right_count)
{
    uint32_t index;

    if (left_count != right_count) return CM_TYPECK_TYPE_MISMATCH;
    for (index = 0u; index < left_count; ++index) {
        CmTypeckStatus status;

        if (left[index].kind != right[index].kind) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        switch (left[index].kind) {
        case CM_HIR_GENERIC_ARG_LIFETIME:
            /* Region compatibility is emitted as a later obligation. */
            status = CM_TYPECK_OK;
            break;
        case CM_HIR_GENERIC_ARG_TYPE:
            status = cm_typeck_unify_inner(unify, left[index].data.type,
                right[index].data.type);
            break;
        case CM_HIR_GENERIC_ARG_CONST:
            status = cm_typeck_unify_const(unify,
                &left[index].data.constant, &right[index].data.constant);
            break;
        default:
            status = CM_TYPECK_TYPE_MISMATCH;
            break;
        }
        if (status != CM_TYPECK_OK) return status;
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_bind_variable(CmTypeckState *state,
    uint32_t variable_id, CmTypeckTypeId type)
{
    CmTypeckVariable *variable;
    CmTypeckVariable updated;

    variable = cm_typeck_variable(state, variable_id);
    if (variable == NULL) return CM_TYPECK_INVALID_ID;
    if (!cm_typeck_class_accepts(state, variable->class_kind, type)) {
        return CM_TYPECK_KIND_CONFLICT;
    }
    if (cm_typeck_occurs(state, variable_id, type)) {
        return CM_TYPECK_OCCURS_CHECK;
    }
    updated = *variable;
    updated.binding = type;
    cm_typeck_write_variable(state, variable_id, &updated);
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_union_variables(CmTypeckUnifyState *unify,
    uint32_t left_id, uint32_t right_id)
{
    CmTypeckState *state;
    CmTypeckVariable *left;
    CmTypeckVariable *right;
    CmTypeckVariable left_updated;
    CmTypeckVariable right_updated;
    CmHirInferenceKind class_kind;
    CmTypeckTypeId left_binding;
    CmTypeckTypeId right_binding;
    uint32_t canonical_id;
    uint32_t root_id;
    uint32_t child_id;

    state = unify->types;
    if (left_id == right_id) return CM_TYPECK_OK;
    left = cm_typeck_variable(state, left_id);
    right = cm_typeck_variable(state, right_id);
    if (left == NULL || right == NULL) return CM_TYPECK_INVALID_ID;
    if (!cm_typeck_class_merge(left->class_kind, right->class_kind,
            &class_kind)) return CM_TYPECK_KIND_CONFLICT;
    if ((left->binding != CM_TYPECK_TYPE_NONE
            && !cm_typeck_class_accepts(state, class_kind, left->binding))
        || (right->binding != CM_TYPECK_TYPE_NONE
            && !cm_typeck_class_accepts(state, class_kind,
                right->binding))) return CM_TYPECK_KIND_CONFLICT;
    if ((right->binding != CM_TYPECK_TYPE_NONE
            && cm_typeck_occurs(state, left_id, right->binding))
        || (left->binding != CM_TYPECK_TYPE_NONE
            && cm_typeck_occurs(state, right_id, left->binding))) {
        return CM_TYPECK_OCCURS_CHECK;
    }
    if (left->rank > right->rank) {
        root_id = left_id;
        child_id = right_id;
    } else if (right->rank > left->rank) {
        root_id = right_id;
        child_id = left_id;
    } else {
        root_id = left_id < right_id ? left_id : right_id;
        child_id = left_id < right_id ? right_id : left_id;
    }
    left = cm_typeck_variable(state, root_id);
    right = cm_typeck_variable(state, child_id);
    canonical_id = left->canonical < right->canonical
        ? left->canonical : right->canonical;
    left_binding = left->binding;
    right_binding = right->binding;
    left_updated = *left;
    right_updated = *right;
    left_updated.class_kind = class_kind;
    left_updated.canonical = canonical_id;
    if (left->rank == right->rank) left_updated.rank += 1u;
    if (left_updated.binding == CM_TYPECK_TYPE_NONE) {
        left_updated.binding = right_updated.binding;
    }
    right_updated.parent = root_id;
    right_updated.class_kind = class_kind;
    cm_typeck_write_variable(state, root_id, &left_updated);
    cm_typeck_write_variable(state, child_id, &right_updated);
    if (canonical_id != root_id && canonical_id != child_id) {
        CmTypeckVariable *canonical;
        CmTypeckVariable canonical_updated;

        canonical = cm_typeck_variable(state, canonical_id);
        canonical_updated = *canonical;
        canonical_updated.class_kind = class_kind;
        cm_typeck_write_variable(state, canonical_id, &canonical_updated);
    }
    if (left_binding != CM_TYPECK_TYPE_NONE
        && right_binding != CM_TYPECK_TYPE_NONE) {
        return cm_typeck_unify_inner(unify, left_binding, right_binding);
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_unify_inner_impl(CmTypeckUnifyState *unify,
    CmTypeckTypeId left, CmTypeckTypeId right)
{
    CmTypeckState *state;
    CmTypeckTypeId left_resolved;
    CmTypeckTypeId right_resolved;
    uint32_t left_variable;
    uint32_t right_variable;
    const CmTypeckType *left_type;
    const CmTypeckType *right_type;
    CmTypeckStatus status;
    uint32_t index;

    state = unify->types;
    status = cm_typeck_resolve_internal(state, left, &left_resolved,
        &left_variable);
    if (status != CM_TYPECK_OK) return status;
    status = cm_typeck_resolve_internal(state, right, &right_resolved,
        &right_variable);
    if (status != CM_TYPECK_OK) return status;
    if (left_resolved == right_resolved) return CM_TYPECK_OK;
    if (left_variable != 0u && right_variable != 0u) {
        return cm_typeck_union_variables(unify, left_variable,
            right_variable);
    }
    if (left_variable != 0u) {
        return cm_typeck_bind_variable(state, left_variable,
            right_resolved);
    }
    if (right_variable != 0u) {
        return cm_typeck_bind_variable(state, right_variable,
            left_resolved);
    }
    left_type = &cm_typeck_stored_type_const(state, left_resolved)->type;
    right_type = &cm_typeck_stored_type_const(state, right_resolved)->type;
    if (left_type->kind != right_type->kind) {
        return CM_TYPECK_TYPE_MISMATCH;
    }
    if (cm_typeck_unify_pair_seen(unify, left_resolved, right_resolved)) {
        return CM_TYPECK_OK;
    }
    switch (left_type->kind) {
    case CM_TYPECK_TYPE_NEVER:
    case CM_TYPECK_TYPE_UNIT:
    case CM_TYPECK_TYPE_BOOL:
    case CM_TYPECK_TYPE_CHAR:
    case CM_TYPECK_TYPE_STR:
        return CM_TYPECK_OK;
    case CM_TYPECK_TYPE_INTEGER:
        return left_type->data.integer_type
                == right_type->data.integer_type
            ? CM_TYPECK_OK : CM_TYPECK_TYPE_MISMATCH;
    case CM_TYPECK_TYPE_FLOAT:
        return left_type->data.float_type == right_type->data.float_type
            ? CM_TYPECK_OK : CM_TYPECK_TYPE_MISMATCH;
    case CM_TYPECK_TYPE_REFERENCE:
        if (left_type->data.reference_type.mutability
                != right_type->data.reference_type.mutability) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        return cm_typeck_unify_inner(unify,
            left_type->data.reference_type.pointee,
            right_type->data.reference_type.pointee);
    case CM_TYPECK_TYPE_RAW_POINTER:
        if (left_type->data.raw_pointer_type.mutability
                != right_type->data.raw_pointer_type.mutability) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        return cm_typeck_unify_inner(unify,
            left_type->data.raw_pointer_type.pointee,
            right_type->data.raw_pointer_type.pointee);
    case CM_TYPECK_TYPE_TUPLE:
        if (left_type->data.tuple_type.element_count
                != right_type->data.tuple_type.element_count) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        for (index = 0u;
             index < left_type->data.tuple_type.element_count; ++index) {
            status = cm_typeck_unify_inner(unify,
                left_type->data.tuple_type.elements[index],
                right_type->data.tuple_type.elements[index]);
            if (status != CM_TYPECK_OK) return status;
        }
        return CM_TYPECK_OK;
    case CM_TYPECK_TYPE_ARRAY:
        status = cm_typeck_unify_inner(unify,
            left_type->data.array_type.element,
            right_type->data.array_type.element);
        return status == CM_TYPECK_OK ? cm_typeck_unify_const(unify,
            &left_type->data.array_type.length,
            &right_type->data.array_type.length) : status;
    case CM_TYPECK_TYPE_SLICE:
        return cm_typeck_unify_inner(unify,
            left_type->data.slice_type.element,
            right_type->data.slice_type.element);
    case CM_TYPECK_TYPE_FN_POINTER:
        if (left_type->data.fn_pointer_type.parameter_count
                != right_type->data.fn_pointer_type.parameter_count
            || left_type->data.fn_pointer_type.binder_lifetime_count
                != right_type->data.fn_pointer_type.binder_lifetime_count
            || left_type->data.fn_pointer_type.abi
                != right_type->data.fn_pointer_type.abi
            || left_type->data.fn_pointer_type.safety
                != right_type->data.fn_pointer_type.safety
            || left_type->data.fn_pointer_type.is_variadic
                != right_type->data.fn_pointer_type.is_variadic) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        for (index = 0u;
             index < left_type->data.fn_pointer_type.parameter_count;
             ++index) {
            status = cm_typeck_unify_inner(unify,
                left_type->data.fn_pointer_type.parameters[index],
                right_type->data.fn_pointer_type.parameters[index]);
            if (status != CM_TYPECK_OK) return status;
        }
        return cm_typeck_unify_inner(unify,
            left_type->data.fn_pointer_type.return_type,
            right_type->data.fn_pointer_type.return_type);
    case CM_TYPECK_TYPE_ADT:
        if (!cm_hir_def_id_equal(left_type->data.named_type.definition,
                right_type->data.named_type.definition)) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        return cm_typeck_unify_args(unify,
            left_type->data.named_type.arguments,
            left_type->data.named_type.argument_count,
            right_type->data.named_type.arguments,
            right_type->data.named_type.argument_count);
    case CM_TYPECK_TYPE_PARAMETER:
        return left_type->data.parameter_type.parameter
                == right_type->data.parameter_type.parameter
            ? CM_TYPECK_OK : CM_TYPECK_TYPE_MISMATCH;
    case CM_TYPECK_TYPE_PROJECTION:
        if (!cm_hir_def_id_equal(
                left_type->data.projection_type.trait_type.definition,
                right_type->data.projection_type.trait_type.definition)
            || !cm_hir_def_id_equal(
                left_type->data.projection_type.associated_type.definition,
                right_type->data.projection_type.associated_type.definition)) {
            return CM_TYPECK_TYPE_MISMATCH;
        }
        status = cm_typeck_unify_inner(unify,
            left_type->data.projection_type.self_type,
            right_type->data.projection_type.self_type);
        if (status == CM_TYPECK_OK) status = cm_typeck_unify_args(unify,
            left_type->data.projection_type.trait_type.arguments,
            left_type->data.projection_type.trait_type.argument_count,
            right_type->data.projection_type.trait_type.arguments,
            right_type->data.projection_type.trait_type.argument_count);
        if (status == CM_TYPECK_OK) status = cm_typeck_unify_args(unify,
            left_type->data.projection_type.associated_type.arguments,
            left_type->data.projection_type.associated_type.argument_count,
            right_type->data.projection_type.associated_type.arguments,
            right_type->data.projection_type.associated_type.argument_count);
        return status;
    case CM_TYPECK_TYPE_VARIABLE:
    default:
        return CM_TYPECK_INVALID_ID;
    }
}

CmTypeckStatus cm_typeck_unify(CmTypeckContext *context,
    CmTypeckTypeId left, CmTypeckTypeId right)
{
    CmTypeckState *state;
    CmTypeckSnapshot snapshot;
    CmTypeckUnifyState unify;
    CmTypeckStatus status;

    state = cm_typeck_state(context);
    if (state == NULL || !cm_typeck_type_id_valid(state, left)
        || !cm_typeck_type_id_valid(state, right)) {
        return CM_TYPECK_INVALID_ID;
    }
    status = cm_typeck_snapshot(context, &snapshot);
    if (status != CM_TYPECK_OK) return status;
    memset(&unify, 0, sizeof(unify));
    unify.types = state;
    cm_vec_init(&unify.visited_pairs, sizeof(CmTypeckUnifyPair));
    status = cm_typeck_unify_inner(&unify, left, right);
    cm_vec_destroy(&unify.visited_pairs);
    if (status == CM_TYPECK_OK) {
        (void)cm_typeck_commit(context, &snapshot);
    } else {
        (void)cm_typeck_rollback(context, &snapshot);
    }
    return status;
}

CmTypeckStatus cm_typeck_default_unresolved(CmTypeckContext *context,
    CmHirInferenceKind class_kind, CmHirTypeId default_hir_type,
    size_t *out_defaulted_count)
{
    CmTypeckState *state;
    CmTypeckTypeId default_type;
    CmTypeckSnapshot snapshot;
    CmTypeckStatus status;
    size_t defaulted_count;
    size_t index;

    if (out_defaulted_count != NULL) *out_defaulted_count = 0u;
    state = cm_typeck_state(context);
    if (!cm_typeck_state_is_current(state)
        || out_defaulted_count == NULL
        || (unsigned int)class_kind > (unsigned int)CM_HIR_INFER_FLOAT
        || cm_hir_get_type(state == NULL ? NULL : state->hir,
            default_hir_type) == NULL) {
        return CM_TYPECK_INVALID_ARGUMENT;
    }
    status = cm_typeck_snapshot(context, &snapshot);
    if (status != CM_TYPECK_OK) return status;
    status = cm_typeck_import_hir_type(context, default_hir_type,
        &default_type);
    if (status != CM_TYPECK_OK
        || !cm_typeck_class_accepts(state, class_kind, default_type)) {
        if (status == CM_TYPECK_OK) status = CM_TYPECK_KIND_CONFLICT;
        (void)cm_typeck_rollback(context, &snapshot);
        return status;
    }
    defaulted_count = 0u;
    for (index = 0u; index < state->variables.len; ++index) {
        CmTypeckVariable *variable;
        uint32_t variable_id;

        variable_id = (uint32_t)(index + 1u);
        variable = cm_typeck_variable(state, variable_id);
        if (variable == NULL) {
            status = CM_TYPECK_INVALID_ID;
            break;
        }
        if (variable->parent != variable_id
            || variable->binding != CM_TYPECK_TYPE_NONE
            || variable->class_kind != class_kind) {
            continue;
        }
        status = cm_typeck_bind_variable(state, variable_id,
            default_type);
        if (status != CM_TYPECK_OK) break;
        defaulted_count += 1u;
    }
    if (status == CM_TYPECK_OK) {
        status = cm_typeck_commit(context, &snapshot);
        if (status == CM_TYPECK_OK) {
            *out_defaulted_count = defaulted_count;
        }
    } else {
        (void)cm_typeck_rollback(context, &snapshot);
    }
    return status;
}

static int cm_typeck_caller_mark_valid(const CmHirContext *hir,
    const CmHirContextMark *mark)
{
    return hir != NULL && mark != NULL && mark->active
        && mark->context == hir && mark->crates <= hir->crates.len
        && mark->modules <= hir->modules.len && mark->items <= hir->items.len
        && mark->bodies <= hir->bodies.len
        && mark->closures <= hir->closures.len
        && mark->expressions <= hir->expressions.len
        && mark->types <= hir->types.len
        && mark->generic_parameters <= hir->generic_parameters.len
        && mark->definitions <= hir->definitions.len
        && mark->prebound_associated_types
            <= hir->prebound_associated_types.len
        && cm_arena_mark_is_valid(&hir->storage, mark->storage)
        && cm_interner_mark_is_valid(&hir->strings, mark->strings);
}

typedef struct CmTypeckFreezeState {
    CmTypeckState *typeck;
    CmHirContext *hir;
    CmHirTypeId *memo;
    CmHirStatus hir_status;
    size_t depth;
} CmTypeckFreezeState;

static CmTypeckStatus cm_typeck_freeze_inner_impl(
    CmTypeckFreezeState *state,
    CmTypeckTypeId type, CmHirTypeId *out_type);

static CmTypeckStatus cm_typeck_freeze_inner(CmTypeckFreezeState *state,
    CmTypeckTypeId type, CmHirTypeId *out_type)
{
    CmTypeckStatus status;

    if (state->depth >= CM_TYPECK_MAX_RECURSION) {
        return CM_TYPECK_OVERFLOW;
    }
    state->depth += 1u;
    status = cm_typeck_freeze_inner_impl(state, type, out_type);
    state->depth -= 1u;
    return status;
}

static CmTypeckStatus cm_typeck_freeze_const(CmTypeckFreezeState *state,
    const CmTypeckConst *source, CmHirConstArg *out)
{
    CmTypeckStatus status;

    memset(out, 0, sizeof(*out));
    status = cm_typeck_freeze_inner(state, source->type, &out->type);
    if (status != CM_TYPECK_OK) return status;
    out->kind = source->kind;
    if (source->kind == CM_HIR_CONST_VALUE) {
        out->data.value.low_bits = source->data.value.low_bits;
        out->data.value.high_bits = source->data.value.high_bits;
    } else if (source->kind == CM_HIR_CONST_PARAMETER) {
        out->data.parameter = source->data.parameter;
    } else {
        return CM_TYPECK_UNSUPPORTED_CONSTANT;
    }
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_freeze_args(CmTypeckFreezeState *state,
    const CmTypeckGenericArg *source, uint32_t count,
    CmHirGenericArg **out_arguments)
{
    CmHirGenericArg *arguments;
    CmTypeckStatus status;
    uint32_t index;

    *out_arguments = NULL;
    if (count == 0u) return CM_TYPECK_OK;
    arguments = (CmHirGenericArg *)cm_alloc_zeroed(count,
        sizeof(CmHirGenericArg));
    status = CM_TYPECK_OK;
    for (index = 0u; index < count; ++index) {
        arguments[index].kind = source[index].kind;
        if (source[index].kind == CM_HIR_GENERIC_ARG_LIFETIME) {
            arguments[index].data.lifetime = source[index].data.lifetime;
        } else if (source[index].kind == CM_HIR_GENERIC_ARG_TYPE) {
            status = cm_typeck_freeze_inner(state, source[index].data.type,
                &arguments[index].data.type);
        } else if (source[index].kind == CM_HIR_GENERIC_ARG_CONST) {
            status = cm_typeck_freeze_const(state,
                &source[index].data.constant,
                &arguments[index].data.constant);
        } else {
            status = CM_TYPECK_TYPE_MISMATCH;
        }
        if (status != CM_TYPECK_OK) break;
    }
    if (status != CM_TYPECK_OK) {
        cm_free(arguments);
        return status;
    }
    *out_arguments = arguments;
    return CM_TYPECK_OK;
}

static CmTypeckStatus cm_typeck_freeze_named(CmTypeckFreezeState *state,
    const CmTypeckNamedType *source, CmHirNamedType *out)
{
    memset(out, 0, sizeof(*out));
    out->definition = source->definition;
    out->argument_count = source->argument_count;
    return cm_typeck_freeze_args(state, source->arguments,
        source->argument_count, &out->arguments);
}

static void cm_typeck_freeze_temp_destroy(CmHirType *type)
{
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        cm_free(type->data.tuple_type.elements);
    } else if (type->kind == CM_HIR_TYPE_FN_POINTER_KIND) {
        cm_free(type->data.fn_pointer_type.parameters);
    } else if (type->kind == CM_HIR_TYPE_ADT_KIND) {
        cm_free(type->data.named_type.arguments);
    } else if (type->kind == CM_HIR_TYPE_PROJECTION_KIND) {
        cm_free(type->data.projection_type.trait_type.arguments);
        cm_free(type->data.projection_type.associated_type.arguments);
    }
}

static CmTypeckStatus cm_typeck_freeze_inner_impl(
    CmTypeckFreezeState *state,
    CmTypeckTypeId type, CmHirTypeId *out_type)
{
    CmTypeckTypeId resolved;
    uint32_t variable;
    const CmTypeckStoredType *stored;
    const CmTypeckType *source;
    CmHirType target;
    CmTypeckStatus status;
    uint32_t index;

    status = cm_typeck_resolve_internal(state->typeck, type, &resolved,
        &variable);
    if (status != CM_TYPECK_OK) return status;
    if (variable != 0u) return CM_TYPECK_UNRESOLVED;
    if (state->memo[(size_t)resolved - 1u] != CM_HIR_TYPE_NONE) {
        *out_type = state->memo[(size_t)resolved - 1u];
        return CM_TYPECK_OK;
    }
    stored = cm_typeck_stored_type_const(state->typeck, resolved);
    if (stored->source_hir_type != CM_HIR_TYPE_NONE) {
        state->memo[(size_t)resolved - 1u] = stored->source_hir_type;
        *out_type = stored->source_hir_type;
        return CM_TYPECK_OK;
    }
    source = &stored->type;
    memset(&target, 0, sizeof(target));
    target.span = source->span;
    status = CM_TYPECK_OK;
    switch (source->kind) {
    case CM_TYPECK_TYPE_NEVER: target.kind = CM_HIR_TYPE_NEVER_KIND; break;
    case CM_TYPECK_TYPE_UNIT: target.kind = CM_HIR_TYPE_UNIT_KIND; break;
    case CM_TYPECK_TYPE_BOOL: target.kind = CM_HIR_TYPE_BOOL_KIND; break;
    case CM_TYPECK_TYPE_CHAR: target.kind = CM_HIR_TYPE_CHAR_KIND; break;
    case CM_TYPECK_TYPE_STR: target.kind = CM_HIR_TYPE_STR_KIND; break;
    case CM_TYPECK_TYPE_INTEGER:
        target.kind = CM_HIR_TYPE_INTEGER_KIND;
        target.data.integer_type.kind = source->data.integer_type;
        break;
    case CM_TYPECK_TYPE_FLOAT:
        target.kind = CM_HIR_TYPE_FLOAT_KIND;
        target.data.float_type.kind = source->data.float_type;
        break;
    case CM_TYPECK_TYPE_REFERENCE:
        target.kind = CM_HIR_TYPE_REFERENCE_KIND;
        target.data.reference_type.region = source->data.reference_type.region;
        target.data.reference_type.mutability =
            source->data.reference_type.mutability;
        status = cm_typeck_freeze_inner(state,
            source->data.reference_type.pointee,
            &target.data.reference_type.pointee);
        break;
    case CM_TYPECK_TYPE_RAW_POINTER:
        target.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
        target.data.raw_pointer_type.mutability =
            source->data.raw_pointer_type.mutability;
        status = cm_typeck_freeze_inner(state,
            source->data.raw_pointer_type.pointee,
            &target.data.raw_pointer_type.pointee);
        break;
    case CM_TYPECK_TYPE_TUPLE:
        target.kind = CM_HIR_TYPE_TUPLE_KIND;
        target.data.tuple_type.element_count =
            source->data.tuple_type.element_count;
        if (target.data.tuple_type.element_count != 0u) {
            target.data.tuple_type.elements =
                (CmHirTypeId *)cm_alloc_zeroed(
                    target.data.tuple_type.element_count,
                    sizeof(CmHirTypeId));
        }
        for (index = 0u; index < target.data.tuple_type.element_count;
             ++index) {
            status = cm_typeck_freeze_inner(state,
                source->data.tuple_type.elements[index],
                &target.data.tuple_type.elements[index]);
            if (status != CM_TYPECK_OK) break;
        }
        break;
    case CM_TYPECK_TYPE_ARRAY:
        target.kind = CM_HIR_TYPE_ARRAY_KIND;
        status = cm_typeck_freeze_inner(state,
            source->data.array_type.element, &target.data.array_type.element);
        if (status == CM_TYPECK_OK) status = cm_typeck_freeze_const(state,
            &source->data.array_type.length, &target.data.array_type.length);
        break;
    case CM_TYPECK_TYPE_SLICE:
        target.kind = CM_HIR_TYPE_SLICE_KIND;
        status = cm_typeck_freeze_inner(state,
            source->data.slice_type.element, &target.data.slice_type.element);
        break;
    case CM_TYPECK_TYPE_FN_POINTER:
        if (source->data.fn_pointer_type.binder_lifetime_count != 0u) {
            return CM_TYPECK_UNSUPPORTED_HIR_TYPE;
        }
        target.kind = CM_HIR_TYPE_FN_POINTER_KIND;
        target.data.fn_pointer_type.parameter_count =
            source->data.fn_pointer_type.parameter_count;
        if (target.data.fn_pointer_type.parameter_count != 0u) {
            target.data.fn_pointer_type.parameters =
                (CmHirTypeId *)cm_alloc_zeroed(
                    target.data.fn_pointer_type.parameter_count,
                    sizeof(CmHirTypeId));
        }
        for (index = 0u;
             index < target.data.fn_pointer_type.parameter_count; ++index) {
            status = cm_typeck_freeze_inner(state,
                source->data.fn_pointer_type.parameters[index],
                &target.data.fn_pointer_type.parameters[index]);
            if (status != CM_TYPECK_OK) break;
        }
        if (status == CM_TYPECK_OK) status = cm_typeck_freeze_inner(state,
            source->data.fn_pointer_type.return_type,
            &target.data.fn_pointer_type.return_type);
        target.data.fn_pointer_type.abi = source->data.fn_pointer_type.abi;
        target.data.fn_pointer_type.safety =
            source->data.fn_pointer_type.safety;
        target.data.fn_pointer_type.is_variadic =
            source->data.fn_pointer_type.is_variadic;
        break;
    case CM_TYPECK_TYPE_ADT:
        target.kind = CM_HIR_TYPE_ADT_KIND;
        status = cm_typeck_freeze_named(state, &source->data.named_type,
            &target.data.named_type);
        break;
    case CM_TYPECK_TYPE_PARAMETER:
        target.kind = CM_HIR_TYPE_PARAMETER_KIND;
        target.data.parameter_type.parameter =
            source->data.parameter_type.parameter;
        break;
    case CM_TYPECK_TYPE_PROJECTION:
        target.kind = CM_HIR_TYPE_PROJECTION_KIND;
        status = cm_typeck_freeze_inner(state,
            source->data.projection_type.self_type,
            &target.data.projection_type.self_type);
        if (status == CM_TYPECK_OK) status = cm_typeck_freeze_named(state,
            &source->data.projection_type.trait_type,
            &target.data.projection_type.trait_type);
        if (status == CM_TYPECK_OK) status = cm_typeck_freeze_named(state,
            &source->data.projection_type.associated_type,
            &target.data.projection_type.associated_type);
        break;
    case CM_TYPECK_TYPE_VARIABLE:
    default:
        status = CM_TYPECK_UNRESOLVED;
        break;
    }
    if (status == CM_TYPECK_OK) {
        state->hir_status = cm_hir_add_type(state->hir, &target, out_type);
        if (state->hir_status != CM_HIR_OK) status = CM_TYPECK_HIR_FAILURE;
    }
    cm_typeck_freeze_temp_destroy(&target);
    if (status == CM_TYPECK_OK) {
        state->memo[(size_t)resolved - 1u] = *out_type;
    }
    return status;
}

CmTypeckFreezeResult cm_typeck_freeze_hir_type(CmTypeckContext *context,
    CmTypeckTypeId type, CmHirContext *hir,
    CmHirContextMark *caller_mark)
{
    CmTypeckFreezeResult result;
    CmTypeckState *state;
    CmTypeckFreezeState freeze;
    CmHirContextMark nested_mark;
    size_t initial_type_count;

    memset(&result, 0, sizeof(result));
    result.status = CM_TYPECK_INVALID_ARGUMENT;
    state = cm_typeck_state(context);
    if (!cm_typeck_state_is_current(state)
        || hir == NULL || state->hir != hir
        || !cm_typeck_type_id_valid(state, type)
        || !cm_typeck_caller_mark_valid(hir, caller_mark)) return result;
    result.hir_status = cm_hir_context_mark(hir, &nested_mark);
    if (result.hir_status != CM_HIR_OK) {
        result.status = CM_TYPECK_HIR_FAILURE;
        return result;
    }
    initial_type_count = hir->types.len;
    memset(&freeze, 0, sizeof(freeze));
    freeze.typeck = state;
    freeze.hir = hir;
    freeze.hir_status = CM_HIR_OK;
    freeze.memo = (CmHirTypeId *)cm_alloc_zeroed(state->types.len,
        sizeof(CmHirTypeId));
    result.status = cm_typeck_freeze_inner(&freeze, type, &result.type);
    result.hir_status = freeze.hir_status;
    cm_free(freeze.memo);
    if (result.status == CM_TYPECK_OK) {
        result.added_type_count = hir->types.len - initial_type_count;
        result.hir_status = cm_hir_context_commit(hir, &nested_mark);
        if (result.hir_status != CM_HIR_OK) {
            result.status = CM_TYPECK_HIR_FAILURE;
            result.type = CM_HIR_TYPE_NONE;
            result.added_type_count = 0u;
        }
    } else {
        (void)cm_hir_context_rewind(hir, &nested_mark);
        result.type = CM_HIR_TYPE_NONE;
        result.added_type_count = 0u;
    }
    state->hir_semantic_generation = hir->semantic_generation;
    state->hir_rewind_generation = hir->rewind_generation;
    return result;
}

const char *cm_typeck_status_name(CmTypeckStatus status)
{
    switch (status) {
    case CM_TYPECK_OK: return "ok";
    case CM_TYPECK_INVALID_ARGUMENT: return "invalid-argument";
    case CM_TYPECK_INVALID_ID: return "invalid-id";
    case CM_TYPECK_INVALID_SNAPSHOT: return "invalid-snapshot";
    case CM_TYPECK_UNSUPPORTED_HIR_TYPE: return "unsupported-hir-type";
    case CM_TYPECK_UNSUPPORTED_CONSTANT: return "unsupported-constant";
    case CM_TYPECK_KIND_CONFLICT: return "kind-conflict";
    case CM_TYPECK_TYPE_MISMATCH: return "type-mismatch";
    case CM_TYPECK_OCCURS_CHECK: return "occurs-check";
    case CM_TYPECK_UNRESOLVED: return "unresolved";
    case CM_TYPECK_OVERFLOW: return "overflow";
    case CM_TYPECK_HIR_FAILURE: return "hir-failure";
    }
    return "unknown";
}
