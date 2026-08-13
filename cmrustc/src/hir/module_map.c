#include "cm/hir/module_map.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmHirModuleMapState {
    uint64_t lifetime_id;
    uint64_t generation;
    const CmModuleGraph *graph_owner;
    uint64_t graph_lifetime_id;
    CmModuleGraphRevision graph_revision;
    const CmHirContext *hir_owner;
    uint64_t hir_lifetime_id;
    CmVec entries;
} CmHirModuleMapState;

static uint64_t cm_hir_module_map_lifetime_counter;

static uint64_t cm_hir_module_map_new_lifetime_id(void)
{
    if (cm_hir_module_map_lifetime_counter == UINT64_MAX) abort();
    cm_hir_module_map_lifetime_counter += UINT64_C(1);
    return cm_hir_module_map_lifetime_counter;
}

static CmHirModuleMapState *cm_module_map_state(CmHirModuleMap *map)
{
    return map == NULL ? NULL : (CmHirModuleMapState *)map->state;
}

static const CmHirModuleMapState *cm_module_map_state_const(
    const CmHirModuleMap *map)
{
    return map == NULL ? NULL :
        (const CmHirModuleMapState *)map->state;
}

void cm_hir_module_map_init(CmHirModuleMap *map)
{
    CmHirModuleMapState *state;

    if (map == NULL) return;
    state = (CmHirModuleMapState *)cm_alloc_zeroed(1u, sizeof(*state));
    cm_vec_init(&state->entries, sizeof(CmHirModuleMapEntry));
    state->lifetime_id = cm_hir_module_map_new_lifetime_id();
    map->state = state;
}

void cm_hir_module_map_destroy(CmHirModuleMap *map)
{
    CmHirModuleMapState *state;

    state = cm_module_map_state(map);
    if (state == NULL) return;
    cm_vec_destroy(&state->entries);
    cm_free(state);
    map->state = NULL;
}

void cm_hir_module_map_clear(CmHirModuleMap *map)
{
    CmHirModuleMapState *state;

    state = cm_module_map_state(map);
    if (state == NULL) return;
    if (state->generation == UINT64_MAX) abort();
    state->generation += UINT64_C(1);
    cm_vec_clear(&state->entries);
    state->graph_owner = NULL;
    state->graph_lifetime_id = UINT64_C(0);
    state->graph_revision = CM_MODULE_GRAPH_REVISION_NONE;
    state->hir_owner = NULL;
    state->hir_lifetime_id = UINT64_C(0);
}

static CmHirModuleMapStatus cm_module_map_validate_owners(
    const CmHirModuleMapState *state, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirContext *hir)
{
    if (state == NULL || graph == NULL || hir == NULL
        || revision == CM_MODULE_GRAPH_REVISION_NONE) {
        return CM_HIR_MODULE_MAP_INVALID_ARGUMENT;
    }
    if (cm_module_graph_revision(graph) != revision)
        return CM_HIR_MODULE_MAP_STALE_GRAPH;
    if (cm_module_graph_error_count(graph) != 0u)
        return CM_HIR_MODULE_MAP_STALE_GRAPH;
    if (state->graph_owner != NULL && state->graph_owner != graph)
        return CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT;
    if (state->graph_owner != NULL
        && state->graph_lifetime_id != cm_module_graph_lifetime_id(graph))
        return CM_HIR_MODULE_MAP_STALE_GRAPH;
    if (state->graph_owner != NULL && state->graph_revision != revision)
        return CM_HIR_MODULE_MAP_STALE_GRAPH;
    if (state->hir_owner != NULL && state->hir_owner != hir)
        return CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT;
    if (state->hir_owner != NULL
        && state->hir_lifetime_id != hir->storage.lifetime_id)
        return CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT;
    return CM_HIR_MODULE_MAP_OK;
}

static const CmHirModuleMapEntry *cm_module_map_find_module(
    const CmHirModuleMapState *state, CmModuleId module)
{
    size_t index;

    for (index = 0u; index < state->entries.len; ++index) {
        const CmHirModuleMapEntry *entry;

        entry = (const CmHirModuleMapEntry *)cm_vec_at_const(
            &state->entries, index);
        if (entry != NULL && entry->module == module) return entry;
    }
    return NULL;
}

static const CmHirModuleMapEntry *cm_module_map_find_hir(
    const CmHirModuleMapState *state, CmHirModuleId hir_module)
{
    size_t index;

    for (index = 0u; index < state->entries.len; ++index) {
        const CmHirModuleMapEntry *entry;

        entry = (const CmHirModuleMapEntry *)cm_vec_at_const(
            &state->entries, index);
        if (entry != NULL && entry->hir_module == hir_module) return entry;
    }
    return NULL;
}

CmHirModuleMapStatus cm_hir_module_map_bind(CmHirModuleMap *map,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module,
    const CmHirContext *hir, CmHirModuleId hir_module)
{
    CmHirModuleMapState *state;
    CmResolveModuleInfo module_info;
    const CmHirModuleMapEntry *by_module;
    const CmHirModuleMapEntry *by_hir;
    CmHirModuleMapEntry entry;
    CmHirModuleMapStatus status;

    state = cm_module_map_state(map);
    status = cm_module_map_validate_owners(state, graph, revision, hir);
    if (status != CM_HIR_MODULE_MAP_OK) return status;
    if (module == CM_MODULE_NONE ||
        !cm_module_graph_get_module(graph, module, &module_info))
        return CM_HIR_MODULE_MAP_INVALID_MODULE_ID;
    if (hir_module == CM_HIR_MODULE_NONE ||
        cm_hir_get_module(hir, hir_module) == NULL)
        return CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID;
    by_module = cm_module_map_find_module(state, module);
    by_hir = cm_module_map_find_hir(state, hir_module);
    if (by_module != NULL && by_hir != NULL &&
        by_module == by_hir) return CM_HIR_MODULE_MAP_DUPLICATE_BINDING;
    if (by_module != NULL) return CM_HIR_MODULE_MAP_MODULE_CONFLICT;
    if (by_hir != NULL) return CM_HIR_MODULE_MAP_HIR_CONFLICT;
    entry.module = module;
    entry.hir_module = hir_module;
    if (state->graph_owner == NULL) {
        if (state->generation == UINT64_MAX) abort();
        state->graph_owner = graph;
        state->graph_lifetime_id = cm_module_graph_lifetime_id(graph);
        state->graph_revision = revision;
        state->hir_owner = hir;
        state->hir_lifetime_id = hir->storage.lifetime_id;
    } else if (state->generation == UINT64_MAX) {
        abort();
    }
    (void)cm_vec_push(&state->entries, &entry);
    state->generation += UINT64_C(1);
    return CM_HIR_MODULE_MAP_OK;
}

size_t cm_hir_module_map_count(const CmHirModuleMap *map)
{
    const CmHirModuleMapState *state;

    state = cm_module_map_state_const(map);
    return state == NULL ? 0u : state->entries.len;
}

uint64_t cm_hir_module_map_lifetime_id(const CmHirModuleMap *map)
{
    const CmHirModuleMapState *state;

    state = cm_module_map_state_const(map);
    return state == NULL ? UINT64_C(0) : state->lifetime_id;
}

uint64_t cm_hir_module_map_generation(const CmHirModuleMap *map)
{
    const CmHirModuleMapState *state;

    state = cm_module_map_state_const(map);
    return state == NULL ? UINT64_C(0) : state->generation;
}

uint64_t cm_hir_module_map_graph_lifetime_id(const CmHirModuleMap *map)
{
    const CmHirModuleMapState *state;

    state = cm_module_map_state_const(map);
    return state == NULL ? UINT64_C(0) : state->graph_lifetime_id;
}

uint64_t cm_hir_module_map_hir_lifetime_id(const CmHirModuleMap *map)
{
    const CmHirModuleMapState *state;

    state = cm_module_map_state_const(map);
    return state == NULL ? UINT64_C(0) : state->hir_lifetime_id;
}

CmHirModuleMapStatus cm_hir_module_map_lookup_hir(
    const CmHirModuleMap *map, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module,
    const CmHirContext *hir, CmHirModuleId *out_hir_module)
{
    const CmHirModuleMapState *state;
    const CmHirModuleMapEntry *entry;
    CmHirModuleMapStatus status;
    CmResolveModuleInfo module_info;

    if (out_hir_module != NULL) *out_hir_module = CM_HIR_MODULE_NONE;
    state = cm_module_map_state_const(map);
    if (out_hir_module == NULL) return CM_HIR_MODULE_MAP_INVALID_ARGUMENT;
    status = cm_module_map_validate_owners(state, graph, revision, hir);
    if (status != CM_HIR_MODULE_MAP_OK) return status;
    if (module == CM_MODULE_NONE
        || !cm_module_graph_get_module(graph, module, &module_info))
        return CM_HIR_MODULE_MAP_INVALID_MODULE_ID;
    entry = cm_module_map_find_module(state, module);
    if (entry == NULL) return CM_HIR_MODULE_MAP_NOT_FOUND;
    *out_hir_module = entry->hir_module;
    return CM_HIR_MODULE_MAP_OK;
}

CmHirModuleMapStatus cm_hir_module_map_lookup_module(
    const CmHirModuleMap *map, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirContext *hir,
    CmHirModuleId hir_module, CmModuleId *out_module)
{
    const CmHirModuleMapState *state;
    const CmHirModuleMapEntry *entry;
    CmHirModuleMapStatus status;

    if (out_module != NULL) *out_module = CM_MODULE_NONE;
    state = cm_module_map_state_const(map);
    if (out_module == NULL) return CM_HIR_MODULE_MAP_INVALID_ARGUMENT;
    status = cm_module_map_validate_owners(state, graph, revision, hir);
    if (status != CM_HIR_MODULE_MAP_OK) return status;
    if (hir_module == CM_HIR_MODULE_NONE
        || cm_hir_get_module(hir, hir_module) == NULL)
        return CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID;
    entry = cm_module_map_find_hir(state, hir_module);
    if (entry == NULL) return CM_HIR_MODULE_MAP_NOT_FOUND;
    *out_module = entry->module;
    return CM_HIR_MODULE_MAP_OK;
}

CmHirModuleMapStatus cm_hir_module_map_get(const CmHirModuleMap *map,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmHirContext *hir, size_t index, CmHirModuleMapEntry *out_entry)
{
    const CmHirModuleMapState *state;
    const CmHirModuleMapEntry *entry;
    CmHirModuleMapStatus status;

    if (out_entry != NULL) memset(out_entry, 0, sizeof(*out_entry));
    state = cm_module_map_state_const(map);
    if (out_entry == NULL) return CM_HIR_MODULE_MAP_INVALID_ARGUMENT;
    status = cm_module_map_validate_owners(state, graph, revision, hir);
    if (status != CM_HIR_MODULE_MAP_OK) return status;
    entry = (const CmHirModuleMapEntry *)cm_vec_at_const(&state->entries,
        index);
    if (entry == NULL) return CM_HIR_MODULE_MAP_NOT_FOUND;
    *out_entry = *entry;
    return CM_HIR_MODULE_MAP_OK;
}

const char *cm_hir_module_map_status_name(CmHirModuleMapStatus status)
{
    switch (status) {
    case CM_HIR_MODULE_MAP_OK: return "ok";
    case CM_HIR_MODULE_MAP_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_MODULE_MAP_NOT_FOUND: return "not found";
    case CM_HIR_MODULE_MAP_STALE_GRAPH: return "stale graph";
    case CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT:
        return "graph owner conflict";
    case CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT:
        return "HIR owner conflict";
    case CM_HIR_MODULE_MAP_INVALID_MODULE_ID: return "invalid module ID";
    case CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID:
        return "invalid HIR module ID";
    case CM_HIR_MODULE_MAP_DUPLICATE_BINDING: return "duplicate binding";
    case CM_HIR_MODULE_MAP_MODULE_CONFLICT: return "module conflict";
    case CM_HIR_MODULE_MAP_HIR_CONFLICT: return "HIR module conflict";
    }
    return "unknown module-map status";
}
