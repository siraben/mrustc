#include "cm/macro/item_macro_plan.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CM_PLAN_MACROS_NONE 0
#define CM_PLAN_MACROS_ALL 1
#define CM_PLAN_MACROS_INVOCATIONS_ONLY 2

typedef struct CmPlanInputItem {
    CmAstItemId item_id;
    const CmExpandedItem *active;
    int is_generated;
    CmItemMacroItemRef source_invocation;
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    unsigned int expansion_depth;
} CmPlanInputItem;

typedef enum CmPlanBindingKind {
    CM_PLAN_BINDING_RULES = 0,
    CM_PLAN_BINDING_DECLARATIVE
} CmPlanBindingKind;

typedef struct CmPlanBinding {
    const CmAst *name_ast;
    CmInternId name;
    CmPlanBindingKind kind;
    CmItemMacroItemRef definition;
    const CmAst *definition_ast;
    const char *crate_identifier;
} CmPlanBinding;

typedef struct CmPlanCatalogEntry {
    CmInternId name;
    CmAstItemId definition;
} CmPlanCatalogEntry;

typedef struct CmItemMacroPlanContext {
    CmAst *ast;
    const CmItemMacroPlanOptions *options;
    CmItemMacroPlanResult result;
    CmVec catalog;
    CmVec declarations;
    CmVec mappings;
    CmVec pending;
    unsigned char *resolved_used;
    size_t catalog_items;
    size_t items_visited;
    size_t expansions;
    size_t generated_bytes;
} CmItemMacroPlanContext;

static void cm_plan_node_destroy(CmItemMacroPlanNode *node)
{
    size_t index;

    for (index = 0u; index < node->child_count; ++index) {
        cm_plan_node_destroy(&node->children[index]);
    }
    cm_free(node->children);
    cm_free(node->external_scope);
    cm_free(node->inner_attributes);
    cm_free(node->attributes);
    memset(node, 0, sizeof(*node));
}

static void cm_plan_declaration_destroy(CmItemMacroDeclaration *declaration)
{
    cm_free(declaration->attributes);
    memset(declaration, 0, sizeof(*declaration));
}

void cm_item_macro_plan_init(CmItemMacroPlan *plan)
{
    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
}

void cm_item_macro_plan_destroy(CmItemMacroPlan *plan)
{
    size_t index;

    if (plan == NULL) {
        return;
    }
    for (index = 0u; index < plan->root_count; ++index) {
        cm_plan_node_destroy(&plan->roots[index]);
    }
    for (index = 0u; index < plan->expansion_count; ++index) {
        cm_free(plan->expansions[index].generated_items);
    }
    for (index = 0u; index < plan->declaration_count; ++index) {
        cm_plan_declaration_destroy(&plan->declarations[index]);
    }
    cm_free(plan->roots);
    cm_free(plan->crate_attributes);
    cm_free(plan->root_items);
    cm_free(plan->declarations);
    cm_free(plan->expansions);
    cm_free(plan->pending_invocations);
    memset(plan, 0, sizeof(*plan));
}

void cm_item_macro_plan_options_init(CmItemMacroPlanOptions *options,
    const CmCfgSet *cfg)
{
    if (options == NULL) {
        return;
    }
    options->cfg = cfg;
    options->current_owner = (CmItemMacroAstOwner)1u;
    options->initial_scope = NULL;
    options->initial_scope_count = 0u;
    options->resolved_invocations = NULL;
    options->resolved_invocation_count = 0u;
    options->resolved_generated_paths = NULL;
    options->resolved_generated_path_count = 0u;
    options->resolve_generated_path = NULL;
    options->resolve_generated_path_context = NULL;
    options->defer_source_invocations = 0;
    cm_macro_reparse_options_init(&options->reparse);
    options->maximum_nesting = CM_ITEM_MACRO_PLAN_DEFAULT_MAX_NESTING;
    options->maximum_items = CM_ITEM_MACRO_PLAN_DEFAULT_MAX_ITEMS;
    options->maximum_expansions =
        CM_ITEM_MACRO_PLAN_DEFAULT_MAX_EXPANSIONS;
    options->maximum_generated_bytes =
        CM_ITEM_MACRO_PLAN_DEFAULT_MAX_GENERATED_BYTES;
}

static CmItemMacroPlanResult cm_plan_result_init(void)
{
    CmItemMacroPlanResult result;

    memset(&result, 0, sizeof(result));
    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.stage = CM_ITEM_MACRO_PLAN_STAGE_VALIDATE;
    result.kind = CM_ITEM_MACRO_PLAN_DIAG_INVALID_ARGUMENT;
    result.message = "invalid item macro planner argument";
    return result;
}

static void cm_plan_error(CmItemMacroPlanContext *context,
    CmMacroStatus status, CmItemMacroPlanStage stage,
    CmItemMacroPlanDiagnosticKind kind, CmAstItemId item_id,
    CmInternId macro_name, const char *message)
{
    if (context->result.status != CM_MACRO_OK) {
        return;
    }
    context->result.status = status;
    context->result.stage = stage;
    context->result.kind = kind;
    context->result.message = message;
    context->result.item_id = item_id;
    context->result.macro_name = macro_name;
}

static int cm_plan_string_is(const CmAst *ast, CmInternId id,
    const char *expected)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return string != NULL && string->len == length
        && memcmp(string->bytes, expected, length) == 0;
}

static CmItemMacroItemRef cm_plan_item_ref(CmItemMacroAstOwner owner,
    CmAstItemId item)
{
    CmItemMacroItemRef reference;

    reference.owner = owner;
    reference.item = item;
    return reference;
}

static void cm_plan_select_input_anchor(CmItemMacroPlanContext *context,
    const CmPlanInputItem *input)
{
    if (input->is_generated) {
        context->result.source_invocation = input->source_invocation;
    } else {
        context->result.source_invocation = cm_plan_item_ref(
            context->options->current_owner, input->item_id);
    }
}

static int cm_plan_names_equal(const CmAst *left_ast, CmInternId left,
    const CmAst *right_ast, CmInternId right)
{
    const CmInternedString *left_string;
    const CmInternedString *right_string;

    left_string = cm_ast_get_string(left_ast, left);
    right_string = cm_ast_get_string(right_ast, right);
    return left_string != NULL && right_string != NULL
        && left_string->len == right_string->len
        && memcmp(left_string->bytes, right_string->bytes,
            left_string->len) == 0;
}

static int cm_plan_path_name(const CmAst *ast, CmAstPathId path_id,
    CmInternId *name, int *qualified)
{
    const CmAstPath *path;
    const CmAstPathSegment *segment;

    path = cm_ast_get_path(ast, path_id);
    if (path == NULL || path->segment_count == 0u) {
        return 0;
    }
    segment = &path->segments[path->segment_count - 1u];
    *name = segment->name;
    *qualified = path->absolute || path->segment_count != 1u
        || segment->argument_count != 0u;
    return cm_ast_get_string(ast, *name) != NULL;
}

static int cm_plan_is_rules_definition(const CmAst *ast,
    const CmAstItem *item, CmInternId *name)
{
    CmInternId path_name;
    int qualified;

    if (item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form != CM_AST_MACRO_RULES_DEFINITION
        || item->name == CM_INTERN_ID_NONE
        || !cm_plan_path_name(ast, item->data.macro_item.path,
            &path_name, &qualified)) {
        return 0;
    }
    if (qualified || !cm_plan_string_is(ast, path_name, "macro_rules")) {
        return 0;
    }
    *name = item->name;
    return cm_ast_get_string(ast, *name) != NULL;
}

static int cm_plan_named_definition(const CmAst *ast,
    const CmAstItem *item, CmInternId *name, CmPlanBindingKind *kind)
{
    if (cm_plan_is_rules_definition(ast, item, name)) {
        *kind = CM_PLAN_BINDING_RULES;
        return 1;
    }
    if (item->kind != CM_AST_ITEM_MACRO
        || item->data.macro_item.form
            != CM_AST_MACRO_DECLARATIVE_DEFINITION
        || item->name == CM_INTERN_ID_NONE
        || item->data.macro_item.path != CM_AST_PATH_NONE
        || item->data.macro_item.delimiter != CM_AST_DELIMITER_BRACE
        || item->data.macro_item.has_semicolon
        || cm_ast_get_string(ast, item->name) == NULL
        || cm_ast_get_string(ast, item->data.macro_item.arguments) == NULL
        || (item->data.macro_item.parameters != CM_INTERN_ID_NONE
            && cm_ast_get_string(ast,
                item->data.macro_item.parameters) == NULL)) {
        return 0;
    }
    *name = item->name;
    *kind = item->data.macro_item.parameters == CM_INTERN_ID_NONE
        ? CM_PLAN_BINDING_RULES : CM_PLAN_BINDING_DECLARATIVE;
    return 1;
}

static int cm_plan_catalog_contains_definition(
    const CmItemMacroPlanContext *context, CmAstItemId definition)
{
    size_t index;
    const CmPlanCatalogEntry *entry;

    for (index = 0u; index < context->catalog.len; ++index) {
        entry = (const CmPlanCatalogEntry *)cm_vec_at_const(
            &context->catalog, index);
        if (entry != NULL && entry->definition == definition) {
            return 1;
        }
    }
    return 0;
}

static void cm_plan_catalog_add(CmItemMacroPlanContext *context,
    CmInternId name, CmAstItemId definition)
{
    CmPlanCatalogEntry entry;

    if (cm_plan_catalog_contains_definition(context, definition)) {
        return;
    }
    entry.name = name;
    entry.definition = definition;
    (void)cm_vec_push(&context->catalog, &entry);
}

static int cm_plan_catalog_active(CmItemMacroPlanContext *context,
    const CmExpandedItem *items, size_t count, unsigned int depth)
{
    size_t index;
    const CmAstItem *item;
    CmInternId name;
    CmPlanBindingKind kind;

    if (depth >= context->options->maximum_nesting) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_CATALOG,
            CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT, CM_AST_ITEM_NONE,
            CM_INTERN_ID_NONE, "active item tree nesting limit exceeded");
        return 0;
    }
    if (count != 0u && items == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_CATALOG,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
            "nonempty active item tree has no nodes");
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        if (context->catalog_items >= context->options->maximum_items) {
            cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
                CM_ITEM_MACRO_PLAN_STAGE_CATALOG,
                CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT,
                items[index].source_id, CM_INTERN_ID_NONE,
                "active item catalog exceeds planner item limit");
            return 0;
        }
        context->catalog_items += 1u;
        item = cm_ast_get_item(context->ast, items[index].source_id);
        if (item == NULL) {
            cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_ITEM_MACRO_PLAN_STAGE_CATALOG,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
                items[index].source_id, CM_INTERN_ID_NONE,
                "active item tree contains an invalid item ID");
            return 0;
        }
        if (cm_plan_named_definition(context->ast, item, &name, &kind)) {
            (void)kind;
            cm_plan_catalog_add(context, name, items[index].source_id);
        }
        if (items[index].child_count != 0u
            && !cm_plan_catalog_active(context, items[index].children,
                items[index].child_count, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static const CmPlanBinding *cm_plan_lookup_binding(const CmVec *scope,
    const CmAst *name_ast, CmInternId name)
{
    size_t index;
    const CmPlanBinding *binding;

    index = scope->len;
    while (index != 0u) {
        index -= 1u;
        binding = (const CmPlanBinding *)cm_vec_at_const(scope, index);
        if (binding != NULL && cm_plan_names_equal(binding->name_ast,
                binding->name, name_ast, name)) {
            return binding;
        }
    }
    return NULL;
}

static void cm_plan_bind(CmVec *scope, const CmAst *name_ast,
    CmInternId name, CmPlanBindingKind kind,
    CmItemMacroItemRef definition,
    const CmAst *definition_ast, const char *crate_identifier)
{
    CmPlanBinding binding;

    binding.name_ast = name_ast;
    binding.name = name;
    binding.kind = kind;
    binding.definition = definition;
    binding.definition_ast = definition_ast;
    binding.crate_identifier = crate_identifier;
    (void)cm_vec_push(scope, &binding);
}

static void cm_plan_clone_scope(CmVec *destination, const CmVec *source)
{
    cm_vec_init(destination, sizeof(CmPlanBinding));
    if (source->len != 0u) {
        cm_vec_append(destination, source->data, source->len);
    }
}

static int cm_plan_seed_scope(CmItemMacroPlanContext *context,
    CmVec *scope)
{
    size_t index;

    for (index = 0u; index < context->options->initial_scope_count;
            ++index) {
        const CmItemMacroScopeSeed *seed;
        const CmAstItem *item;
        CmInternId name;
        CmPlanBindingKind kind;

        seed = &context->options->initial_scope[index];
        item = seed->definition_ast == NULL ? NULL : cm_ast_get_item(
            seed->definition_ast, seed->definition.item);
        if (seed->definition.owner == CM_ITEM_MACRO_AST_OWNER_NONE
            || item == NULL || !cm_plan_named_definition(
                seed->definition_ast, item, &name, &kind)) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_ARGUMENT,
                CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
                "invalid inherited macro scope seed");
            return 0;
        }
        cm_plan_bind(scope, seed->definition_ast, name, kind,
            seed->definition, seed->definition_ast, "crate");
    }
    return 1;
}

static int cm_plan_item_refs_equal(CmItemMacroItemRef left,
    CmItemMacroItemRef right)
{
    return left.owner == right.owner && left.item == right.item;
}

static int cm_plan_validate_resolved_invocations(
    CmItemMacroPlanContext *context)
{
    size_t index;

    for (index = 0u;
            index < context->options->resolved_invocation_count; ++index) {
        const CmItemMacroResolvedInvocation *resolved;
        const CmAstItem *invocation_item;
        const CmAstItem *definition_item;
        CmInternId invocation_name;
        CmInternId definition_name;
        CmPlanBindingKind definition_kind;
        int qualified;
        size_t earlier;

        resolved = &context->options->resolved_invocations[index];
        invocation_name = CM_INTERN_ID_NONE;
        definition_name = CM_INTERN_ID_NONE;
        definition_kind = CM_PLAN_BINDING_RULES;
        qualified = 0;
        invocation_item = cm_ast_get_item(context->ast,
            resolved->invocation.item);
        definition_item = resolved->definition_ast == NULL ? NULL
            : cm_ast_get_item(resolved->definition_ast,
                resolved->definition.item);
        if (resolved->invocation.owner != context->options->current_owner
            || invocation_item == NULL
            || invocation_item->kind != CM_AST_ITEM_MACRO
            || invocation_item->name != CM_INTERN_ID_NONE
            || invocation_item->data.macro_item.form
                != CM_AST_MACRO_INVOCATION
            || resolved->definition.owner == CM_ITEM_MACRO_AST_OWNER_NONE
            || (resolved->definition.owner
                    == context->options->current_owner
                && resolved->definition_ast != context->ast)
            || (resolved->definition.owner
                    != context->options->current_owner
                && resolved->definition_ast == context->ast)
            || definition_item == NULL
            || !cm_plan_path_name(context->ast,
                invocation_item->data.macro_item.path, &invocation_name,
                &qualified)
            || !cm_plan_named_definition(resolved->definition_ast,
                definition_item, &definition_name, &definition_kind)) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                resolved->invocation.item, invocation_item == NULL
                    ? CM_INTERN_ID_NONE : invocation_name,
                "invalid resolver-certified macro invocation binding");
            return 0;
        }
        if (resolved->builtin
                != CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE
            && (resolved->builtin
                    != CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT
                || definition_kind != CM_PLAN_BINDING_DECLARATIVE
                || !cm_plan_string_is(resolved->definition_ast,
                    definition_name, "cfg_select"))) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                resolved->invocation.item, invocation_name,
                "invalid resolver-certified builtin macro binding");
            return 0;
        }
        for (earlier = 0u; earlier < index; ++earlier) {
            if (cm_plan_item_refs_equal(resolved->invocation,
                    context->options->resolved_invocations[earlier]
                        .invocation)) {
                cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                    CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                    CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                    resolved->invocation.item, invocation_name,
                    "duplicate resolver-certified macro invocation binding");
                return 0;
            }
        }
    }
    return 1;
}

static int cm_plan_resolved_segment_valid(
    const CmItemMacroPathSegment *segment)
{
    size_t index;

    if (segment == NULL || segment->bytes == NULL || segment->length == 0u)
        return 0;
    if (!((segment->bytes[0] >= (unsigned char)'a'
                && segment->bytes[0] <= (unsigned char)'z')
            || (segment->bytes[0] >= (unsigned char)'A'
                && segment->bytes[0] <= (unsigned char)'Z')
            || segment->bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; index < segment->length; ++index) {
        if (!((segment->bytes[index] >= (unsigned char)'a'
                    && segment->bytes[index] <= (unsigned char)'z')
                || (segment->bytes[index] >= (unsigned char)'A'
                    && segment->bytes[index] <= (unsigned char)'Z')
                || (segment->bytes[index] >= (unsigned char)'0'
                    && segment->bytes[index] <= (unsigned char)'9')
                || segment->bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_plan_resolved_crate_identifier_valid(const char *identifier)
{
    CmItemMacroPathSegment segment;

    if (identifier == NULL) return 1;
    segment.bytes = (const unsigned char *)identifier;
    segment.length = strlen(identifier);
    return cm_plan_resolved_segment_valid(&segment);
}

static int cm_plan_resolved_paths_equal(
    const CmItemMacroResolvedGeneratedPath *left,
    const CmItemMacroResolvedGeneratedPath *right)
{
    size_t index;

    if (left->segment_count != right->segment_count) return 0;
    for (index = 0u; index < left->segment_count; ++index) {
        if (left->segments[index].length != right->segments[index].length
            || memcmp(left->segments[index].bytes,
                right->segments[index].bytes,
                left->segments[index].length) != 0) return 0;
    }
    return 1;
}

static int cm_plan_validate_resolved_generated_paths(
    CmItemMacroPlanContext *context)
{
    size_t index;

    for (index = 0u;
            index < context->options->resolved_generated_path_count;
            ++index) {
        const CmItemMacroResolvedGeneratedPath *resolved;
        const CmAstItem *definition_item;
        CmInternId definition_name;
        CmPlanBindingKind definition_kind;
        size_t segment_index;
        size_t earlier;

        resolved = &context->options->resolved_generated_paths[index];
        definition_item = resolved->definition_ast == NULL ? NULL
            : cm_ast_get_item(resolved->definition_ast,
                resolved->definition.item);
        definition_name = CM_INTERN_ID_NONE;
        definition_kind = CM_PLAN_BINDING_RULES;
        if (resolved->segments == NULL || resolved->segment_count < 2u
            || resolved->definition.owner == CM_ITEM_MACRO_AST_OWNER_NONE
            || !cm_plan_resolved_crate_identifier_valid(
                resolved->crate_identifier)
            || (resolved->definition.owner
                    == context->options->current_owner
                && resolved->definition_ast != context->ast)
            || (resolved->definition.owner
                    != context->options->current_owner
                && resolved->definition_ast == context->ast)
            || definition_item == NULL
            || !cm_plan_named_definition(resolved->definition_ast,
                definition_item, &definition_name, &definition_kind)) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
                "invalid resolver-certified generated macro path");
            return 0;
        }
        for (segment_index = 0u; segment_index < resolved->segment_count;
                ++segment_index) {
            if (!cm_plan_resolved_segment_valid(
                    &resolved->segments[segment_index])) {
                cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                    CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                    CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                    CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
                    "invalid resolver-certified generated path segment");
                return 0;
            }
        }
        for (earlier = 0u; earlier < index; ++earlier) {
            if (cm_plan_resolved_paths_equal(resolved,
                    &context->options->resolved_generated_paths[earlier])) {
                cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                    CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                    CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                    CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
                    "duplicate resolver-certified generated macro path");
                return 0;
            }
        }
    }
    return 1;
}

static int cm_plan_ast_path_matches_resolved(const CmAst *ast,
    CmAstPathId path_id, const CmItemMacroResolvedGeneratedPath *resolved)
{
    const CmAstPath *path;
    size_t index;

    path = cm_ast_get_path(ast, path_id);
    if (path == NULL || path->absolute
        || path->segment_count != resolved->segment_count) return 0;
    for (index = 0u; index < resolved->segment_count; ++index) {
        const CmInternedString *name;

        if (path->segments[index].argument_count != 0u) return 0;
        name = cm_ast_get_string(ast, path->segments[index].name);
        if (name == NULL || name->len != resolved->segments[index].length
            || memcmp(name->bytes, resolved->segments[index].bytes,
                name->len) != 0) return 0;
    }
    return 1;
}

static const CmItemMacroResolvedGeneratedPath *
cm_plan_resolved_generated_path(CmItemMacroPlanContext *context,
    CmAstPathId path_id)
{
    size_t index;

    for (index = 0u;
            index < context->options->resolved_generated_path_count;
            ++index) {
        const CmItemMacroResolvedGeneratedPath *resolved;

        resolved = &context->options->resolved_generated_paths[index];
        if (cm_plan_ast_path_matches_resolved(context->ast, path_id,
                resolved)) return resolved;
    }
    return NULL;
}

static CmItemMacroGeneratedLookupStatus
cm_plan_callback_generated_path(CmItemMacroPlanContext *context,
    CmAstPathId path_id, CmItemMacroResolvedGeneratedTarget *out_target)
{
    const CmAstPath *path;
    CmItemMacroPathSegment *segments;
    CmItemMacroGeneratedLookupStatus status;
    size_t index;

    memset(out_target, 0, sizeof(*out_target));
    if (context->options->resolve_generated_path == NULL)
        return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
    path = cm_ast_get_path(context->ast, path_id);
    if (path == NULL || path->absolute || path->segment_count < 2u)
        return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
    segments = (CmItemMacroPathSegment *)cm_alloc_zeroed(
        path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name;

        if (path->segments[index].argument_count != 0u) {
            cm_free(segments);
            return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
        }
        name = cm_ast_get_string(context->ast,
            path->segments[index].name);
        if (name == NULL) {
            cm_free(segments);
            return CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    status = context->options->resolve_generated_path(
        context->options->resolve_generated_path_context, segments,
        path->segment_count, out_target);
    cm_free(segments);
    return status;
}

static int cm_plan_generated_target_valid(CmItemMacroPlanContext *context,
    const CmItemMacroResolvedGeneratedTarget *target)
{
    const CmAstItem *definition_item;
    CmInternId definition_name;
    CmPlanBindingKind definition_kind;

    definition_item = target->definition_ast == NULL ? NULL
        : cm_ast_get_item(target->definition_ast, target->definition.item);
    definition_name = CM_INTERN_ID_NONE;
    definition_kind = CM_PLAN_BINDING_RULES;
    return target->definition.owner != CM_ITEM_MACRO_AST_OWNER_NONE
        && (target->definition.owner != context->options->current_owner
            || target->definition_ast == context->ast)
        && (target->definition.owner == context->options->current_owner
            || target->definition_ast != context->ast)
        && cm_plan_resolved_crate_identifier_valid(target->crate_identifier)
        && definition_item != NULL
        && cm_plan_named_definition(target->definition_ast, definition_item,
            &definition_name, &definition_kind);
}

static const CmItemMacroResolvedInvocation *cm_plan_resolved_invocation(
    CmItemMacroPlanContext *context, CmItemMacroItemRef invocation,
    size_t *out_index)
{
    size_t index;

    for (index = 0u;
            index < context->options->resolved_invocation_count; ++index) {
        const CmItemMacroResolvedInvocation *resolved;

        resolved = &context->options->resolved_invocations[index];
        if (cm_plan_item_refs_equal(resolved->invocation, invocation)) {
            *out_index = index;
            return resolved;
        }
    }
    return NULL;
}

static int cm_plan_all_resolved_invocations_used(
    CmItemMacroPlanContext *context)
{
    size_t index;

    for (index = 0u;
            index < context->options->resolved_invocation_count; ++index) {
        if (context->resolved_used[index] == 0u) {
            const CmItemMacroResolvedInvocation *resolved;

            resolved = &context->options->resolved_invocations[index];
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                resolved->invocation.item, CM_INTERN_ID_NONE,
                "resolver-certified macro invocation was not active");
            return 0;
        }
    }
    return 1;
}

static void cm_plan_copy_external_scope(const CmVec *scope,
    CmItemMacroPlanNode *node)
{
    size_t index;

    node->external_scope_count = scope->len;
    if (scope->len == 0u) {
        return;
    }
    node->external_scope = (CmItemMacroItemRef *)cm_alloc(
        scope->len * sizeof(CmItemMacroItemRef));
    for (index = 0u; index < scope->len; ++index) {
        const CmPlanBinding *binding;

        binding = (const CmPlanBinding *)cm_vec_at_const(scope, index);
        node->external_scope[index] = binding->definition;
    }
}

static size_t cm_plan_catalog_name_count(
    const CmItemMacroPlanContext *context, CmInternId name)
{
    size_t index;
    size_t count;
    const CmPlanCatalogEntry *entry;

    count = 0u;
    for (index = 0u; index < context->catalog.len; ++index) {
        entry = (const CmPlanCatalogEntry *)cm_vec_at_const(
            &context->catalog, index);
        if (entry != NULL && entry->name == name) {
            count += 1u;
        }
    }
    return count;
}

static int cm_plan_later_definition(CmItemMacroPlanContext *context,
    const CmPlanInputItem *items, size_t start, size_t count,
    CmInternId name)
{
    size_t index;
    const CmAstItem *item;
    CmInternId definition_name;

    for (index = start; index < count; ++index) {
        item = cm_ast_get_item(context->ast, items[index].item_id);
        if (item != NULL && cm_plan_is_rules_definition(context->ast,
            item, &definition_name) && definition_name == name) {
            return 1;
        }
    }
    return 0;
}

static void cm_plan_destroy_node_vector(CmVec *nodes)
{
    size_t index;
    CmItemMacroPlanNode *node;

    for (index = 0u; index < nodes->len; ++index) {
        node = (CmItemMacroPlanNode *)cm_vec_at(nodes, index);
        if (node != NULL) {
            cm_plan_node_destroy(node);
        }
    }
    cm_vec_destroy(nodes);
}

static CmItemMacroPlanNode *cm_plan_copy_nodes(CmVec *nodes)
{
    CmItemMacroPlanNode *copy;

    if (nodes->len == 0u) {
        return NULL;
    }
    copy = (CmItemMacroPlanNode *)cm_alloc(
        nodes->len * sizeof(CmItemMacroPlanNode));
    memcpy(copy, nodes->data, nodes->len * sizeof(CmItemMacroPlanNode));
    return copy;
}

static void cm_plan_destroy_mappings(CmVec *mappings)
{
    size_t index;
    CmItemMacroExpansion *mapping;

    for (index = 0u; index < mappings->len; ++index) {
        mapping = (CmItemMacroExpansion *)cm_vec_at(mappings, index);
        if (mapping != NULL) {
            cm_free(mapping->generated_items);
            mapping->generated_items = NULL;
            mapping->generated_item_count = 0u;
        }
    }
    cm_vec_destroy(mappings);
}

static void cm_plan_destroy_declarations(CmVec *declarations)
{
    size_t index;
    CmItemMacroDeclaration *declaration;

    for (index = 0u; index < declarations->len; ++index) {
        declaration = (CmItemMacroDeclaration *)cm_vec_at(
            declarations, index);
        if (declaration != NULL) {
            cm_plan_declaration_destroy(declaration);
        }
    }
    cm_vec_destroy(declarations);
}

static int cm_plan_record_expansion(CmItemMacroPlanContext *context,
    CmItemMacroItemRef invocation, CmItemMacroItemRef definition,
    const CmAstItemId *generated_items, size_t generated_item_count)
{
    CmItemMacroExpansion mapping;

    memset(&mapping, 0, sizeof(mapping));
    mapping.invocation = invocation;
    mapping.definition = definition;
    mapping.generated_item_count = generated_item_count;
    if (generated_item_count != 0u) {
        if (generated_items == NULL) {
            cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_ITEM_MACRO_PLAN_STAGE_REPARSE,
                CM_ITEM_MACRO_PLAN_DIAG_REPARSE, invocation.item,
                CM_INTERN_ID_NONE,
                "successful macro reparse returned no generated item IDs");
            return 0;
        }
        mapping.generated_items = (CmAstItemId *)cm_alloc_zeroed(
            generated_item_count, sizeof(CmAstItemId));
        memcpy(mapping.generated_items, generated_items,
            generated_item_count * sizeof(CmAstItemId));
    }
    (void)cm_vec_push(&context->mappings, &mapping);
    return 1;
}

static int cm_plan_defer_invocation(CmItemMacroPlanContext *context,
    const CmPlanInputItem *input, const CmAstItem *item, int qualified,
    CmAstItemId container_item)
{
    const CmAstItem *source_item;
    CmItemMacroPendingInvocation pending;

    if (!context->options->defer_source_invocations) return 0;
    memset(&pending, 0, sizeof(pending));
    pending.invocation = cm_plan_item_ref(
        context->options->current_owner, input->item_id);
    pending.source_invocation = input->is_generated
        ? input->source_invocation : pending.invocation;
    pending.container_item = container_item;
    pending.is_generated = input->is_generated;
    source_item = input->is_generated
        && input->source_invocation.owner
            == context->options->current_owner
        ? cm_ast_get_item(context->ast, input->source_invocation.item)
        : NULL;
    if (input->is_generated && source_item == NULL) return 0;
    pending.span = source_item == NULL ? item->span : source_item->span;
    pending.is_qualified = qualified;
    (void)cm_vec_push(&context->pending, &pending);
    return 1;
}

static int cm_plan_process_sequence(CmItemMacroPlanContext *context,
    const CmPlanInputItem *inputs, size_t input_count, CmVec *scope,
    unsigned int nesting, unsigned int expansion_depth, int allow_macros,
    CmAstItemId container_item, CmVec *output);

static CmPlanInputItem *cm_plan_inputs_from_active(
    const CmExpandedItem *items, size_t count, int is_generated,
    CmItemMacroItemRef source_invocation,
    CmItemMacroItemRef invocation, CmItemMacroItemRef definition,
    unsigned int expansion_depth)
{
    CmPlanInputItem *inputs;
    size_t index;

    if (count == 0u) {
        return NULL;
    }
    inputs = (CmPlanInputItem *)cm_alloc_zeroed(count,
        sizeof(CmPlanInputItem));
    for (index = 0u; index < count; ++index) {
        inputs[index].item_id = items[index].source_id;
        inputs[index].active = &items[index];
        inputs[index].is_generated = is_generated;
        inputs[index].source_invocation = source_invocation;
        inputs[index].invocation = invocation;
        inputs[index].definition = definition;
        inputs[index].expansion_depth = expansion_depth;
    }
    return inputs;
}

static int cm_plan_attribute_id_in(const CmAstAttributeId *ids,
    size_t count, CmAstAttributeId id)
{
    size_t index;

    if (count != 0u && ids == NULL) return 0;
    for (index = 0u; index < count; ++index) {
        if (ids[index] == id) return 1;
    }
    return 0;
}

static int cm_plan_copy_active_attributes(CmItemMacroPlanContext *context,
    const CmPlanInputItem *input, CmItemMacroPlanNode *node)
{
    const CmAstItem *item;
    size_t count;
    size_t index;

    if (input->active == NULL) {
        return 1;
    }
    count = input->active->attribute_count;
    if (count != 0u && input->active->attributes == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            input->item_id, CM_INTERN_ID_NONE,
            "active item has a nonempty attribute count but no attributes");
        return 0;
    }
    if (count != 0u) {
        node->attributes = (CmEffectiveAttribute *)cm_alloc(
            count * sizeof(CmEffectiveAttribute));
        memcpy(node->attributes, input->active->attributes,
            count * sizeof(CmEffectiveAttribute));
    }
    node->attribute_count = count;
    count = input->active->inner_attribute_count;
    if (count != 0u && input->active->inner_attributes == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            input->item_id, CM_INTERN_ID_NONE,
            "active module has inner-attribute count but no attributes");
        return 0;
    }
    if (count != 0u) {
        item = cm_ast_get_item(context->ast, input->item_id);
        if (item == NULL || item->kind != CM_AST_ITEM_MODULE
            || !item->data.module_item.is_inline) {
            cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
                input->item_id, CM_INTERN_ID_NONE,
                "only an inline module can own effective inner attributes");
            return 0;
        }
        for (index = 0u; index < count; ++index) {
            const CmEffectiveAttribute *effective;
            const CmAstAttribute *source_attribute;

            effective = &input->active->inner_attributes[index];
            source_attribute = cm_ast_get_attribute(context->ast,
                effective->source_id);
            if (effective->style != CM_AST_ATTR_INNER
                || source_attribute == NULL
                || source_attribute->style != CM_AST_ATTR_INNER
                || !cm_plan_attribute_id_in(
                    item->data.module_item.inner_attributes,
                    (size_t)item->data.module_item.inner_attribute_count,
                    effective->source_id)
                || effective->span.start > effective->span.end
                || (effective->meta_length != 0u
                    && effective->meta == NULL)) {
                cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
                    CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                    CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
                    input->item_id, CM_INTERN_ID_NONE,
                    "effective inner attribute has invalid source provenance");
                return 0;
            }
        }
        node->inner_attributes = (CmEffectiveAttribute *)cm_alloc(
            count * sizeof(CmEffectiveAttribute));
        memcpy(node->inner_attributes, input->active->inner_attributes,
            count * sizeof(CmEffectiveAttribute));
    }
    node->inner_attribute_count = count;
    return 1;
}

static int cm_plan_record_declaration(CmItemMacroPlanContext *context,
    const CmPlanInputItem *input, const CmAstItem *item,
    CmAstItemId container_item)
{
    CmItemMacroPlanNode copied;
    CmItemMacroDeclaration declaration;

    memset(&copied, 0, sizeof(copied));
    if (!cm_plan_copy_active_attributes(context, input, &copied)) {
        cm_plan_node_destroy(&copied);
        return 0;
    }
    if (copied.inner_attribute_count != 0u) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            input->item_id, item->name,
            "macro declaration cannot own inner attributes");
        cm_plan_node_destroy(&copied);
        return 0;
    }
    memset(&declaration, 0, sizeof(declaration));
    declaration.item_id = input->item_id;
    declaration.container_item = container_item;
    declaration.span = item->span;
    declaration.form = item->data.macro_item.form;
    declaration.is_generated = input->is_generated;
    declaration.attributes = copied.attributes;
    declaration.attribute_count = copied.attribute_count;
    declaration.source_invocation = input->source_invocation;
    declaration.invocation = input->invocation;
    declaration.definition = input->definition;
    declaration.expansion_depth = input->expansion_depth;
    copied.attributes = NULL;
    copied.attribute_count = 0u;
    cm_plan_node_destroy(&copied);
    (void)cm_vec_push(&context->declarations, &declaration);
    return 1;
}

static int cm_plan_process_children(CmItemMacroPlanContext *context,
    const CmPlanInputItem *input, const CmAstItem *item, CmVec *scope,
    unsigned int nesting, unsigned int expansion_depth,
    CmAstItemId container_item,
    CmItemMacroPlanNode *node)
{
    size_t child_count;
    CmPlanInputItem *child_inputs;
    CmVec child_scope;
    CmVec children;
    int allow_child_macros;

    child_inputs = NULL;
    child_count = 0u;
    if (input->active == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            input->item_id, CM_INTERN_ID_NONE,
            "planner item has no cfg-expanded view");
        return 0;
    }
    node->child_kind = input->active->child_kind;
    child_count = input->active->child_count;
    if (child_count != 0u && input->active->children == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            input->item_id, CM_INTERN_ID_NONE,
            "cfg-expanded item has child count but no child view");
        return 0;
    }
    child_inputs = cm_plan_inputs_from_active(input->active->children,
        child_count, input->is_generated, input->source_invocation,
        input->invocation, input->definition,
        input->expansion_depth);
    if (child_count == 0u) {
        return 1;
    }
    cm_plan_clone_scope(&child_scope, scope);
    cm_vec_init(&children, sizeof(CmItemMacroPlanNode));
    allow_child_macros = item->kind == CM_AST_ITEM_MODULE
        ? CM_PLAN_MACROS_ALL
        : (item->kind == CM_AST_ITEM_IMPL
            ? CM_PLAN_MACROS_INVOCATIONS_ONLY : CM_PLAN_MACROS_NONE);
    if (!cm_plan_process_sequence(context, child_inputs, child_count,
        &child_scope, nesting + 1u, expansion_depth, allow_child_macros,
        item->kind == CM_AST_ITEM_MODULE
            ? input->item_id : container_item,
        &children)) {
        cm_plan_destroy_node_vector(&children);
        cm_vec_destroy(&child_scope);
        cm_free(child_inputs);
        return 0;
    }
    node->children = cm_plan_copy_nodes(&children);
    node->child_count = children.len;
    cm_vec_destroy(&children);
    cm_vec_destroy(&child_scope);
    cm_free(child_inputs);
    return 1;
}

static int cm_plan_resolve_missing(CmItemMacroPlanContext *context,
    const CmPlanInputItem *inputs, size_t index, size_t count,
    CmAstItemId invocation, CmInternId name)
{
    size_t catalog_count;

    if (cm_plan_later_definition(context, inputs, index + 1u, count,
        name)) {
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_FORWARD_MACRO, invocation, name,
            "macro invocation precedes its local macro_rules definition");
        return 0;
    }
    catalog_count = cm_plan_catalog_name_count(context, name);
    if (catalog_count > 1u) {
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_AMBIGUOUS_MACRO, invocation, name,
            "multiple inaccessible macro_rules definitions share this name");
    } else if (catalog_count == 1u) {
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_OUT_OF_SCOPE_MACRO, invocation, name,
            "macro_rules definition is outside the lexical macro scope");
    } else {
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO, invocation, name,
            "macro has no supported local macro_rules definition");
    }
    return 0;
}

static int cm_plan_expand_invocation(CmItemMacroPlanContext *context,
    const CmPlanInputItem *inputs, size_t index, size_t input_count,
    const CmAstItem *item, CmVec *scope, unsigned int nesting,
    unsigned int expansion_depth, int allow_macros,
    CmAstItemId container_item, CmVec *output)
{
    CmInternId name;
    int qualified;
    const CmPlanBinding *binding;
    CmPlanBinding resolved_binding;
    const CmItemMacroResolvedInvocation *resolved;
    const CmItemMacroResolvedGeneratedPath *resolved_generated;
    CmItemMacroResolvedGeneratedTarget callback_target;
    CmItemMacroGeneratedLookupStatus callback_status;
    const CmAstItem *resolved_definition_item;
    const CmAstItem *definition_item_node;
    CmInternId resolved_name;
    CmPlanBindingKind resolved_kind;
    size_t resolved_index;
    CmItemMacroItemRef invocation;
    CmItemMacroItemRef definition;
    CmMacroReparseResult reparse;
    CmMacroReparseOptions reparse_options;
    CmExpandOptions expand_options;
    CmExpandedItemSequence active_generated;
    CmExpandResult expand;
    CmPlanInputItem *generated;
    CmVec generated_scope;
    CmVec *expansion_scope;
    size_t inherited_scope_count;
    size_t generated_count;
    size_t remaining_items;
    CmItemMacroItemRef source_invocation;
    CmItemMacroResolvedBuiltin resolved_builtin;
    int local_parameterized;
    int resolver_certified;

    invocation = cm_plan_item_ref(context->options->current_owner,
        inputs[index].item_id);
    source_invocation = inputs[index].is_generated
        ? inputs[index].source_invocation : invocation;
    context->result.source_invocation = source_invocation;
    if (item->data.macro_item.form != CM_AST_MACRO_INVOCATION) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            inputs[index].item_id, CM_INTERN_ID_NONE,
            "macro invocation has an invalid AST form");
        return 0;
    }
    if (!cm_plan_path_name(context->ast, item->data.macro_item.path,
        &name, &qualified)) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            inputs[index].item_id, CM_INTERN_ID_NONE,
            "macro invocation has an invalid path");
        return 0;
    }
    binding = cm_plan_lookup_binding(scope, context->ast, name);
    resolved_index = 0u;
    resolved_builtin = CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE;
    resolved = cm_plan_resolved_invocation(context, invocation,
        &resolved_index);
    resolved_generated = resolved == NULL && qualified
            && inputs[index].is_generated
        ? cm_plan_resolved_generated_path(context,
            item->data.macro_item.path)
        : NULL;
    memset(&callback_target, 0, sizeof(callback_target));
    callback_status = CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
    if (resolved == NULL && resolved_generated == NULL && qualified
        && inputs[index].is_generated) {
        callback_status = cm_plan_callback_generated_path(context,
            item->data.macro_item.path, &callback_target);
        if (callback_status > CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID) {
            callback_status = CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID;
        }
        if (callback_status == CM_ITEM_MACRO_GENERATED_LOOKUP_AMBIGUOUS) {
            cm_plan_error(context, CM_MACRO_UNSUPPORTED,
                CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
                CM_ITEM_MACRO_PLAN_DIAG_AMBIGUOUS_MACRO,
                inputs[index].item_id, name,
                "generated qualified macro path is ambiguous");
            return 0;
        }
        if (callback_status == CM_ITEM_MACRO_GENERATED_LOOKUP_INVALID
            || (callback_status == CM_ITEM_MACRO_GENERATED_LOOKUP_OK
                && !cm_plan_generated_target_valid(context,
                    &callback_target))) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                inputs[index].item_id, name,
                "generated macro resolver returned an invalid target");
            return 0;
        }
    }
    if (resolved == NULL && context->options->defer_source_invocations
        && !inputs[index].is_generated) {
        if (!qualified && binding == NULL
            && cm_plan_later_definition(context, inputs, index + 1u,
                input_count, name)) {
            return cm_plan_resolve_missing(context, inputs, index,
                input_count, inputs[index].item_id, name);
        }
        if (cm_plan_defer_invocation(context, &inputs[index], item,
                qualified, container_item)) return 1;
    }
    if (allow_macros == CM_PLAN_MACROS_NONE) {
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO,
            inputs[index].item_id, CM_INTERN_ID_NONE,
            "item macros in this non-module container are unsupported");
        return 0;
    }
    resolver_certified = resolved != NULL || resolved_generated != NULL
        || callback_status == CM_ITEM_MACRO_GENERATED_LOOKUP_OK;
    if (resolver_certified) {
        const CmAst *resolved_ast;
        CmItemMacroItemRef resolved_definition;
        const char *resolved_crate_identifier;

        resolved_ast = resolved != NULL ? resolved->definition_ast
            : (resolved_generated != NULL
                ? resolved_generated->definition_ast
                : callback_target.definition_ast);
        resolved_definition = resolved != NULL ? resolved->definition
            : (resolved_generated != NULL
                ? resolved_generated->definition
                : callback_target.definition);
        resolved_crate_identifier = resolved != NULL
            ? resolved->crate_identifier
            : (resolved_generated != NULL
                ? resolved_generated->crate_identifier
                : callback_target.crate_identifier);
        resolved_definition_item = cm_ast_get_item(resolved_ast,
            resolved_definition.item);
        if (resolved_definition_item == NULL
            || !cm_plan_named_definition(resolved_ast,
                resolved_definition_item, &resolved_name, &resolved_kind)) {
            cm_plan_error(context, CM_MACRO_INVALID_ARGUMENT,
                CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING,
                inputs[index].item_id, name,
                "resolver-certified macro definition became invalid");
            return 0;
        }
        memset(&resolved_binding, 0, sizeof(resolved_binding));
        resolved_binding.name_ast = resolved_ast;
        resolved_binding.name = resolved_name;
        resolved_binding.kind = resolved_kind;
        resolved_binding.definition = resolved_definition;
        resolved_binding.definition_ast = resolved_ast;
        resolved_binding.crate_identifier = resolved_crate_identifier == NULL
            ? "crate" : resolved_crate_identifier;
        binding = &resolved_binding;
        if (resolved != NULL) {
            resolved_builtin = resolved->builtin;
            context->resolved_used[resolved_index] = 1u;
        }
    } else if (qualified) {
        if (cm_plan_defer_invocation(context, &inputs[index], item,
                qualified, container_item)) return 1;
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_QUALIFIED_MACRO,
            inputs[index].item_id, name,
            "qualified item macro paths are outside local resolution");
        return 0;
    }
    if (binding == NULL) {
        if (!cm_plan_later_definition(context, inputs, index + 1u,
                input_count, name)
            && cm_plan_defer_invocation(context, &inputs[index], item,
                qualified, container_item)) return 1;
        return cm_plan_resolve_missing(context, inputs, index,
            input_count, inputs[index].item_id, name);
    }
    definition_item_node = cm_ast_get_item(binding->definition_ast,
        binding->definition.item);
    local_parameterized = binding->kind == CM_PLAN_BINDING_DECLARATIVE
        && resolved_builtin == CM_ITEM_MACRO_RESOLVED_BUILTIN_NONE
        && binding->definition.owner == context->options->current_owner
        && binding->definition_ast == context->ast
        && definition_item_node != NULL
        && definition_item_node->data.macro_item.parameters
            != CM_INTERN_ID_NONE
        && definition_item_node->visibility.kind == CM_AST_VIS_INHERITED
        && definition_item_node->attribute_count == 0u
        && definition_item_node->span.end <= item->span.start
        && !qualified && !inputs[index].is_generated
        && allow_macros == CM_PLAN_MACROS_ALL
        && item->data.macro_item.delimiter == CM_AST_DELIMITER_PAREN;
    if (binding->kind != CM_PLAN_BINDING_RULES && !local_parameterized
        && resolved_builtin != CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT) {
        context->result.definition = binding->definition;
        cm_plan_error(context, CM_MACRO_UNSUPPORTED,
            CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
            CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO,
            inputs[index].item_id, name,
            "declarative macro expansion is unsupported");
        return 0;
    }
    definition = binding->definition;
    if (expansion_depth >= context->options->maximum_nesting) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
            CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT,
            inputs[index].item_id, name,
            "recursive item macro expansion depth exceeded");
        return 0;
    }
    if (context->expansions >= context->options->maximum_expansions) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
            CM_ITEM_MACRO_PLAN_DIAG_EXPANSION_LIMIT,
            inputs[index].item_id, name,
            "item macro expansion count exceeded");
        return 0;
    }
    context->expansions += 1u;
    reparse_options = context->options->reparse;
    reparse_options.item_context = allow_macros
            == CM_PLAN_MACROS_INVOCATIONS_ONLY
        ? CM_ITEM_LIST_FRAGMENT_IMPL : CM_ITEM_LIST_FRAGMENT_ROOT;
    reparse_options.expansion.crate_identifier = binding->crate_identifier;
    if (resolved_builtin == CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT) {
        reparse = cm_cfg_select_reparse_items(context->ast,
            inputs[index].item_id, &context->options->cfg->environment,
            context->ast, &reparse_options);
    } else {
        reparse = cm_macro_rules_reparse_items(binding->definition_ast,
            definition.item, context->ast, inputs[index].item_id,
            context->ast, &reparse_options);
    }
    context->result.reparse = reparse;
    context->result.definition = definition;
    if (reparse.status != CM_MACRO_OK) {
        cm_plan_error(context, reparse.status,
            CM_ITEM_MACRO_PLAN_STAGE_REPARSE,
            CM_ITEM_MACRO_PLAN_DIAG_REPARSE,
            inputs[index].item_id, name,
            reparse.message);
        return 0;
    }
    if (context->generated_bytes
        > context->options->maximum_generated_bytes
        || reparse.generated_length
            > context->options->maximum_generated_bytes
                - context->generated_bytes) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
            CM_ITEM_MACRO_PLAN_DIAG_OUTPUT_LIMIT,
            inputs[index].item_id, name,
            "total generated macro source exceeds planner limit");
        return 0;
    }
    context->generated_bytes += reparse.generated_length;
    generated_count = (size_t)reparse.item_count;
    if (context->items_visited > context->options->maximum_items
        || generated_count > context->options->maximum_items
            - context->items_visited) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
            CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT,
            inputs[index].item_id, name,
            "generated item list exceeds remaining planner item limit");
        return 0;
    }
    if (!cm_plan_record_expansion(context, invocation,
        definition, reparse.items, generated_count)) {
        return 0;
    }
    remaining_items = context->options->maximum_items
        - context->items_visited;
    cm_expand_options_init(&expand_options, context->options->cfg);
    expand_options.maximum_nesting = context->options->maximum_nesting;
    expand_options.maximum_items = remaining_items == 0u
        ? 1u : remaining_items;
    cm_expanded_item_sequence_init(&active_generated);
    expand = cm_expand_cfg_item_sequence(context->ast, reparse.items,
        generated_count, &expand_options, &active_generated);
    context->result.cfg = expand;
    if (expand.status != CM_MACRO_OK) {
        if (getenv("CM_MACRO_DEBUG") != NULL) {
            fprintf(stderr, "MACRO generated-item cfg failed status=%d "
                "code=%d message=%s cfg=%s\n", (int)expand.status,
                (int)expand.diagnostic.code,
                expand.diagnostic.message == NULL ? "(none)"
                    : expand.diagnostic.message,
                expand.diagnostic.cfg_diagnostic.message == NULL ? "(none)"
                    : expand.diagnostic.cfg_diagnostic.message);
        }
        cm_plan_error(context, expand.status,
            CM_ITEM_MACRO_PLAN_STAGE_CFG,
            CM_ITEM_MACRO_PLAN_DIAG_CFG,
            inputs[index].item_id, name,
            "generated-item cfg or cfg_attr expansion failed");
        cm_expanded_item_sequence_destroy(&active_generated);
        return 0;
    }
    generated_count = active_generated.item_count;
    generated = cm_plan_inputs_from_active(active_generated.items,
        generated_count, 1, source_invocation, invocation,
        definition, expansion_depth + 1u);
    expansion_scope = scope;
    inherited_scope_count = 0u;
    if (resolver_certified) {
        inherited_scope_count = scope->len;
        cm_plan_clone_scope(&generated_scope, scope);
        cm_plan_bind(&generated_scope, binding->name_ast, binding->name,
            binding->kind, binding->definition,
            binding->definition_ast, binding->crate_identifier);
        expansion_scope = &generated_scope;
    }
    if (!cm_plan_process_sequence(context, generated, generated_count,
        expansion_scope, nesting, expansion_depth + 1u, allow_macros,
        container_item, output)) {
        if (resolver_certified) cm_vec_destroy(&generated_scope);
        cm_free(generated);
        cm_expanded_item_sequence_destroy(&active_generated);
        return 0;
    }
    if (resolver_certified) {
        size_t added_scope_count;

        added_scope_count = generated_scope.len
            - inherited_scope_count - 1u;
        if (added_scope_count != 0u) {
            cm_vec_append(scope, (const CmPlanBinding *)generated_scope.data
                + inherited_scope_count + 1u, added_scope_count);
        }
        cm_vec_destroy(&generated_scope);
    }
    cm_free(generated);
    cm_expanded_item_sequence_destroy(&active_generated);
    return 1;
}

static int cm_plan_process_sequence(CmItemMacroPlanContext *context,
    const CmPlanInputItem *inputs, size_t input_count, CmVec *scope,
    unsigned int nesting, unsigned int expansion_depth, int allow_macros,
    CmAstItemId container_item, CmVec *output)
{
    size_t index;
    const CmAstItem *item;
    CmInternId name;
    CmPlanBindingKind binding_kind;
    CmItemMacroPlanNode node;

    if (input_count != 0u && inputs == NULL) {
        cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
            CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
            CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
            "nonempty planner input has no item IDs");
        return 0;
    }
    if (input_count != 0u) {
        cm_plan_select_input_anchor(context, &inputs[0]);
    }
    if (nesting >= context->options->maximum_nesting) {
        cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
            CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT, CM_AST_ITEM_NONE,
            CM_INTERN_ID_NONE, "item tree nesting limit exceeded");
        return 0;
    }
    for (index = 0u; index < input_count; ++index) {
        cm_plan_select_input_anchor(context, &inputs[index]);
        if (context->items_visited >= context->options->maximum_items) {
            cm_plan_error(context, CM_MACRO_LIMIT_EXCEEDED,
                CM_ITEM_MACRO_PLAN_STAGE_LIMIT,
                CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT,
                inputs[index].item_id, CM_INTERN_ID_NONE,
                "item macro planner item limit exceeded");
            return 0;
        }
        context->items_visited += 1u;
        item = cm_ast_get_item(context->ast, inputs[index].item_id);
        if (item == NULL) {
            cm_plan_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
                inputs[index].item_id, CM_INTERN_ID_NONE,
                "planner input contains an invalid item ID");
            return 0;
        }
        if (item->kind == CM_AST_ITEM_MACRO
            && item->name != CM_INTERN_ID_NONE) {
            if (allow_macros != CM_PLAN_MACROS_ALL
                || !cm_plan_named_definition(
                context->ast, item, &name, &binding_kind)) {
                cm_plan_error(context, CM_MACRO_UNSUPPORTED,
                    CM_ITEM_MACRO_PLAN_STAGE_RESOLVE,
                    CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO,
                    inputs[index].item_id, item->name,
                    "unsupported named macro definition");
                return 0;
            }
            if (!cm_plan_record_declaration(context, &inputs[index], item,
                    container_item)) {
                return 0;
            }
            cm_plan_bind(scope, context->ast, name, binding_kind,
                cm_plan_item_ref(context->options->current_owner,
                    inputs[index].item_id), context->ast, "crate");
            if (inputs[index].is_generated) {
                cm_plan_catalog_add(context, name, inputs[index].item_id);
            }
            continue;
        }
        if (item->kind == CM_AST_ITEM_MACRO) {
            if (!cm_plan_expand_invocation(context, inputs, index,
                input_count, item, scope, nesting, expansion_depth,
                allow_macros, container_item, output)) {
                return 0;
            }
            continue;
        }
        memset(&node, 0, sizeof(node));
        node.item_id = inputs[index].item_id;
        node.span = item->span;
        node.is_generated = inputs[index].is_generated;
        node.source_invocation = inputs[index].source_invocation;
        node.invocation = inputs[index].invocation;
        node.definition = inputs[index].definition;
        node.expansion_depth = inputs[index].expansion_depth;
        if (!cm_plan_copy_active_attributes(context, &inputs[index],
            &node)) {
            cm_plan_node_destroy(&node);
            return 0;
        }
        if (!inputs[index].is_generated
            && item->kind == CM_AST_ITEM_MODULE) {
            cm_plan_copy_external_scope(scope, &node);
        }
        if (!cm_plan_process_children(context, &inputs[index], item,
            scope, nesting, expansion_depth, container_item, &node)) {
            cm_plan_node_destroy(&node);
            return 0;
        }
        (void)cm_vec_push(output, &node);
    }
    return 1;
}

CmItemMacroPlanResult cm_plan_item_macros(const CmExpandedAst *active,
    CmAst *ast, const CmItemMacroPlanOptions *options,
    CmItemMacroPlan *plan)
{
    CmItemMacroPlanContext context;
    CmItemMacroPlanOptions effective_options;
    CmPlanInputItem *inputs;
    CmVec scope;
    CmVec roots;
    CmItemMacroItemRef no_item;
    size_t index;

    if (plan != NULL) {
        cm_item_macro_plan_destroy(plan);
        cm_item_macro_plan_init(plan);
    }
    memset(&context, 0, sizeof(context));
    context.result = cm_plan_result_init();
    cm_vec_init(&context.catalog, sizeof(CmPlanCatalogEntry));
    cm_vec_init(&context.declarations, sizeof(CmItemMacroDeclaration));
    cm_vec_init(&context.mappings, sizeof(CmItemMacroExpansion));
    cm_vec_init(&context.pending, sizeof(CmItemMacroPendingInvocation));
    if (options == NULL) {
        cm_item_macro_plan_options_init(&effective_options, NULL);
    } else {
        effective_options = *options;
    }
    context.ast = ast;
    context.options = &effective_options;
    no_item = cm_plan_item_ref(CM_ITEM_MACRO_AST_OWNER_NONE,
        CM_AST_ITEM_NONE);
    if (active == NULL || ast == NULL || plan == NULL
        || effective_options.cfg == NULL
        || effective_options.current_owner
            == CM_ITEM_MACRO_AST_OWNER_NONE
        || (effective_options.initial_scope_count != 0u
            && effective_options.initial_scope == NULL)
        || effective_options.initial_scope_count
            > effective_options.maximum_items
        || (effective_options.resolved_invocation_count != 0u
            && effective_options.resolved_invocations == NULL)
        || effective_options.resolved_invocation_count
            > effective_options.maximum_items
        || (effective_options.resolved_generated_path_count != 0u
            && effective_options.resolved_generated_paths == NULL)
        || effective_options.resolved_generated_path_count
            > effective_options.maximum_items
        || (effective_options.defer_source_invocations != 0
            && effective_options.defer_source_invocations != 1)
        || (active->crate_is_active != 0 && active->crate_is_active != 1)
        || effective_options.maximum_nesting == 0u
        || effective_options.maximum_items == 0u
        || effective_options.maximum_expansions == 0u
        || effective_options.maximum_generated_bytes == 0u) {
        cm_vec_destroy(&context.catalog);
        cm_plan_destroy_declarations(&context.declarations);
        cm_plan_destroy_mappings(&context.mappings);
        cm_vec_destroy(&context.pending);
        return context.result;
    }
    context.result.status = CM_MACRO_OK;
    context.result.kind = CM_ITEM_MACRO_PLAN_DIAG_NONE;
    context.result.message = "";
    if (effective_options.resolved_invocation_count != 0u) {
        context.resolved_used = (unsigned char *)cm_alloc_zeroed(
            effective_options.resolved_invocation_count,
            sizeof(unsigned char));
    }
    if (!cm_plan_validate_resolved_invocations(&context)) goto failure;
    if (!cm_plan_validate_resolved_generated_paths(&context)) goto failure;
    plan->owner = effective_options.current_owner;
    plan->crate_is_active = active->crate_is_active;
    plan->crate_attribute_count = active->crate_attribute_count;
    if (active->crate_attribute_count != 0u) {
        if (active->crate_attributes == NULL) {
            cm_plan_error(&context, CM_MACRO_SYNTAX_ERROR,
                CM_ITEM_MACRO_PLAN_STAGE_VALIDATE,
                CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW,
                CM_AST_ITEM_NONE, CM_INTERN_ID_NONE,
                "active crate has attribute count but no attributes");
            goto failure;
        }
        plan->crate_attributes = (CmEffectiveAttribute *)cm_alloc(
            active->crate_attribute_count * sizeof(CmEffectiveAttribute));
        memcpy(plan->crate_attributes, active->crate_attributes,
            active->crate_attribute_count * sizeof(CmEffectiveAttribute));
    }
    if (!active->crate_is_active) {
        if (!cm_plan_all_resolved_invocations_used(&context)) goto failure;
        context.result.stage = CM_ITEM_MACRO_PLAN_STAGE_COMPLETE;
        cm_free(context.resolved_used);
        cm_vec_destroy(&context.catalog);
        cm_plan_destroy_declarations(&context.declarations);
        cm_plan_destroy_mappings(&context.mappings);
        cm_vec_destroy(&context.pending);
        return context.result;
    }
    if (!cm_plan_catalog_active(&context, active->root_items,
        active->root_item_count, 0u)) {
        goto failure;
    }
    inputs = cm_plan_inputs_from_active(active->root_items,
        active->root_item_count, 0, no_item, no_item, no_item, 0u);
    cm_vec_init(&scope, sizeof(CmPlanBinding));
    if (!cm_plan_seed_scope(&context, &scope)) {
        cm_vec_destroy(&scope);
        cm_free(inputs);
        goto failure;
    }
    cm_vec_init(&roots, sizeof(CmItemMacroPlanNode));
    if (!cm_plan_process_sequence(&context, inputs,
        active->root_item_count, &scope, 0u, 0u, CM_PLAN_MACROS_ALL,
        CM_AST_ITEM_NONE, &roots)) {
        cm_plan_destroy_node_vector(&roots);
        cm_vec_destroy(&scope);
        cm_free(inputs);
        goto failure;
    }
    if (!cm_plan_all_resolved_invocations_used(&context)) {
        cm_plan_destroy_node_vector(&roots);
        cm_vec_destroy(&scope);
        cm_free(inputs);
        goto failure;
    }
    plan->roots = cm_plan_copy_nodes(&roots);
    plan->root_count = roots.len;
    plan->root_item_count = roots.len;
    if (roots.len != 0u) {
        plan->root_items = (CmAstItemId *)cm_alloc(
            roots.len * sizeof(CmAstItemId));
        for (index = 0u; index < roots.len; ++index) {
            const CmItemMacroPlanNode *node;

            node = (const CmItemMacroPlanNode *)cm_vec_at_const(
                &roots, index);
            plan->root_items[index] = node->item_id;
        }
    }
    plan->declaration_count = context.declarations.len;
    if (context.declarations.len != 0u) {
        plan->declarations = (CmItemMacroDeclaration *)cm_alloc(
            context.declarations.len * sizeof(CmItemMacroDeclaration));
        memcpy(plan->declarations, context.declarations.data,
            context.declarations.len * sizeof(CmItemMacroDeclaration));
    }
    plan->expansion_count = context.mappings.len;
    if (context.mappings.len != 0u) {
        plan->expansions = (CmItemMacroExpansion *)cm_alloc(
            context.mappings.len * sizeof(CmItemMacroExpansion));
        memcpy(plan->expansions, context.mappings.data,
            context.mappings.len * sizeof(CmItemMacroExpansion));
    }
    plan->pending_invocation_count = context.pending.len;
    if (context.pending.len != 0u) {
        plan->pending_invocations = (CmItemMacroPendingInvocation *)cm_alloc(
            context.pending.len * sizeof(CmItemMacroPendingInvocation));
        memcpy(plan->pending_invocations, context.pending.data,
            context.pending.len * sizeof(CmItemMacroPendingInvocation));
    }
    cm_vec_destroy(&context.pending);
    cm_vec_destroy(&context.mappings);
    cm_vec_destroy(&context.declarations);
    cm_vec_destroy(&roots);
    cm_vec_destroy(&scope);
    cm_free(inputs);
    context.result.status = CM_MACRO_OK;
    context.result.stage = CM_ITEM_MACRO_PLAN_STAGE_COMPLETE;
    context.result.kind = CM_ITEM_MACRO_PLAN_DIAG_NONE;
    context.result.message = "";
    context.result.items_visited = context.items_visited;
    context.result.expansions = context.expansions;
    context.result.generated_bytes = context.generated_bytes;
    cm_free(context.resolved_used);
    cm_vec_destroy(&context.catalog);
    return context.result;

failure:
    cm_item_macro_plan_destroy(plan);
    cm_plan_destroy_declarations(&context.declarations);
    cm_plan_destroy_mappings(&context.mappings);
    cm_vec_destroy(&context.pending);
    context.result.items_visited = context.items_visited;
    context.result.expansions = context.expansions;
    context.result.generated_bytes = context.generated_bytes;
    cm_free(context.resolved_used);
    cm_vec_destroy(&context.catalog);
    return context.result;
}

const char *cm_item_macro_plan_stage_name(CmItemMacroPlanStage stage)
{
    switch (stage) {
    case CM_ITEM_MACRO_PLAN_STAGE_VALIDATE: return "validate";
    case CM_ITEM_MACRO_PLAN_STAGE_CATALOG: return "catalog";
    case CM_ITEM_MACRO_PLAN_STAGE_RESOLVE: return "resolve";
    case CM_ITEM_MACRO_PLAN_STAGE_REPARSE: return "reparse";
    case CM_ITEM_MACRO_PLAN_STAGE_CFG: return "cfg";
    case CM_ITEM_MACRO_PLAN_STAGE_LIMIT: return "limit";
    case CM_ITEM_MACRO_PLAN_STAGE_COMPLETE: return "complete";
    }
    return "unknown";
}

const char *cm_item_macro_plan_diagnostic_kind_name(
    CmItemMacroPlanDiagnosticKind kind)
{
    switch (kind) {
    case CM_ITEM_MACRO_PLAN_DIAG_NONE: return "none";
    case CM_ITEM_MACRO_PLAN_DIAG_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW:
        return "invalid active view";
    case CM_ITEM_MACRO_PLAN_DIAG_FORWARD_MACRO: return "forward macro";
    case CM_ITEM_MACRO_PLAN_DIAG_OUT_OF_SCOPE_MACRO:
        return "out-of-scope macro";
    case CM_ITEM_MACRO_PLAN_DIAG_QUALIFIED_MACRO:
        return "qualified macro";
    case CM_ITEM_MACRO_PLAN_DIAG_AMBIGUOUS_MACRO:
        return "ambiguous macro";
    case CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING:
        return "invalid resolved macro binding";
    case CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO:
        return "unsupported macro";
    case CM_ITEM_MACRO_PLAN_DIAG_REPARSE: return "reparse";
    case CM_ITEM_MACRO_PLAN_DIAG_CFG: return "cfg";
    case CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT:
        return "nesting limit";
    case CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT: return "item limit";
    case CM_ITEM_MACRO_PLAN_DIAG_EXPANSION_LIMIT:
        return "expansion limit";
    case CM_ITEM_MACRO_PLAN_DIAG_OUTPUT_LIMIT: return "output limit";
    }
    return "unknown";
}
