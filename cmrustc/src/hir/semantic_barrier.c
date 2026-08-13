#include "cm/hir/semantic_barrier.h"

#include "cm/alloc.h"
#include "cm/hir/finalization.h"
#include "cm/hir/module_map.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmSemanticBarrierState {
    CmHirContext *hir;
    CmHirCrateId local_crate;
    uint64_t storage_lifetime_id;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    uint64_t capability_id;
    const CmModuleGraph *graph;
    uint64_t graph_lifetime_id;
    CmModuleGraphRevision graph_revision;
    CmModuleId graph_root;
    size_t graph_module_count;
    const CmImportResolver *imports;
    uint64_t imports_lifetime_id;
    uint64_t imports_generation;
    uint64_t imports_graph_lifetime_id;
    const CmHirModuleMap *modules;
    uint64_t modules_lifetime_id;
    uint64_t modules_generation;
    uint64_t modules_graph_lifetime_id;
    uint64_t modules_hir_lifetime_id;
    size_t modules_entry_count;
    CmSemanticBarrierPhase phase;
    size_t crate_count;
    size_t module_count;
    size_t item_count;
    size_t body_count;
    size_t expression_count;
    size_t type_count;
    size_t generic_parameter_count;
    size_t definition_count;
    size_t prebound_associated_type_count;
    CmSemanticAtomView *atoms;
    size_t atom_count;
} CmSemanticBarrierState;

static uint64_t cm_semantic_barrier_capability_counter;

static int cm_semantic_barrier_source_matches(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, const CmHirModuleMap *modules);

static uint64_t cm_semantic_barrier_new_capability_id(void)
{
    if (cm_semantic_barrier_capability_counter == UINT64_MAX) abort();
    cm_semantic_barrier_capability_counter += UINT64_C(1);
    return cm_semantic_barrier_capability_counter;
}

static CmSemanticBarrierResult cm_semantic_barrier_result(
    CmSemanticBarrierStatus status, CmSemanticBarrierPhase phase)
{
    CmSemanticBarrierResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.phase = phase;
    result.atom_index = CM_SEMANTIC_ATOM_INDEX_NONE;
    result.atom.kind = CM_SEMANTIC_ATOM_NONE;
    result.atom.owner = cm_hir_def_id_none();
    result.atom.body = CM_HIR_BODY_NONE;
    result.atom.declared_type = CM_HIR_TYPE_NONE;
    result.local_bodies.status = CM_HIR_LOCAL_BODIES_INVALID_ARGUMENT;
    result.local_bodies.item = CM_HIR_ITEM_NONE;
    result.local_bodies.owner = cm_hir_def_id_none();
    result.local_bodies.body = CM_HIR_BODY_NONE;
    result.local_bodies.body_result.status =
        CM_HIR_BODY_LOWER_INVALID_ARGUMENT;
    result.hir_status = CM_HIR_OK;
    return result;
}

static void cm_semantic_barrier_capture_generation(
    CmSemanticBarrierState *state)
{
    const CmHirContext *hir;

    hir = state->hir;
    state->storage_lifetime_id = hir->storage.lifetime_id;
    state->semantic_generation = hir->semantic_generation;
    state->rewind_generation = hir->rewind_generation;
    state->crate_count = hir->crates.len;
    state->module_count = hir->modules.len;
    state->item_count = hir->items.len;
    state->body_count = hir->bodies.len;
    state->expression_count = hir->expressions.len;
    state->type_count = hir->types.len;
    state->generic_parameter_count = hir->generic_parameters.len;
    state->definition_count = hir->definitions.len;
    state->prebound_associated_type_count =
        hir->prebound_associated_types.len;
}

static int cm_semantic_barrier_state_current(
    const CmSemanticBarrierState *state)
{
    const CmHirContext *hir;
    CmModuleId graph_root;

    if (state == NULL || state->hir == NULL
        || state->local_crate == CM_HIR_CRATE_NONE
        || state->capability_id == UINT64_C(0)
        || state->phase == CM_SEMANTIC_BARRIER_NONE) return 0;
    hir = state->hir;
    graph_root = CM_MODULE_NONE;
    return hir->storage.lifetime_id == state->storage_lifetime_id
        && hir->semantic_generation == state->semantic_generation
        && hir->rewind_generation == state->rewind_generation
        && hir->crates.len == state->crate_count
        && hir->modules.len == state->module_count
        && hir->items.len == state->item_count
        && hir->bodies.len == state->body_count
        && hir->expressions.len == state->expression_count
        && hir->types.len == state->type_count
        && hir->generic_parameters.len
            == state->generic_parameter_count
        && hir->definitions.len == state->definition_count
        && hir->prebound_associated_types.len
            == state->prebound_associated_type_count
        && state->graph != NULL && state->imports != NULL
        && state->modules != NULL
        && state->graph_lifetime_id != UINT64_C(0)
        && cm_module_graph_lifetime_id(state->graph)
            == state->graph_lifetime_id
        && cm_module_graph_revision(state->graph)
            == state->graph_revision
        && cm_module_graph_error_count(state->graph) == 0u
        && cm_module_graph_module_count(state->graph)
            == state->graph_module_count
        && cm_module_graph_get_root(state->graph, &graph_root)
        && graph_root == state->graph_root
        && state->imports_lifetime_id != UINT64_C(0)
        && cm_import_resolver_lifetime_id(state->imports)
            == state->imports_lifetime_id
        && cm_import_resolver_generation(state->imports)
            == state->imports_generation
        && cm_import_resolver_graph_lifetime_id(state->imports)
            == state->imports_graph_lifetime_id
        && cm_import_resolver_matches_graph(state->imports, state->graph)
        && cm_import_resolver_revision(state->imports)
            == state->graph_revision
        && state->modules_lifetime_id != UINT64_C(0)
        && cm_hir_module_map_lifetime_id(state->modules)
            == state->modules_lifetime_id
        && cm_hir_module_map_generation(state->modules)
            == state->modules_generation
        && cm_hir_module_map_graph_lifetime_id(state->modules)
            == state->modules_graph_lifetime_id
        && cm_hir_module_map_hir_lifetime_id(state->modules)
            == state->modules_hir_lifetime_id
        && cm_hir_module_map_count(state->modules)
            == state->modules_entry_count
        && cm_semantic_barrier_source_matches(hir, state->local_crate,
            state->graph, state->graph_revision, state->imports,
            state->modules)
        && cm_hir_get_crate(hir, state->local_crate) != NULL;
}

static CmHirBodyId cm_semantic_barrier_item_body(const CmHirItem *item)
{
    if (item->kind == CM_HIR_ITEM_FUNCTION)
        return item->data.function_item.body;
    if (item->kind == CM_HIR_ITEM_CONST || item->kind == CM_HIR_ITEM_STATIC)
        return item->data.value_item.body;
    return CM_HIR_BODY_NONE;
}

static CmHirTypeId cm_semantic_barrier_item_type(const CmHirItem *item)
{
    if (item->kind == CM_HIR_ITEM_FUNCTION)
        return item->data.function_item.signature.return_type;
    if (item->kind == CM_HIR_ITEM_CONST || item->kind == CM_HIR_ITEM_STATIC)
        return item->data.value_item.type;
    return CM_HIR_TYPE_NONE;
}

static CmSemanticAtomKind cm_semantic_barrier_item_kind(
    const CmHirItem *item)
{
    return item->kind == CM_HIR_ITEM_FUNCTION
        ? CM_SEMANTIC_ATOM_FUNCTION
        : item->kind == CM_HIR_ITEM_CONST
            ? CM_SEMANTIC_ATOM_CONST : CM_SEMANTIC_ATOM_STATIC;
}

static int cm_semantic_barrier_source_matches(
    const CmHirContext *hir, CmHirCrateId local_crate,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, const CmHirModuleMap *modules)
{
    const CmHirCrate *crate_value;
    CmModuleId graph_root;
    CmModuleId mapped_root;
    size_t graph_module_count;
    size_t local_hir_module_count;
    size_t index;

    if (hir == NULL || graph == NULL || imports == NULL || modules == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE
        || !cm_import_resolver_matches_graph(imports, graph)
        || cm_import_resolver_revision(imports) != revision) return 0;
    crate_value = cm_hir_get_crate(hir, local_crate);
    graph_root = CM_MODULE_NONE;
    mapped_root = CM_MODULE_NONE;
    if (crate_value == NULL
        || crate_value->root_module == CM_HIR_MODULE_NONE
        || cm_module_graph_error_count(graph) != 0u
        || cm_module_graph_revision(graph) != revision
        || !cm_module_graph_get_root(graph, &graph_root)
        || graph_root == CM_MODULE_NONE
        || cm_hir_module_map_lookup_module(modules, graph, revision, hir,
            crate_value->root_module, &mapped_root)
            != CM_HIR_MODULE_MAP_OK
        || mapped_root != graph_root) return 0;
    graph_module_count = cm_module_graph_module_count(graph);
    if (cm_hir_module_map_count(modules) != graph_module_count) return 0;
    local_hir_module_count = 0u;
    for (index = 0u; index < hir->modules.len; ++index) {
        const CmHirModule *hir_module;

        hir_module = cm_hir_get_module(hir,
            (CmHirModuleId)(index + 1u));
        if (hir_module == NULL) return 0;
        if (hir_module->crate_id == local_crate) ++local_hir_module_count;
    }
    if (local_hir_module_count != graph_module_count) return 0;
    for (index = 0u; index < graph_module_count; ++index) {
        CmResolveModuleInfo graph_module;
        CmHirModuleId hir_module_id;
        const CmHirModule *hir_module;

        if (!cm_module_graph_get_module_at(graph, index, &graph_module)
            || cm_hir_module_map_lookup_hir(modules, graph, revision,
                graph_module.id, hir, &hir_module_id)
                != CM_HIR_MODULE_MAP_OK) return 0;
        hir_module = cm_hir_get_module(hir, hir_module_id);
        if (hir_module == NULL || hir_module->crate_id != local_crate)
            return 0;
    }
    return crate_value != NULL
        && crate_value->root_module != CM_HIR_MODULE_NONE
        && mapped_root == graph_root;
}

static int cm_semantic_barrier_atom_still_matches(
    const CmSemanticBarrierState *state, size_t index)
{
    const CmSemanticAtomView *atom;
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirBody *body;

    atom = &state->atoms[index];
    definition = cm_hir_lookup_definition(state->hir, atom->owner);
    item = definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(state->hir, definition->entity.item_id);
    body = cm_hir_get_body(state->hir, atom->body);
    return item != NULL && body != NULL
        && item->definition.crate_id == state->local_crate
        && cm_hir_def_id_equal(item->definition, atom->owner)
        && cm_semantic_barrier_item_body(item) == atom->body
        && cm_semantic_barrier_item_type(item) == atom->declared_type
        && cm_semantic_barrier_item_kind(item) == atom->kind
        && cm_hir_def_id_equal(body->owner, atom->owner)
        && body->expected_type == atom->declared_type
        && body->source == atom->source
        && body->source_expression_id == atom->source_expression;
}

CmSemanticBarrierResult cm_semantic_barrier_init_structural(
    CmSemanticBarrier *barrier, CmHirContext *hir,
    CmHirCrateId local_crate, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    CmSemanticBarrierResult result;
    CmHirCrateFinalization finalization;
    CmSemanticBarrierState *state;
    CmHirItemId *body_owners;
    size_t item_index;
    size_t body_index;
    size_t atom_count;

    result = cm_semantic_barrier_result(
        CM_SEMANTIC_BARRIER_INVALID_ARGUMENT,
        CM_SEMANTIC_BARRIER_NONE);
    if (barrier == NULL || barrier->state != NULL || hir == NULL
        || local_crate == CM_HIR_CRATE_NONE || graph == NULL
        || imports == NULL || modules == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) return result;
    if (!cm_semantic_barrier_source_matches(hir, local_crate, graph,
            revision, imports, modules)) {
        result.status = CM_SEMANTIC_BARRIER_SOURCE_MISMATCH;
        return result;
    }
    memset(&finalization, 0, sizeof(finalization));
    result.hir_status = cm_hir_crate_finalization_init(&finalization, hir,
        local_crate);
    if (result.hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_BARRIER_INVALID_HIR;
        return result;
    }
    cm_hir_crate_finalization_destroy(&finalization);
    body_owners = (CmHirItemId *)cm_alloc_zeroed(
        hir->bodies.len == 0u ? 1u : hir->bodies.len,
        sizeof(*body_owners));
    atom_count = 0u;
    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        CmHirBodyId body;

        item = cm_hir_get_item(hir, (CmHirItemId)(item_index + 1u));
        if (item == NULL) goto invalid_hir;
        if (item->definition.crate_id != local_crate) continue;
        body = cm_semantic_barrier_item_body(item);
        if (body == CM_HIR_BODY_NONE) continue;
        if ((size_t)body > hir->bodies.len
            || body_owners[(size_t)body - 1u] != CM_HIR_ITEM_NONE) {
            result.atom_index = atom_count;
            result.atom.owner = item->definition;
            result.atom.body = body;
            goto invalid_hir;
        }
        body_owners[(size_t)body - 1u] =
            (CmHirItemId)(item_index + 1u);
        ++atom_count;
    }
    for (body_index = 0u; body_index < hir->bodies.len; ++body_index) {
        const CmHirBody *body;

        body = cm_hir_get_body(hir, (CmHirBodyId)(body_index + 1u));
        if (body == NULL) goto invalid_hir;
        if (body->owner.crate_id == local_crate
            && body_owners[body_index] == CM_HIR_ITEM_NONE) {
            result.atom.owner = body->owner;
            result.atom.body = (CmHirBodyId)(body_index + 1u);
            goto invalid_hir;
        }
    }
    state = (CmSemanticBarrierState *)cm_alloc_zeroed(1u, sizeof(*state));
    state->atoms = atom_count == 0u ? NULL
        : (CmSemanticAtomView *)cm_alloc_zeroed(atom_count,
            sizeof(*state->atoms));
    state->hir = hir;
    state->local_crate = local_crate;
    state->graph = graph;
    state->graph_lifetime_id = cm_module_graph_lifetime_id(graph);
    state->graph_revision = revision;
    (void)cm_module_graph_get_root(graph, &state->graph_root);
    state->graph_module_count = cm_module_graph_module_count(graph);
    state->imports = imports;
    state->imports_lifetime_id = cm_import_resolver_lifetime_id(imports);
    state->imports_generation = cm_import_resolver_generation(imports);
    state->imports_graph_lifetime_id =
        cm_import_resolver_graph_lifetime_id(imports);
    state->modules = modules;
    state->modules_lifetime_id = cm_hir_module_map_lifetime_id(modules);
    state->modules_generation = cm_hir_module_map_generation(modules);
    state->modules_graph_lifetime_id =
        cm_hir_module_map_graph_lifetime_id(modules);
    state->modules_hir_lifetime_id =
        cm_hir_module_map_hir_lifetime_id(modules);
    state->modules_entry_count = cm_hir_module_map_count(modules);
    state->phase = CM_SEMANTIC_BARRIER_STRUCTURAL;
    state->atom_count = atom_count;
    atom_count = 0u;
    for (item_index = 0u; item_index < hir->items.len; ++item_index) {
        const CmHirItem *item;
        const CmHirBody *body;
        CmHirBodyId body_id;
        CmSemanticAtomView *atom;

        item = cm_hir_get_item(hir, (CmHirItemId)(item_index + 1u));
        if (item == NULL || item->definition.crate_id != local_crate)
            continue;
        body_id = cm_semantic_barrier_item_body(item);
        if (body_id == CM_HIR_BODY_NONE) continue;
        body = cm_hir_get_body(hir, body_id);
        atom = &state->atoms[atom_count];
        atom->kind = cm_semantic_barrier_item_kind(item);
        atom->owner = item->definition;
        atom->body = body_id;
        atom->declared_type = cm_semantic_barrier_item_type(item);
        atom->source = body->source;
        atom->source_expression = body->source_expression_id;
        if (!cm_semantic_barrier_atom_still_matches(state, atom_count)) {
            result.atom_index = atom_count;
            result.atom = *atom;
            cm_free(state->atoms);
            cm_free(state);
            goto invalid_hir;
        }
        ++atom_count;
    }
    cm_semantic_barrier_capture_generation(state);
    state->capability_id = cm_semantic_barrier_new_capability_id();
    barrier->state = state;
    cm_free(body_owners);
    result.status = CM_SEMANTIC_BARRIER_OK;
    result.phase = CM_SEMANTIC_BARRIER_STRUCTURAL;
    return result;

invalid_hir:
    cm_free(body_owners);
    result.status = CM_SEMANTIC_BARRIER_INVALID_HIR;
    return result;
}

CmSemanticBarrierResult cm_semantic_barrier_advance_typed(
    CmSemanticBarrier *barrier, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules)
{
    CmSemanticBarrierState *state;
    CmSemanticBarrierResult result;
    CmHirContextMark mark;
    CmHirBody *journal;
    size_t body_bytes;
    CmHirStatus hir_status;
    size_t index;
    int mark_active;
    uint64_t next_capability_id;

    result = cm_semantic_barrier_result(
        CM_SEMANTIC_BARRIER_INVALID_ARGUMENT,
        CM_SEMANTIC_BARRIER_NONE);
    state = barrier == NULL ? NULL
        : (CmSemanticBarrierState *)barrier->state;
    if (state == NULL || graph == NULL || imports == NULL || modules == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) return result;
    result.phase = state->phase;
    if (!cm_semantic_barrier_state_current(state)) {
        result.status = CM_SEMANTIC_BARRIER_STALE;
        result.phase = CM_SEMANTIC_BARRIER_NONE;
        return result;
    }
    if (state->phase != CM_SEMANTIC_BARRIER_STRUCTURAL) {
        result.status = CM_SEMANTIC_BARRIER_PHASE_ORDER;
        return result;
    }
    if (!cm_semantic_barrier_source_matches(state->hir,
            state->local_crate, graph, revision, imports, modules)) {
        result.status = CM_SEMANTIC_BARRIER_SOURCE_MISMATCH;
        return result;
    }
    if (graph != state->graph || revision != state->graph_revision
        || imports != state->imports || modules != state->modules) {
        result.status = CM_SEMANTIC_BARRIER_SOURCE_MISMATCH;
        return result;
    }
    for (index = 0u; index < state->atom_count; ++index) {
        const CmHirDefinition *definition;
        const CmHirItem *item;

        if (!cm_semantic_barrier_atom_still_matches(state, index)) {
            result.status = CM_SEMANTIC_BARRIER_INVALID_HIR;
            result.atom_index = index;
            result.atom = state->atoms[index];
            return result;
        }
        if (state->atoms[index].kind != CM_SEMANTIC_ATOM_FUNCTION) {
            result.status = CM_SEMANTIC_BARRIER_UNSUPPORTED_ATOM;
            result.atom_index = index;
            result.atom = state->atoms[index];
            return result;
        }
        definition = cm_hir_lookup_definition(state->hir,
            state->atoms[index].owner);
        item = definition == NULL ? NULL : cm_hir_get_item(state->hir,
            definition->entity.item_id);
        if (cm_hir_body_function_owner_kind(state->hir, item)
                == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED) {
            result.status = CM_SEMANTIC_BARRIER_UNSUPPORTED_ATOM;
            result.atom_index = index;
            result.atom = state->atoms[index];
            return result;
        }
    }
    if (!cm_size_mul(state->hir->bodies.len, sizeof(*journal),
            &body_bytes)) {
        result.status = CM_SEMANTIC_BARRIER_HIR_FAILURE;
        result.hir_status = CM_HIR_ID_EXHAUSTED;
        return result;
    }
    journal = body_bytes == 0u ? NULL : (CmHirBody *)cm_alloc(body_bytes);
    if (body_bytes != 0u)
        memcpy(journal, state->hir->bodies.data, body_bytes);
    memset(&mark, 0, sizeof(mark));
    next_capability_id = cm_semantic_barrier_new_capability_id();
    hir_status = cm_hir_context_mark(state->hir, &mark);
    if (hir_status != CM_HIR_OK) {
        cm_free(journal);
        result.status = CM_SEMANTIC_BARRIER_HIR_FAILURE;
        result.hir_status = hir_status;
        return result;
    }
    mark_active = 1;
    result.local_bodies = cm_hir_lower_local_bodies(state->hir,
        state->local_crate, graph, revision, imports, modules);
    if (result.local_bodies.status != CM_HIR_LOCAL_BODIES_OK) {
        result.status = result.local_bodies.status
                == CM_HIR_LOCAL_BODIES_HIR_FAILURE
            ? CM_SEMANTIC_BARRIER_HIR_FAILURE
            : CM_SEMANTIC_BARRIER_BODY_FAILURE;
        result.hir_status = result.local_bodies.hir_status;
        result.atom.owner = result.local_bodies.owner;
        result.atom.body = result.local_bodies.body;
        for (index = 0u; index < state->atom_count; ++index) {
            if (state->atoms[index].body == result.atom.body) {
                result.atom_index = index;
                result.atom = state->atoms[index];
                break;
            }
        }
        goto rollback;
    }
    for (index = 0u; index < state->atom_count; ++index) {
        const CmHirBody *body;

        body = cm_hir_get_body(state->hir, state->atoms[index].body);
        if (body == NULL || body->state != CM_HIR_BODY_TYPED
            || body->root_expression == CM_HIR_EXPR_NONE
            || !cm_semantic_barrier_atom_still_matches(state, index)) {
            result.status = CM_SEMANTIC_BARRIER_INVALID_HIR;
            result.atom_index = index;
            result.atom = state->atoms[index];
            goto rollback;
        }
    }
    hir_status = cm_hir_context_commit(state->hir, &mark);
    if (hir_status != CM_HIR_OK) {
        result.status = CM_SEMANTIC_BARRIER_HIR_FAILURE;
        result.hir_status = hir_status;
        goto rollback;
    }
    mark_active = 0;
    cm_semantic_barrier_capture_generation(state);
    state->capability_id = next_capability_id;
    state->phase = CM_SEMANTIC_BARRIER_TYPED;
    cm_free(journal);
    result.status = CM_SEMANTIC_BARRIER_OK;
    result.phase = CM_SEMANTIC_BARRIER_TYPED;
    return result;

rollback:
    if (mark_active) {
        if (state->hir->bodies.len < state->body_count
            || (body_bytes != 0u && journal == NULL)) {
            hir_status = CM_HIR_INVARIANT_VIOLATION;
        } else {
            if (body_bytes != 0u)
                memcpy(state->hir->bodies.data, journal, body_bytes);
            hir_status = cm_hir_context_rewind(state->hir, &mark);
        }
        if (hir_status != CM_HIR_OK) {
            result.status = CM_SEMANTIC_BARRIER_HIR_FAILURE;
            result.hir_status = hir_status;
        } else {
            cm_semantic_barrier_capture_generation(state);
            state->capability_id = next_capability_id;
        }
    }
    cm_free(journal);
    return result;
}

void cm_semantic_barrier_destroy(CmSemanticBarrier *barrier)
{
    CmSemanticBarrierState *state;

    if (barrier == NULL) return;
    state = (CmSemanticBarrierState *)barrier->state;
    if (state != NULL) {
        cm_free(state->atoms);
        memset(state, 0, sizeof(*state));
        cm_free(state);
    }
    barrier->state = NULL;
}

int cm_semantic_barrier_is_current(const CmSemanticBarrier *barrier)
{
    return barrier != NULL
        && cm_semantic_barrier_state_current(
            (const CmSemanticBarrierState *)barrier->state);
}

CmSemanticBarrierPhase cm_semantic_barrier_phase(
    const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state)
        ? state->phase : CM_SEMANTIC_BARRIER_NONE;
}

const CmHirContext *cm_semantic_barrier_hir(
    const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state) ? state->hir : NULL;
}

CmHirCrateId cm_semantic_barrier_crate(const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state)
        ? state->local_crate : CM_HIR_CRATE_NONE;
}

uint64_t cm_semantic_barrier_generation(const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state)
        ? state->semantic_generation : UINT64_C(0);
}

uint64_t cm_semantic_barrier_capability_id(
    const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state)
        ? state->capability_id : UINT64_C(0);
}

size_t cm_semantic_barrier_atom_count(const CmSemanticBarrier *barrier)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    return cm_semantic_barrier_state_current(state)
        ? state->atom_count : 0u;
}

CmSemanticBarrierStatus cm_semantic_barrier_atom_at(
    const CmSemanticBarrier *barrier, size_t index,
    CmSemanticAtomView *out_atom)
{
    const CmSemanticBarrierState *state;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    if (out_atom == NULL) return CM_SEMANTIC_BARRIER_INVALID_ARGUMENT;
    memset(out_atom, 0, sizeof(*out_atom));
    out_atom->kind = CM_SEMANTIC_ATOM_NONE;
    out_atom->owner = cm_hir_def_id_none();
    if (!cm_semantic_barrier_state_current(state))
        return CM_SEMANTIC_BARRIER_STALE;
    if (index >= state->atom_count)
        return CM_SEMANTIC_BARRIER_INVALID_ARGUMENT;
    *out_atom = state->atoms[index];
    return CM_SEMANTIC_BARRIER_OK;
}

int cm_semantic_barrier_contains_body(const CmSemanticBarrier *barrier,
    CmHirBodyId body, CmSemanticAtomView *out_atom)
{
    const CmSemanticBarrierState *state;
    size_t index;

    state = barrier == NULL ? NULL
        : (const CmSemanticBarrierState *)barrier->state;
    if (out_atom != NULL) {
        memset(out_atom, 0, sizeof(*out_atom));
        out_atom->kind = CM_SEMANTIC_ATOM_NONE;
        out_atom->owner = cm_hir_def_id_none();
    }
    if (!cm_semantic_barrier_state_current(state)
        || body == CM_HIR_BODY_NONE) return 0;
    for (index = 0u; index < state->atom_count; ++index) {
        if (state->atoms[index].body == body) {
            if (out_atom != NULL) *out_atom = state->atoms[index];
            return 1;
        }
    }
    return 0;
}

const char *cm_semantic_barrier_status_name(CmSemanticBarrierStatus status)
{
    switch (status) {
    case CM_SEMANTIC_BARRIER_OK: return "ok";
    case CM_SEMANTIC_BARRIER_INVALID_ARGUMENT: return "invalid argument";
    case CM_SEMANTIC_BARRIER_SOURCE_MISMATCH: return "source mismatch";
    case CM_SEMANTIC_BARRIER_INVALID_HIR: return "invalid HIR";
    case CM_SEMANTIC_BARRIER_UNSUPPORTED_ATOM: return "unsupported atom";
    case CM_SEMANTIC_BARRIER_BODY_FAILURE: return "body failure";
    case CM_SEMANTIC_BARRIER_HIR_FAILURE: return "HIR failure";
    case CM_SEMANTIC_BARRIER_STALE: return "stale";
    case CM_SEMANTIC_BARRIER_PHASE_ORDER: return "phase order";
    }
    return "unknown";
}

const char *cm_semantic_barrier_phase_name(CmSemanticBarrierPhase phase)
{
    switch (phase) {
    case CM_SEMANTIC_BARRIER_NONE: return "none";
    case CM_SEMANTIC_BARRIER_STRUCTURAL: return "structural";
    case CM_SEMANTIC_BARRIER_TYPED: return "typed";
    case CM_SEMANTIC_BARRIER_MARKED: return "marked";
    case CM_SEMANTIC_BARRIER_REGIONS: return "regions";
    case CM_SEMANTIC_BARRIER_REWRITTEN: return "rewritten";
    case CM_SEMANTIC_BARRIER_VALIDATED: return "validated";
    }
    return "unknown";
}
