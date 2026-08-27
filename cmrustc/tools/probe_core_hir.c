#include "cm/driver/cfg.h"
#include "cm/hir/declaration_capture.h"
#include "cm/hir/library.h"
#include "cm/hir/lower.h"
#include "cm/resolve/body_expand.h"
#include "cm/hir/module_map.h"
#include "cm/hir/metadata.h"
#include "cm/sha256.h"

#include <stdio.h>
#include <stdlib.h>
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
    case CM_HIR_LIBRARY_BINDING_ENUM_VARIANT:
        return "enum-variant";
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
    input.target_pointer_bits = target->pointer_bits;
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


/* ---- body census: AST expression kinds and macro invocations per body ---- */

#define CENSUS_MAX_KINDS 64u
#define CENSUS_MAX_MACROS 512u

typedef struct CensusMacro {
    char name[64];
    unsigned long count;
    unsigned long bodies;
} CensusMacro;

typedef struct Census {
    unsigned long expr_kinds[CENSUS_MAX_KINDS];
    unsigned long stmt_let;
    unsigned long stmt_let_else;
    unsigned long stmt_item;
    unsigned long bodies;
    unsigned long bodies_with_ast;
    unsigned long bodies_with_macros;
    unsigned long body_macro_seen;
    CensusMacro macros[CENSUS_MAX_MACROS];
    size_t macro_count;
    unsigned long current_body_macros;
} Census;

static const char *const census_kind_names[] = {
    "literal", "path", "qualified_path", "block", "call", "method_call",
    "field", "tuple_field", "index", "unary", "binary", "assign", "cast",
    "try", "try_block", "range", "let", "return", "break", "continue", "if",
    "match", "loop", "while", "for", "closure", "tuple", "array", "struct",
    "macro", "raw_reference"
};

static void census_macro(Census *census, const CmAst *ast,
    const CmAstMacroInvocation *invocation)
{
    const CmAstPath *path = cm_ast_get_path(ast, invocation->path);
    const CmInternedString *name = NULL;
    char text[64];
    size_t index;
    size_t length;
    if (path != NULL && path->segment_count != 0u && path->segments != NULL)
        name = cm_ast_get_string(ast,
            path->segments[path->segment_count - 1u].name);
    if (name == NULL) {
        strcpy(text, "<unknown>");
    } else {
        length = name->len < sizeof(text) - 1u ? name->len : sizeof(text) - 1u;
        memcpy(text, name->bytes, length);
        text[length] = '\0';
    }
    census->current_body_macros += 1u;
    for (index = 0u; index < census->macro_count; ++index) {
        if (strcmp(census->macros[index].name, text) == 0) {
            census->macros[index].count += 1u;
            if (census->macros[index].bodies != census->bodies) {
                census->macros[index].bodies = census->bodies;
            }
            return;
        }
    }
    if (census->macro_count == CENSUS_MAX_MACROS) return;
    strcpy(census->macros[census->macro_count].name, text);
    census->macros[census->macro_count].count = 1u;
    census->macros[census->macro_count].bodies = census->bodies;
    census->macro_count += 1u;
}

static void census_expr(Census *census, const CmAst *ast, CmAstExprId id);

static void census_stmt(Census *census, const CmAst *ast, CmAstStmtId id)
{
    const CmAstStmt *stmt = cm_ast_get_stmt(ast, id);
    if (stmt == NULL) return;
    switch (stmt->kind) {
    case CM_AST_STMT_LET:
        census->stmt_let += 1u;
        if (stmt->data.let_stmt.else_block != CM_AST_EXPR_NONE)
            census->stmt_let_else += 1u;
        census_expr(census, ast, stmt->data.let_stmt.initializer);
        census_expr(census, ast, stmt->data.let_stmt.else_block);
        break;
    case CM_AST_STMT_EXPR:
        census_expr(census, ast, stmt->data.expr_stmt.expression);
        break;
    case CM_AST_STMT_ITEM:
        census->stmt_item += 1u;
        break;
    }
}

static void census_expr(Census *census, const CmAst *ast, CmAstExprId id)
{
    const CmAstExpr *expr = cm_ast_get_expr(ast, id);
    uint32_t index;
    if (expr == NULL) return;
    if ((unsigned int)expr->kind < CENSUS_MAX_KINDS)
        census->expr_kinds[expr->kind] += 1u;
    switch (expr->kind) {
    case CM_AST_EXPR_BLOCK:
    case CM_AST_EXPR_TRY_BLOCK:
        for (index = 0u; index < expr->data.block.statement_count; ++index)
            census_stmt(census, ast, expr->data.block.statements[index]);
        census_expr(census, ast, expr->data.block.tail);
        break;
    case CM_AST_EXPR_CALL:
        census_expr(census, ast, expr->data.call.callee);
        for (index = 0u; index < expr->data.call.argument_count; ++index)
            census_expr(census, ast, expr->data.call.arguments[index]);
        break;
    case CM_AST_EXPR_METHOD_CALL:
        census_expr(census, ast, expr->data.method_call.receiver);
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            census_expr(census, ast,
                expr->data.method_call.arguments[index]);
        break;
    case CM_AST_EXPR_FIELD:
        census_expr(census, ast, expr->data.field.base);
        break;
    case CM_AST_EXPR_TUPLE_FIELD:
        census_expr(census, ast, expr->data.tuple_field.base);
        break;
    case CM_AST_EXPR_INDEX:
        census_expr(census, ast, expr->data.index.base);
        census_expr(census, ast, expr->data.index.index);
        break;
    case CM_AST_EXPR_UNARY:
        census_expr(census, ast, expr->data.unary.operand);
        break;
    case CM_AST_EXPR_RAW_REFERENCE:
        census_expr(census, ast, expr->data.raw_reference.operand);
        break;
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN:
        census_expr(census, ast, expr->data.binary.left);
        census_expr(census, ast, expr->data.binary.right);
        break;
    case CM_AST_EXPR_CAST:
        census_expr(census, ast, expr->data.cast.value);
        break;
    case CM_AST_EXPR_TRY:
        census_expr(census, ast, expr->data.try_expr.operand);
        break;
    case CM_AST_EXPR_RANGE:
        census_expr(census, ast, expr->data.range.start);
        census_expr(census, ast, expr->data.range.end);
        break;
    case CM_AST_EXPR_LET:
        census_expr(census, ast, expr->data.let_expr.initializer);
        break;
    case CM_AST_EXPR_RETURN:
    case CM_AST_EXPR_BREAK:
    case CM_AST_EXPR_CONTINUE:
        census_expr(census, ast, expr->data.flow.value);
        break;
    case CM_AST_EXPR_IF:
        census_expr(census, ast, expr->data.if_expr.condition);
        census_expr(census, ast, expr->data.if_expr.then_expr);
        census_expr(census, ast, expr->data.if_expr.else_expr);
        break;
    case CM_AST_EXPR_MATCH:
        census_expr(census, ast, expr->data.match_expr.scrutinee);
        for (index = 0u; index < expr->data.match_expr.arm_count; ++index) {
            const CmAstMatchArm *arm = &expr->data.match_expr.arms[index];
            census_expr(census, ast, arm->guard);
            census_expr(census, ast, arm->guard_initializer);
            census_expr(census, ast, arm->body);
        }
        break;
    case CM_AST_EXPR_LOOP:
        census_expr(census, ast, expr->data.loop_expr.body);
        break;
    case CM_AST_EXPR_WHILE:
        census_expr(census, ast, expr->data.while_expr.condition);
        census_expr(census, ast, expr->data.while_expr.body);
        break;
    case CM_AST_EXPR_FOR:
        census_expr(census, ast, expr->data.for_expr.iterable);
        census_expr(census, ast, expr->data.for_expr.body);
        break;
    case CM_AST_EXPR_CLOSURE:
        census_expr(census, ast, expr->data.closure.body);
        break;
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY:
        for (index = 0u; index < expr->data.list.element_count; ++index)
            census_expr(census, ast, expr->data.list.elements[index]);
        census_expr(census, ast, expr->data.list.repeat_value);
        census_expr(census, ast, expr->data.list.repeat_length);
        break;
    case CM_AST_EXPR_STRUCT:
        for (index = 0u; index < expr->data.struct_expr.field_count; ++index)
            census_expr(census, ast,
                expr->data.struct_expr.fields[index].value);
        census_expr(census, ast, expr->data.struct_expr.base);
        break;
    case CM_AST_EXPR_MACRO:
        census_macro(census, ast, &expr->data.macro_expr);
        break;
    default:
        break;
    }
}

static int census_macro_compare(const void *left, const void *right)
{
    const CensusMacro *a = (const CensusMacro *)left;
    const CensusMacro *b = (const CensusMacro *)right;
    if (a->count != b->count) return a->count < b->count ? 1 : -1;
    return strcmp(a->name, b->name);
}

static void body_census(const CmHirContext *hir, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmHirModuleMap *modules)
{
    Census census;
    size_t index;
    unsigned long unresolved = 0u;
    memset(&census, 0, sizeof(census));
    for (index = 0u; index < hir->bodies.len; ++index) {
        const CmHirBody *body = (const CmHirBody *)cm_vec_at_const(
            &hir->bodies, index);
        const CmHirDefinition *definition;
        const CmHirItem *item;
        CmModuleId module = 0u;
        const CmAst *ast = NULL;
        if (body == NULL) continue;
        census.bodies += 1u;
        definition = cm_hir_lookup_definition(hir, body->origin.definition);
        item = definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM
            ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        if (item == NULL) {
            definition = cm_hir_lookup_definition(hir,
                body->origin.enclosing_definition);
            item = definition == NULL
                    || definition->kind != CM_HIR_DEFINITION_ITEM
                ? NULL : cm_hir_get_item(hir, definition->entity.item_id);
        }
        if (item == NULL
            || cm_hir_module_map_lookup_module(modules, graph, revision, hir,
                item->owner_module, &module) != CM_HIR_MODULE_MAP_OK
            || !cm_module_graph_borrow_ast(graph, module, &ast)
            || ast == NULL) {
            unresolved += 1u;
            continue;
        }
        census.bodies_with_ast += 1u;
        census.current_body_macros = 0u;
        census_expr(&census, ast, body->source_expression_id);
        if (census.current_body_macros != 0u)
            census.bodies_with_macros += 1u;
    }
    printf("body-census bodies=%lu with_ast=%lu unresolved=%lu "
        "with_macros=%lu macro_free=%lu let=%lu let_else=%lu item_stmts=%lu\n",
        census.bodies, census.bodies_with_ast, unresolved,
        census.bodies_with_macros,
        census.bodies_with_ast - census.bodies_with_macros,
        census.stmt_let, census.stmt_let_else, census.stmt_item);
    for (index = 0u; index < CENSUS_MAX_KINDS; ++index) {
        if (census.expr_kinds[index] == 0u) continue;
        if (index < sizeof(census_kind_names) / sizeof(census_kind_names[0]))
            printf("body-census expr %s=%lu\n", census_kind_names[index],
                census.expr_kinds[index]);
        else
            printf("body-census expr kind%lu=%lu\n", (unsigned long)index,
                census.expr_kinds[index]);
    }
    qsort(census.macros, census.macro_count, sizeof(census.macros[0]),
        census_macro_compare);
    for (index = 0u; index < census.macro_count; ++index)
        printf("body-census macro %s=%lu\n", census.macros[index].name,
            census.macros[index].count);
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
    int body_census_requested;
    int status;

    if (argc != 2 && (argc != 3
            || (strcmp(argv[2], "--require-metadata") != 0
                && strcmp(argv[2], "--body-census") != 0))) {
        fprintf(stderr, "usage: %s /path/to/library/core/src/lib.rs "
            "[--require-metadata]\n",
            argc == 0 ? "probe_core_hir" : argv[0]);
        return 2;
    }
    require_metadata = argc == 3
        && strcmp(argv[2], "--require-metadata") == 0;
    body_census_requested = argc == 3
        && strcmp(argv[2], "--body-census") == 0;
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
    if (body_census_requested) {
        /* M9-01: expand expression-position macros in place before HIR
         * lowering so the census below measures what remains. */
        CmBodyExpandOptions expand_options;
        CmBodyExpandResult expand_result;
        cm_body_expand_options_init(&expand_options);
        expand_options.edition = CM_EDITION_2024;
        expand_options.crate_identifier = "crate";
        expand_options.cfg = &cfg;
        expand_options.imports = &imports;
        expand_result = cm_body_expand_graph(&graph, graph_result.revision,
            &expand_options);
        printf("body-expand bodies=%lu invocations=%lu rules=%lu builtin=%lu "
            "asm=%lu unsupported_builtin=%lu failed=%lu\n",
            (unsigned long)expand_result.bodies,
            (unsigned long)expand_result.invocations,
            (unsigned long)expand_result.expanded_rules,
            (unsigned long)expand_result.expanded_builtin,
            (unsigned long)expand_result.remaining_asm,
            (unsigned long)expand_result.remaining_builtin,
            (unsigned long)expand_result.failed);
        if (expand_result.failed != 0u) {
            size_t class_index;
            printf("body-expand first_failure macro=%s reason=%s "
                "span=%lu..%lu\n", expand_result.first_failure_macro,
                expand_result.first_failure_reason,
                (unsigned long)expand_result.first_failure_span.start,
                (unsigned long)expand_result.first_failure_span.end);
            for (class_index = 0u;
                    class_index < expand_result.failure_class_count;
                    ++class_index)
                printf("body-expand failure macro=%s count=%lu reason=%s\n",
                    expand_result.failure_classes[class_index].macro_name,
                    (unsigned long)
                        expand_result.failure_classes[class_index].count,
                    expand_result.failure_classes[class_index].reason);
        }
    }
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "core";
    lower_options.source = root;
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.pointer_bits = target->pointer_bits;
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
        if (body_census_requested)
            body_census(&hir, &graph, graph_result.revision, &modules);
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
