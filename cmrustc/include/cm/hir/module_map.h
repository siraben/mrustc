#ifndef CMRUSTC_CM_HIR_MODULE_MAP_H
#define CMRUSTC_CM_HIR_MODULE_MAP_H

#include "cm/hir/model.h"
#include "cm/resolve/module_graph.h"

typedef enum CmHirModuleMapStatus {
    CM_HIR_MODULE_MAP_OK = 0,
    CM_HIR_MODULE_MAP_INVALID_ARGUMENT,
    CM_HIR_MODULE_MAP_NOT_FOUND,
    CM_HIR_MODULE_MAP_STALE_GRAPH,
    CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT,
    CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT,
    CM_HIR_MODULE_MAP_INVALID_MODULE_ID,
    CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID,
    CM_HIR_MODULE_MAP_DUPLICATE_BINDING,
    CM_HIR_MODULE_MAP_MODULE_CONFLICT,
    CM_HIR_MODULE_MAP_HIR_CONFLICT
} CmHirModuleMapStatus;

typedef struct CmHirModuleMapEntry {
    CmModuleId module;
    CmHirModuleId hir_module;
} CmHirModuleMapEntry;

typedef struct CmHirModuleMap {
    void *state;
} CmHirModuleMap;

void cm_hir_module_map_init(CmHirModuleMap *map);
void cm_hir_module_map_destroy(CmHirModuleMap *map);
void cm_hir_module_map_clear(CmHirModuleMap *map);

/*
 * Both IDs are checked against the supplied owners before insertion.  The
 * first successful binding latches the borrowed graph identity, its revision,
 * and the borrowed HIR context identity until clear or destroy.  No names or
 * hierarchy are compared: the caller explicitly chooses each association.
 * Rebinding either side, including the same pair, is an error and leaves the
 * map unchanged.
 */
CmHirModuleMapStatus cm_hir_module_map_bind(CmHirModuleMap *map,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module,
    const CmHirContext *hir, CmHirModuleId hir_module);

size_t cm_hir_module_map_count(const CmHirModuleMap *map);
/* Process-local object lifetime and monotonic successful-mutation generation. */
uint64_t cm_hir_module_map_lifetime_id(const CmHirModuleMap *map);
uint64_t cm_hir_module_map_generation(const CmHirModuleMap *map);
/* Owner lifetimes latched by the first binding; zero while unbound. */
uint64_t cm_hir_module_map_graph_lifetime_id(const CmHirModuleMap *map);
uint64_t cm_hir_module_map_hir_lifetime_id(const CmHirModuleMap *map);
CmHirModuleMapStatus cm_hir_module_map_lookup_hir(
    const CmHirModuleMap *map, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, CmModuleId module,
    const CmHirContext *hir, CmHirModuleId *out_hir_module);
CmHirModuleMapStatus cm_hir_module_map_lookup_module(
    const CmHirModuleMap *map, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirContext *hir,
    CmHirModuleId hir_module, CmModuleId *out_module);

/* Entries iterate in successful binding order under the latched owners. */
CmHirModuleMapStatus cm_hir_module_map_get(const CmHirModuleMap *map,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmHirContext *hir, size_t index, CmHirModuleMapEntry *out_entry);

const char *cm_hir_module_map_status_name(CmHirModuleMapStatus status);

#endif
