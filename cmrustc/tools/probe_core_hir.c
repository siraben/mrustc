#include "cm/driver/cfg.h"
#include "cm/hir/lower.h"

#include <stdio.h>

static size_t source_line(const CmSourceFile *source, size_t offset)
{
    size_t index;
    size_t line;

    if (source == NULL) return 0u;
    line = 1u;
    for (index = 0u; index < offset && index < source->length; ++index) {
        if (source->bytes[index] == (unsigned char)'\n') line += 1u;
    }
    return line;
}

int main(int argc, char **argv)
{
    CmSourceSet sources;
    CmSourceId root;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmCfgSet cfg;
    const CmTargetDesc *target;
    CmHirContext hir;
    CmHirModuleMap modules;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmSourceFile *error_source;
    size_t line;
    int status;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/library/core/src/lib.rs\n",
            argc == 0 ? "probe_core_hir" : argv[0]);
        return 2;
    }
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_import_resolver_init(&imports);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&modules);
    status = 2;
    target = cm_target_find("x86_64-unknown-linux-gnu");
    if (target == NULL || !cm_target_cfg_set(&cfg, target)
        || cm_source_load_file(&sources, argv[1], &root) != CM_SOURCE_OK) {
        fputs("core HIR probe setup failed\n", stderr);
        goto cleanup;
    }
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_options.edition = CM_EDITION_2024;
    graph_result = cm_module_graph_build(&graph, &sources, root,
        &graph_options);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    printf("graph errors=%lu imports=%lu sources=%lu modules=%lu\n",
        (unsigned long)graph_result.error_count,
        (unsigned long)import_result.error_count,
        (unsigned long)sources.length,
        (unsigned long)cm_module_graph_module_count(&graph));
    if (graph_result.error_count != 0u || import_result.error_count != 0u) {
        status = 1;
        goto cleanup;
    }
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "core";
    lower_options.source = root;
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &modules, &lower_options);
    if (lower_result.error_count == 0u) {
        printf("hir errors=0 items=%lu bodies=%lu types=%lu\n",
            (unsigned long)hir.items.len, (unsigned long)hir.bodies.len,
            (unsigned long)hir.types.len);
        status = 0;
        goto cleanup;
    }
    error_source = cm_source_get(&sources,
        lower_result.first_error.span.source);
    line = source_line(error_source,
        (size_t)lower_result.first_error.span.start);
    printf("hir errors=%lu kind=%s source=%s line=%lu item=%u "
        "span=%lu..%lu message=%s\n",
        (unsigned long)lower_result.error_count,
        cm_hir_lower_error_kind_name(lower_result.first_error.kind),
        error_source == NULL ? "<none>" : error_source->path,
        (unsigned long)line,
        (unsigned int)lower_result.first_error.item,
        (unsigned long)lower_result.first_error.span.start,
        (unsigned long)lower_result.first_error.span.end,
        lower_result.first_error.message);
    status = 1;

cleanup:
    cm_hir_module_map_destroy(&modules);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return status;
}
