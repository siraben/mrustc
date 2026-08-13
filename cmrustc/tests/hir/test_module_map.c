#include "cm/hir/module_map.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "hir-module-map: %s\n", message);
        failures += 1;
    }
}

static CmModuleGraphResult build_graph_with_empty_cfg(CmModuleGraph *graph,
    CmSourceSet *sources, CmSourceId root)
{
    CmCfgSet cfg;
    CmModuleGraphOptions options;

    cm_cfg_set_init(&cfg);
    cm_module_graph_options_init(&options);
    options.cfg = &cfg;
    return cm_module_graph_build(graph, sources, root, &options);
}

static int build_graph(CmSourceSet *sources, CmModuleGraph *graph,
    CmModuleGraphResult *result, CmSourceId *out_root)
{
    static const unsigned char source[] =
        "mod alpha {} mod beta { mod nested {} } mod gamma {}\n";
    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    if (cm_source_add_memory(sources, "map/lib.rs", source,
        sizeof(source) - 1u, out_root) != CM_SOURCE_OK) return 0;
    *result = build_graph_with_empty_cfg(graph, sources, *out_root);
    return result->root != CM_MODULE_NONE && result->error_count == 0u &&
        cm_module_graph_module_count(graph) == 5u;
}

static int build_hir(CmHirContext *hir, CmHirModuleId *root,
    CmHirModuleId *beta, CmHirModuleId *alpha, CmHirModuleId *nested)
{
    CmHirCrateId crate_id;
    CmSpan span;
    CmInternId name;

    memset(&span, 0, sizeof(span));
    span.source = 1u;
    span.end = 100u;
    cm_hir_context_init(hir);
    name = cm_hir_intern(hir, "names_are_not_mapping_keys");
    if (cm_hir_create_crate(hir, name, CM_HIR_EDITION_2024, span,
        &crate_id, root) != CM_HIR_OK) return 0;
    if (cm_hir_add_module(hir, crate_id, *root,
        cm_hir_intern(hir, "not_beta"), span, beta) != CM_HIR_OK) return 0;
    if (cm_hir_add_module(hir, crate_id, *root,
        cm_hir_intern(hir, "not_alpha"), span, alpha) != CM_HIR_OK) return 0;
    if (cm_hir_add_module(hir, crate_id, *beta,
        cm_hir_intern(hir, "not_nested"), span, nested) != CM_HIR_OK)
        return 0;
    return 1;
}

static void test_mapping(void)
{
    CmSourceSet sources;
    CmSourceSet other_sources;
    CmSourceId source_root;
    CmSourceId other_source_root;
    CmModuleGraph graph;
    CmModuleGraph other_graph;
    CmModuleGraphResult graph_result;
    CmModuleGraphResult refreshed_result;
    CmModuleGraphResult failed_result;
    CmModuleGraphResult other_graph_result;
    CmHirContext hir;
    CmHirContext other_hir;
    CmHirModuleId hir_root;
    CmHirModuleId hir_beta;
    CmHirModuleId hir_alpha;
    CmHirModuleId hir_nested;
    CmHirModuleId other_hir_root;
    CmHirModuleId other_hir_beta;
    CmHirModuleId other_hir_alpha;
    CmHirModuleId other_hir_nested;
    CmHirModuleMap map;
    CmHirModuleMapEntry entry;
    CmHirModuleId hir_lookup;
    CmModuleId module_lookup;
    CmHirModuleMapStatus status;

    if (!build_graph(&sources, &graph, &graph_result, &source_root)) {
        check(0, "could not build module graph fixture");
        return;
    }
    if (!build_graph(&other_sources, &other_graph, &other_graph_result,
            &other_source_root)) {
        check(0, "could not build second module graph fixture");
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    if (!build_hir(&hir, &hir_root, &hir_beta, &hir_alpha, &hir_nested)) {
        check(0, "could not build HIR fixture");
        cm_module_graph_destroy(&other_graph);
        cm_source_set_destroy(&other_sources);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    if (!build_hir(&other_hir, &other_hir_root, &other_hir_beta,
            &other_hir_alpha, &other_hir_nested)) {
        check(0, "could not build second HIR fixture");
        cm_hir_context_destroy(&hir);
        cm_module_graph_destroy(&other_graph);
        cm_source_set_destroy(&other_sources);
        cm_module_graph_destroy(&graph);
        cm_source_set_destroy(&sources);
        return;
    }
    cm_hir_module_map_init(&map);

    check(cm_hir_module_map_bind(&map, &other_graph,
        other_graph_result.revision, other_graph_result.root, &other_hir,
        99u) == CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID &&
        cm_hir_module_map_count(&map) == 0u,
        "failed first binding latched ownership or mutated the map");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 3u,
        &hir, hir_beta) == CM_HIR_MODULE_MAP_OK,
        "explicit beta binding failed");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        graph_result.root, &hir, hir_root) == CM_HIR_MODULE_MAP_OK,
        "root binding failed");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 4u,
        &hir, hir_nested) == CM_HIR_MODULE_MAP_OK,
        "nested binding failed");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 2u,
        &hir, hir_alpha) == CM_HIR_MODULE_MAP_OK,
        "explicit alpha binding failed");
    check(cm_hir_module_map_count(&map) == 4u,
        "successful binding count differs");

    check(cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
        0u, &entry) == CM_HIR_MODULE_MAP_OK && entry.module == 3u &&
        entry.hir_module == hir_beta,
        "iteration did not preserve first insertion");
    check(cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
        1u, &entry) == CM_HIR_MODULE_MAP_OK &&
        entry.module == graph_result.root && entry.hir_module == hir_root,
        "iteration did not preserve second insertion");
    check(cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
        2u, &entry) == CM_HIR_MODULE_MAP_OK && entry.module == 4u &&
        entry.hir_module == hir_nested,
        "iteration did not preserve third insertion");
    check(cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
        3u, &entry) == CM_HIR_MODULE_MAP_OK && entry.module == 2u &&
        entry.hir_module == hir_alpha,
        "iteration did not preserve fourth insertion");
    entry.module = 99u;
    entry.hir_module = 99u;
    check(cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
        4u, &entry) == CM_HIR_MODULE_MAP_NOT_FOUND &&
        entry.module == CM_MODULE_NONE &&
        entry.hir_module == CM_HIR_MODULE_NONE,
        "out-of-range iteration did not clear output");

    hir_lookup = CM_HIR_MODULE_NONE;
    module_lookup = CM_MODULE_NONE;
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, 2u, &hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_OK && hir_lookup == hir_alpha,
        "forward lookup differs");
    check(cm_hir_module_map_lookup_module(&map, &graph,
        graph_result.revision, &hir, hir_beta, &module_lookup) ==
        CM_HIR_MODULE_MAP_OK && module_lookup == 3u,
        "reverse lookup differs");

    status = cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        3u, &hir, hir_beta);
    check(status == CM_HIR_MODULE_MAP_DUPLICATE_BINDING,
        "duplicate pair was not rejected");
    status = cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        3u, &hir, hir_alpha);
    check(status == CM_HIR_MODULE_MAP_MODULE_CONFLICT,
        "source-module conflict was not rejected");
    status = cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        5u, &hir, hir_beta);
    check(status == CM_HIR_MODULE_MAP_HIR_CONFLICT,
        "HIR-module conflict was not rejected");
    check(cm_hir_module_map_count(&map) == 4u,
        "failed binding mutated the map");

    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision,
        CM_MODULE_NONE, &hir, hir_root) ==
        CM_HIR_MODULE_MAP_INVALID_MODULE_ID,
        "zero module ID was accepted");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 99u,
        &hir, hir_root) == CM_HIR_MODULE_MAP_INVALID_MODULE_ID,
        "out-of-range module ID was accepted");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 1u,
        &hir, CM_HIR_MODULE_NONE) ==
        CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID,
        "zero HIR module ID was accepted");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision, 1u,
        &hir, 99u) == CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID,
        "out-of-range HIR module ID was accepted");
    check(cm_hir_module_map_bind(&map, NULL, graph_result.revision, 1u,
        &hir, hir_root) == CM_HIR_MODULE_MAP_INVALID_ARGUMENT &&
        cm_hir_module_map_bind(&map, &graph, graph_result.revision, 1u,
            NULL, hir_root) == CM_HIR_MODULE_MAP_INVALID_ARGUMENT &&
        cm_hir_module_map_bind(&map, &graph,
            CM_MODULE_GRAPH_REVISION_NONE, 1u, &hir, hir_root) ==
            CM_HIR_MODULE_MAP_INVALID_ARGUMENT,
        "missing owner was not rejected");

    hir_lookup = 99u;
    module_lookup = 99u;
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, CM_MODULE_NONE, &hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_INVALID_MODULE_ID &&
        hir_lookup == CM_HIR_MODULE_NONE,
        "invalid forward lookup did not clear output");
    check(cm_hir_module_map_lookup_module(&map, &graph,
        graph_result.revision, &hir, CM_HIR_MODULE_NONE, &module_lookup) ==
        CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID &&
        module_lookup == CM_MODULE_NONE,
        "invalid reverse lookup did not clear output");
    hir_lookup = 99u;
    module_lookup = 99u;
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, 5u, &hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_NOT_FOUND && hir_lookup == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_lookup_module(&map, &graph,
            graph_result.revision, &hir, 99u, &module_lookup) ==
            CM_HIR_MODULE_MAP_INVALID_HIR_MODULE_ID &&
        module_lookup == CM_MODULE_NONE,
        "missing or invalid lookup did not clear output");

    hir_lookup = 99u;
    check(cm_hir_module_map_lookup_hir(&map, &other_graph,
        other_graph_result.revision, 2u, &hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT &&
        hir_lookup == CM_HIR_MODULE_NONE,
        "wrong graph owner was not rejected with cleared output");
    hir_lookup = 99u;
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, 2u, &other_hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT &&
        hir_lookup == CM_HIR_MODULE_NONE,
        "wrong HIR owner was not rejected with cleared output");
    check(cm_hir_module_map_count(&map) == 4u,
        "owner-conflicting operations mutated the map");
    check(cm_hir_module_map_bind(&map, &other_graph,
        other_graph_result.revision, other_graph_result.root, &hir,
        hir_root) == CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT &&
        cm_hir_module_map_count(&map) == 4u,
        "wrong graph owner binding mutated the map");
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, 2u, &hir, NULL) ==
        CM_HIR_MODULE_MAP_INVALID_ARGUMENT &&
        cm_hir_module_map_lookup_module(&map, &graph,
            graph_result.revision, &hir, hir_alpha, NULL) ==
            CM_HIR_MODULE_MAP_INVALID_ARGUMENT,
        "missing lookup output was accepted");

    refreshed_result = build_graph_with_empty_cfg(&graph, &sources,
        source_root);
    check(refreshed_result.error_count == 0u &&
        refreshed_result.revision != graph_result.revision,
        "fixture rebuild did not advance the graph revision");
    hir_lookup = 99u;
    check(cm_hir_module_map_lookup_hir(&map, &graph,
        graph_result.revision, 2u, &hir, &hir_lookup) ==
        CM_HIR_MODULE_MAP_STALE_GRAPH && hir_lookup == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_get(&map, &graph, graph_result.revision, &hir,
            0u, &entry) == CM_HIR_MODULE_MAP_STALE_GRAPH &&
        entry.module == CM_MODULE_NONE &&
        entry.hir_module == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_bind(&map, &graph, refreshed_result.revision, 5u,
            &hir, hir_root) == CM_HIR_MODULE_MAP_STALE_GRAPH &&
        cm_hir_module_map_count(&map) == 4u,
        "graph rebuild did not stale the latched map without mutation");

    cm_hir_module_map_clear(&map);
    check(cm_hir_module_map_count(&map) == 0u &&
        cm_hir_module_map_bind(&map, &graph, refreshed_result.revision,
            refreshed_result.root, &other_hir, other_hir_root) ==
            CM_HIR_MODULE_MAP_OK &&
        cm_hir_module_map_lookup_hir(&map, &graph,
            refreshed_result.revision, refreshed_result.root, &other_hir,
            &hir_lookup) == CM_HIR_MODULE_MAP_OK &&
        hir_lookup == other_hir_root,
        "clear did not reset ownership for a refreshed graph");

    failed_result = cm_module_graph_build(&graph, NULL, source_root, NULL);
    hir_lookup = 99u;
    module_lookup = 99u;
    entry.module = 99u;
    entry.hir_module = 99u;
    check(failed_result.error_count != 0u &&
        cm_module_graph_error_count(&graph) != 0u &&
        cm_hir_module_map_lookup_hir(&map, &graph,
            failed_result.revision, refreshed_result.root, &other_hir,
            &hir_lookup) == CM_HIR_MODULE_MAP_STALE_GRAPH &&
        hir_lookup == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_lookup_module(&map, &graph,
            failed_result.revision, &other_hir, other_hir_root,
            &module_lookup) == CM_HIR_MODULE_MAP_STALE_GRAPH &&
        module_lookup == CM_MODULE_NONE &&
        cm_hir_module_map_get(&map, &graph, failed_result.revision,
            &other_hir, 0u, &entry) == CM_HIR_MODULE_MAP_STALE_GRAPH &&
        entry.module == CM_MODULE_NONE &&
        entry.hir_module == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_count(&map) == 1u,
        "failed graph build exposed stale module-map entries");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&other_hir);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&other_graph);
    cm_source_set_destroy(&other_sources);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_invalid_map(void)
{
    CmHirModuleMap map;
    CmHirModuleMapEntry entry;
    CmHirModuleId hir_module;

    memset(&map, 0, sizeof(map));
    hir_module = 99u;
    entry.module = 99u;
    entry.hir_module = 99u;
    check(cm_hir_module_map_count(&map) == 0u &&
        cm_hir_module_map_lookup_hir(&map, NULL,
            CM_MODULE_GRAPH_REVISION_NONE, 1u, NULL, &hir_module) ==
            CM_HIR_MODULE_MAP_INVALID_ARGUMENT &&
        hir_module == CM_HIR_MODULE_NONE &&
        cm_hir_module_map_get(&map, NULL,
            CM_MODULE_GRAPH_REVISION_NONE, NULL, 0u, &entry) ==
            CM_HIR_MODULE_MAP_INVALID_ARGUMENT &&
        entry.module == CM_MODULE_NONE &&
        entry.hir_module == CM_HIR_MODULE_NONE,
        "uninitialized map queries did not fail cleanly");
    cm_hir_module_map_destroy(&map);
    check(strcmp(cm_hir_module_map_status_name(
        CM_HIR_MODULE_MAP_DUPLICATE_BINDING), "duplicate binding") == 0 &&
        strcmp(cm_hir_module_map_status_name(
        CM_HIR_MODULE_MAP_HIR_CONFLICT), "HIR module conflict") == 0 &&
        strcmp(cm_hir_module_map_status_name(
        CM_HIR_MODULE_MAP_STALE_GRAPH), "stale graph") == 0 &&
        strcmp(cm_hir_module_map_status_name(
        CM_HIR_MODULE_MAP_GRAPH_OWNER_CONFLICT),
        "graph owner conflict") == 0 &&
        strcmp(cm_hir_module_map_status_name(
        CM_HIR_MODULE_MAP_HIR_OWNER_CONFLICT), "HIR owner conflict") == 0,
        "status names differ");
}

static void test_map_lifetime_and_generation(void)
{
    CmSourceSet sources;
    CmSourceId source_root;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmHirContext hir;
    CmHirModuleId hir_root;
    CmHirModuleId hir_beta;
    CmHirModuleId hir_alpha;
    CmHirModuleId hir_nested;
    CmHirModuleMap map;
    uint64_t first_lifetime;

    check(build_graph(&sources, &graph, &graph_result, &source_root),
        "could not build map identity graph fixture");
    check(build_hir(&hir, &hir_root, &hir_beta, &hir_alpha, &hir_nested),
        "could not build map identity HIR fixture");
    cm_hir_module_map_init(&map);
    first_lifetime = cm_hir_module_map_lifetime_id(&map);
    check(first_lifetime != UINT64_C(0)
        && cm_hir_module_map_generation(&map) == UINT64_C(0),
        "new map did not publish its lifetime and zero generation");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision,
            graph_result.root, &hir, hir_root) == CM_HIR_MODULE_MAP_OK
        && cm_hir_module_map_generation(&map) == UINT64_C(1)
        && cm_hir_module_map_graph_lifetime_id(&map)
            == cm_module_graph_lifetime_id(&graph)
        && cm_hir_module_map_hir_lifetime_id(&map)
            == hir.storage.lifetime_id,
        "successful bind did not advance and latch owner identities");
    check(cm_hir_module_map_bind(&map, &graph, graph_result.revision,
            graph_result.root, &hir, hir_root)
            == CM_HIR_MODULE_MAP_DUPLICATE_BINDING
        && cm_hir_module_map_generation(&map) == UINT64_C(1),
        "failed bind advanced the map generation");
    cm_hir_module_map_clear(&map);
    check(cm_hir_module_map_generation(&map) == UINT64_C(2)
        && cm_hir_module_map_graph_lifetime_id(&map) == UINT64_C(0)
        && cm_hir_module_map_hir_lifetime_id(&map) == UINT64_C(0),
        "clear did not advance and release owner identities");
    cm_hir_module_map_destroy(&map);
    check(cm_hir_module_map_lifetime_id(&map) == UINT64_C(0)
        && cm_hir_module_map_generation(&map) == UINT64_C(0),
        "destroyed map retained a public identity");
    cm_hir_module_map_init(&map);
    check(cm_hir_module_map_lifetime_id(&map) != UINT64_C(0)
        && cm_hir_module_map_lifetime_id(&map) != first_lifetime,
        "reinitialized map reused its process lifetime identity");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

int main(void)
{
    test_mapping();
    test_invalid_map();
    test_map_lifetime_and_generation();
    if (failures == 0) puts("HIR module map tests: ok");
    return failures == 0 ? 0 : 1;
}
