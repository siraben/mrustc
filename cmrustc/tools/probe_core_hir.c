#include "cm/driver/cfg.h"
#include "cm/hir/declaration_capture.h"
#include "cm/hir/library.h"
#include "cm/hir/lower.h"
#include "cm/hir/metadata.h"
#include "cm/sha256.h"

#include <stdio.h>
#include <string.h>

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

static void source_closure_disambiguator(const CmSourceSet *sources,
    char output[69])
{
    static const unsigned char domain[] = "cmrustc-core-probe-sources-v1";
    static const char hex[] = "0123456789abcdef";
    CmSha256 sha;
    unsigned char digest[CM_SHA256_DIGEST_SIZE];
    unsigned char framed_size[8];
    size_t source_index;
    size_t byte_index;

    cm_sha256_init(&sha);
    cm_sha256_update(&sha, domain, sizeof(domain));
    for (source_index = 0u; source_index < sources->length; ++source_index) {
        const CmSourceFile *source = &sources->files[source_index];
        uint64_t length = (uint64_t)source->length;

        for (byte_index = 0u; byte_index < sizeof(framed_size);
                ++byte_index) {
            framed_size[sizeof(framed_size) - 1u - byte_index]
                = (unsigned char)(length & UINT64_C(255));
            length >>= 8u;
        }
        cm_sha256_update(&sha, framed_size, sizeof(framed_size));
        cm_sha256_update(&sha, source->bytes, source->length);
    }
    cm_sha256_final(&sha, digest);
    memcpy(output, "src-", 4u);
    for (byte_index = 0u; byte_index < sizeof(digest); ++byte_index) {
        output[4u + byte_index * 2u] = hex[digest[byte_index] >> 4u];
        output[5u + byte_index * 2u] = hex[digest[byte_index] & 15u];
    }
    output[68] = '\0';
}

static CmSpan declaration_rejection_span(const CmHirContext *hir,
    const CmHirDeclarationCaptureResult *result)
{
    CmSpan span = { 0u, 0u, 0u };
    const CmHirItem *item;
    const CmHirType *type;

    if (result->has_rejected_span) return result->rejected_span;
    if (result->rejected_item != CM_HIR_ITEM_NONE) {
        item = cm_hir_get_item(hir, result->rejected_item);
        if (item != NULL) return item->span;
    }
    if (result->rejected_type != CM_HIR_TYPE_NONE) {
        type = cm_hir_get_type(hir, result->rejected_type);
        if (type != NULL) return type->span;
    }
    return span;
}

static const char *declaration_binding_kind_name(
    const CmHirDeclarationCaptureResult *result)
{
    if (!result->has_rejected_target) return "<none>";
    switch (result->rejected_binding_kind) {
    case CM_HIR_LIBRARY_BINDING_TYPE: return "type";
    case CM_HIR_LIBRARY_BINDING_MODULE: return "module";
    case CM_HIR_LIBRARY_BINDING_TRAIT: return "trait";
    case CM_HIR_LIBRARY_BINDING_PRIMITIVE: return "primitive";
    case CM_HIR_LIBRARY_BINDING_VALUE: return "value";
    case CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR:
        return "struct-constructor";
    }
    return "unknown";
}

static const char *declaration_ast_item_kind_name(
    const CmHirDeclarationCaptureResult *result)
{
    if (!result->has_rejected_binding) return "<none>";
    switch (result->rejected_ast_item_kind) {
    case CM_AST_ITEM_FUNCTION: return "function";
    case CM_AST_ITEM_STRUCT: return "struct";
    case CM_AST_ITEM_ENUM: return "enum";
    case CM_AST_ITEM_TYPE_ALIAS: return "type-alias";
    case CM_AST_ITEM_CONST: return "const";
    case CM_AST_ITEM_STATIC: return "static";
    case CM_AST_ITEM_MODULE: return "module";
    case CM_AST_ITEM_USE: return "use";
    case CM_AST_ITEM_EXTERN_CRATE: return "extern-crate";
    case CM_AST_ITEM_EXTERN_BLOCK: return "extern-block";
    case CM_AST_ITEM_TRAIT: return "trait";
    case CM_AST_ITEM_IMPL: return "impl";
    case CM_AST_ITEM_MACRO: return "macro";
    case CM_AST_ITEM_UNION: return "union";
    }
    return "unknown";
}

static const char *declaration_namespace_name(
    const CmHirDeclarationCaptureResult *result)
{
    if (!result->has_rejected_binding) return "<none>";
    switch (result->rejected_namespace_kind) {
    case CM_RESOLVE_NAMESPACE_TYPE: return "type";
    case CM_RESOLVE_NAMESPACE_VALUE: return "value";
    case CM_RESOLVE_NAMESPACE_MACRO: return "macro";
    }
    return "unknown";
}

static void report_declaration_v3(const CmSourceSet *sources,
    const CmTargetDesc *target, const CmCfgSet *cfg,
    const CmHirContext *hir, CmHirCrateId crate_id,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, const CmHirModuleMap *modules)
{
    CmHirArtifactConfig config;
    CmHirArtifactConfigStatus config_status;
    CmHirDeclarationCaptureInput input;
    CmHirDeclarationCaptureResult result;
    CmHirDeclarationMetadata metadata;
    CmSpan span;
    const CmSourceFile *source;
    char disambiguator[69];
    char layout[64];
    int layout_length;

    cm_hir_artifact_config_init(&config);
    cm_hir_declaration_metadata_init(&metadata);
    config_status = cm_hir_artifact_config_build(target, CM_EDITION_2024,
        CM_HIR_ARTIFACT_PANIC_ABORT, cfg, &config);
    if (config_status != CM_HIR_ARTIFACT_CONFIG_OK) {
        printf("metadata-v3.0 status=probe-config-failure config=%s "
            "rejected_item=0 rejected_type=0 source=<none> line=0\n",
            cm_hir_artifact_config_status_name(config_status));
        goto cleanup;
    }
    /* Diagnostic-only canonical probe descriptor.  This is deliberately not
     * LLVM data-layout or link authority and must not escape this probe. */
    layout_length = snprintf(layout, sizeof(layout),
        "cmrustc-probe-layout-v1:%c-p:%u:%u",
        target->endian == CM_ENDIAN_LITTLE ? 'e' : 'E',
        target->pointer_bits, target->pointer_bits);
    if (layout_length <= 0 || (size_t)layout_length >= sizeof(layout)) {
        printf("metadata-v3.0 status=probe-layout-failure "
            "rejected_item=0 rejected_type=0 source=<none> line=0\n");
        goto cleanup;
    }
    source_closure_disambiguator(sources, disambiguator);
    memset(&input, 0, sizeof(input));
    input.hir = hir;
    input.crate_id = crate_id;
    input.graph = graph;
    input.revision = revision;
    input.imports = imports;
    input.modules = modules;
    input.configuration = &config;
    input.crate_disambiguator.data = disambiguator;
    input.crate_disambiguator.length = 68u;
    input.target_triple.data = target->triple;
    input.target_triple.length = strlen(target->triple);
    input.data_layout.data = layout;
    input.data_layout.length = (size_t)layout_length;
    result = cm_hir_declaration_metadata_capture(&input, &metadata);
    span = declaration_rejection_span(hir, &result);
    source = cm_source_get(sources, span.source);
    printf("metadata-v3.0 status=%s stage=%s reason=%s "
        "rejected_item=%u rejected_type=%u binding=%s ast_item=%s "
        "namespace=%s def=%u:%u source_item=%u:%u source=%s line=%lu\n",
        cm_hir_declaration_capture_status_name(result.status),
        cm_hir_declaration_capture_stage_name(result.failure_stage),
        cm_hir_declaration_capture_reason_name(result.failure_reason),
        (unsigned int)result.rejected_item,
        (unsigned int)result.rejected_type,
        declaration_binding_kind_name(&result),
        declaration_ast_item_kind_name(&result),
        declaration_namespace_name(&result),
        (unsigned int)result.rejected_definition.crate_id,
        (unsigned int)result.rejected_definition.index,
        (unsigned int)result.rejected_source_item.source,
        (unsigned int)result.rejected_source_item.item,
        source == NULL ? "<none>" : source->path,
        (unsigned long)source_line(source, (size_t)span.start));

cleanup:
    cm_hir_declaration_metadata_destroy(&metadata);
    cm_hir_artifact_config_destroy(&config);
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
    const CmSourceFile *related_source;
    size_t line;
    size_t related_line;
    int require_metadata;
    int status;

    if (argc != 2 && (argc != 3
            || strcmp(argv[2], "--require-metadata") != 0)) {
        fprintf(stderr, "usage: %s /path/to/library/core/src/lib.rs "
            "[--require-metadata]\n",
            argc == 0 ? "probe_core_hir" : argv[0]);
        return 2;
    }
    require_metadata = argc == 3;
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
        /* Census the declaration surface the metadata boundary must grow
         * to cover for M6-06: item kinds, generic parameters, and the
         * trait/impl families that stay outside cmhir-meta today. */
        size_t census_index;
        size_t trait_count = 0u;
        size_t impl_count = 0u;
        size_t generic_count = 0u;
        size_t const_generic_count = 0u;
        size_t lifetime_generic_count = 0u;

        for (census_index = 0u; census_index < hir.generic_parameters.len;
             ++census_index) {
            const CmHirGenericParam *census_parameter
                = (const CmHirGenericParam *)cm_vec_at_const(
                    &hir.generic_parameters, census_index);

            if (census_parameter == NULL) continue;
            generic_count += 1u;
            if (census_parameter->kind == CM_HIR_GENERIC_CONST) {
                const_generic_count += 1u;
            } else if (census_parameter->kind
                == CM_HIR_GENERIC_LIFETIME) {
                lifetime_generic_count += 1u;
            }
        }
        for (census_index = 0u; census_index < hir.items.len;
             ++census_index) {
            const CmHirItem *census_item = (const CmHirItem *)
                cm_vec_at_const(&hir.items, census_index);

            if (census_item == NULL) continue;
            if (census_item->kind == CM_HIR_ITEM_TRAIT) {
                trait_count += 1u;
            } else if (census_item->kind == CM_HIR_ITEM_IMPL) {
                impl_count += 1u;
            }
        }
        printf("hir errors=0 items=%lu bodies=%lu types=%lu\n",
            (unsigned long)hir.items.len, (unsigned long)hir.bodies.len,
            (unsigned long)hir.types.len);
        printf("census traits=%lu impls=%lu generics=%lu "
            "const_generics=%lu lifetime_generics=%lu\n",
            (unsigned long)trait_count, (unsigned long)impl_count,
            (unsigned long)generic_count,
            (unsigned long)const_generic_count,
            (unsigned long)lifetime_generic_count);
        (void)fflush(stdout);
        {
            CmHirLibraryArtifact artifact;
            CmHirLibraryArtifactResult library_result;
            CmByteBuf encoded;
            CmHirMetadataArtifactResult metadata_result;

            cm_hir_library_artifact_init(&artifact);
            library_result = cm_hir_library_declaration_artifact_build(
                &artifact, &hir, lower_result.crate_id, &graph,
                graph_result.revision, &modules, "core");
            printf("library status=%s modules=%lu types=%lu values=%lu\n",
                cm_hir_library_status_name(library_result.status),
                (unsigned long)library_result.module_count,
                (unsigned long)library_result.public_type_entry_count,
                (unsigned long)library_result.public_value_entry_count);
            (void)fflush(stdout);
            if (library_result.status == CM_HIR_LIBRARY_OK) {
                cm_byte_buf_init(&encoded);
                metadata_result =
                    cm_hir_metadata_encode_declaration_artifact(&encoded,
                        &artifact);
                printf("metadata-v2.6 status=%s bytes=%lu\n",
                    cm_hir_metadata_artifact_status_name(
                        metadata_result.status),
                    metadata_result.status
                        == CM_HIR_METADATA_ARTIFACT_OK
                        ? (unsigned long)encoded.len : 0ul);
                cm_byte_buf_destroy(&encoded);
                if (require_metadata
                    && metadata_result.status
                        != CM_HIR_METADATA_ARTIFACT_OK) {
                    status = 1;
                }
            } else if (require_metadata) {
                status = 1;
            }
            cm_hir_library_artifact_destroy(&artifact);
        }
        report_declaration_v3(&sources, target, &cfg, &hir,
            lower_result.crate_id, &graph, graph_result.revision, &imports,
            &modules);
        if (!require_metadata || status != 1) status = 0;
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
    if (lower_result.first_error.has_related_span) {
        related_source = cm_source_get(&sources,
            lower_result.first_error.related_span.source);
        related_line = source_line(related_source,
            (size_t)lower_result.first_error.related_span.start);
        printf("hir related_source=%s related_line=%lu related_span=%lu..%lu\n",
            related_source == NULL ? "<none>" : related_source->path,
            (unsigned long)related_line,
            (unsigned long)lower_result.first_error.related_span.start,
            (unsigned long)lower_result.first_error.related_span.end);
    }
    status = 1;

cleanup:
    cm_hir_module_map_destroy(&modules);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return status;
}
