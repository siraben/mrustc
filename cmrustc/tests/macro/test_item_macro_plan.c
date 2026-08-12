#include "cm/macro/item_macro_plan.h"

#include <stdio.h>
#include <string.h>

static int failures;

static CmItemMacroPlanResult run_failure(const char *source,
    const CmItemMacroPlanOptions *options, CmItemMacroPlan *plan_out);

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "item-plan/%s: %s\n", test, message);
    failures += 1;
}

static int parse_active(CmAst *ast, CmExpandedAst *active,
    CmCfgSet *cfg, const char *source)
{
    CmParseResult parse;
    CmExpandOptions options;
    CmExpandResult expand;

    cm_ast_init(ast);
    cm_expanded_ast_init(active);
    parse = cm_parse_crate(ast, source, strlen(source), CM_EDITION_2024);
    if (parse.error_count != 0u) {
        fprintf(stderr, "item-plan/fixture: %lu:%lu: %s\n",
            (unsigned long)parse.first_error.line,
            (unsigned long)parse.first_error.column,
            parse.first_error.message);
        failures += 1;
        cm_ast_destroy(ast);
        return 0;
    }
    cm_cfg_set_init(cfg);
    cfg->environment.target_family = "unix";
    cfg->environment.target_os = "linux";
    cm_expand_options_init(&options, cfg);
    expand = cm_expand_cfg_view(ast, &options, active);
    if (expand.status != CM_MACRO_OK) {
        fprintf(stderr, "item-plan/cfg: %s\n", expand.diagnostic.message);
        failures += 1;
        cm_expanded_ast_destroy(active);
        cm_ast_destroy(ast);
        return 0;
    }
    return 1;
}

static int name_is(const CmAst *ast, CmInternId id, const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_ast_get_string(ast, id);
    length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static int planned_name_is(const CmAst *ast,
    const CmItemMacroPlanNode *node, const char *expected)
{
    const CmAstItem *item;

    item = cm_ast_get_item(ast, node->item_id);
    return item != NULL && name_is(ast, item->name, expected);
}

static int planned_type_path_starts_with(const CmAst *ast,
    const CmItemMacroPlanNode *node, const char *expected)
{
    const CmAstItem *item;
    const CmAstType *type;
    const CmAstPath *path;

    item = cm_ast_get_item(ast, node->item_id);
    type = item == NULL || item->kind != CM_AST_ITEM_TYPE_ALIAS
        ? NULL : cm_ast_get_type(ast, item->data.value_item.type);
    path = type == NULL || type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(ast, type->path);
    return path != NULL && path->segment_count != 0u
        && name_is(ast, path->segments[0].name, expected);
}

static int planned_attribute_is(const CmItemMacroPlanNode *node,
    size_t index, const char *expected)
{
    size_t length;

    length = strlen(expected);
    return index < node->attribute_count
        && node->attributes[index].meta_length == length
        && memcmp(node->attributes[index].meta, expected, length) == 0;
}

static int declaration_attribute_is(const CmItemMacroDeclaration *declaration,
    size_t index, const char *expected)
{
    size_t length;

    length = strlen(expected);
    return index < declaration->attribute_count
        && declaration->attributes[index].meta_length == length
        && memcmp(declaration->attributes[index].meta,
            expected, length) == 0;
}

static CmAstItemId root_item(const CmAst *ast, size_t index)
{
    const CmAstItemId *id;

    id = (const CmAstItemId *)cm_vec_at_const(&ast->root_items, index);
    return id == NULL ? CM_AST_ITEM_NONE : *id;
}

static int item_ref_is(CmItemMacroItemRef reference,
    CmItemMacroAstOwner owner, CmAstItemId item)
{
    return reference.owner == owner && reference.item == item;
}

static void cleanup(CmAst *ast, CmExpandedAst *active,
    CmItemMacroPlan *plan)
{
    cm_item_macro_plan_destroy(plan);
    cm_expanded_ast_destroy(active);
    cm_ast_destroy(ast);
}

static void test_lexical_scope_and_cfg(void)
{
    static const char source[] =
        "macro_rules! emit_outer { () => { struct Outer; }; }\n"
        "emit_outer!();\n"
        "#[cfg(windows)] unknown_disabled!();\n"
        "mod nested {\n"
        "  emit_outer!();\n"
        "  macro_rules! emit_outer { () => { struct Inner; }; }\n"
        "  emit_outer!();\n"
        "}\n"
        "emit_outer!();\n";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;
    const CmAstItem *module;
    size_t roots_before;

    if (!parse_active(&ast, &active, &cfg, source)) {
        return;
    }
    roots_before = ast.root_items.len;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_COMPLETE
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_NONE
        || result.expansions != 4u || plan.root_count != 3u
        || plan.root_item_count != plan.root_count
        || plan.expansion_count != 4u) {
        fail("scope", "lexical macro plan did not complete with four expansions");
    } else {
        if (!planned_name_is(&ast, &plan.roots[0], "Outer")
            || !plan.roots[0].is_generated
            || !planned_name_is(&ast, &plan.roots[2], "Outer")
            || !plan.roots[2].is_generated) {
            fail("outer-shadow", "outer binding was not stable around module");
        }
        module = cm_ast_get_item(&ast, plan.roots[1].item_id);
        if (module == NULL || module->kind != CM_AST_ITEM_MODULE
            || !name_is(&ast, module->name, "nested")
            || plan.roots[1].child_kind != CM_EXPANDED_CHILD_MODULE
            || plan.roots[1].child_count != 2u
            || !planned_name_is(&ast, &plan.roots[1].children[0], "Outer")
            || !planned_name_is(&ast, &plan.roots[1].children[1], "Inner")) {
            fail("module-shadow", "module inheritance or local shadowing differs");
        }
        if (plan.root_items[0] != plan.roots[0].item_id
            || plan.root_items[1] != plan.roots[1].item_id
            || plan.root_items[2] != plan.roots[2].item_id) {
            fail("flat-roots", "flat active roots differ from owning tree");
        }
    }
    if (ast.root_items.len != roots_before) {
        fail("root-isolation", "planner mutated source AST root items");
    }
    cleanup(&ast, &active, &plan);
}

static void test_recursive_generated_items(void)
{
    static const char source[] =
        "macro_rules! inner { () => { struct Done; }; }"
        "macro_rules! outer { () => { inner!(); }; }"
        "outer!();";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;
    CmAstItemId generated;
    CmAstItemId inner_definition;
    CmAstItemId outer_definition;
    CmAstItemId outer_invocation;
    const CmAstItem *intermediate;

    if (!parse_active(&ast, &active, &cfg, source)) {
        return;
    }
    inner_definition = root_item(&ast, 0u);
    outer_definition = root_item(&ast, 1u);
    outer_invocation = root_item(&ast, 2u);
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 2u
        || result.generated_bytes == 0u || plan.root_count != 1u
        || !planned_name_is(&ast, &plan.roots[0], "Done")
        || !plan.roots[0].is_generated || plan.expansion_count != 2u) {
        fail("recursive", "generated invocation was not recursively expanded");
    } else {
        generated = plan.roots[0].item_id;
        intermediate = plan.expansions[0].generated_item_count == 1u
            ? cm_ast_get_item(&ast,
                plan.expansions[0].generated_items[0]) : NULL;
        if (!item_ref_is(plan.expansions[0].invocation, 1u,
                outer_invocation)
            || !item_ref_is(plan.expansions[0].definition, 1u,
                outer_definition)
            || plan.expansions[0].generated_item_count != 1u
            || intermediate == NULL
            || intermediate->kind != CM_AST_ITEM_MACRO
            || !item_ref_is(plan.expansions[1].invocation, 1u,
                plan.expansions[0].generated_items[0])
            || !item_ref_is(plan.expansions[1].definition, 1u,
                inner_definition)
            || plan.expansions[1].generated_item_count != 1u
            || plan.expansions[1].generated_items[0] != generated
            || !item_ref_is(plan.roots[0].source_invocation, 1u,
                outer_invocation)
            || !item_ref_is(plan.roots[0].invocation, 1u,
                plan.expansions[0].generated_items[0])
            || !item_ref_is(plan.roots[0].definition, 1u,
                inner_definition)
            || plan.roots[0].expansion_depth != 2u) {
            fail("recursive-mapping",
                "immediate outer/inner generated-ID mappings differ");
        }
        cm_item_macro_plan_destroy(&plan);
        if (cm_ast_get_item(&ast, generated) == NULL) {
            fail("ownership", "destroying plan invalidated generated AST item");
        }
        cm_item_macro_plan_init(&plan);
    }
    cleanup(&ast, &active, &plan);
}

static void test_rules_style_declarative_macro(void)
{
    static const char source[] =
        "macro modern {"
        "  ($name:ident) => { struct $name; },"
        "}"
        "modern!(Made);";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;
    CmAstItemId definition;
    CmAstItemId invocation;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    definition = root_item(&ast, 0u);
    invocation = root_item(&ast, 1u);
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 1u
        || plan.declaration_count != 1u || plan.root_count != 1u
        || plan.declarations[0].form
            != CM_AST_MACRO_DECLARATIVE_DEFINITION
        || !planned_name_is(&ast, &plan.roots[0], "Made")
        || !item_ref_is(plan.expansions[0].invocation, 1u, invocation)
        || !item_ref_is(plan.expansions[0].definition, 1u, definition)) {
        fail("rules-style-declarative",
            "rule-bodied declarative macro was not expanded exactly");
    }
    cleanup(&ast, &active, &plan);
}

static void test_local_parameterized_declarative_macro(void)
{
    static const char source[] =
        "macro modern($name:ident $(<$T:ident>)?) {"
        "  struct $name $(<$T>)?;"
        "}"
        "modern!(Plain);"
        "modern!(Generic<T>);";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 2u
        || plan.declaration_count != 1u || plan.root_count != 2u
        || !planned_name_is(&ast, &plan.roots[0], "Plain")
        || !planned_name_is(&ast, &plan.roots[1], "Generic")) {
        fail("local-parameterized-declarative",
            "bounded local parameterized macro did not expand exactly");
    }
    cleanup(&ast, &active, &plan);
}

static void test_impl_item_macro_invocation(void)
{
    static const char source[] =
        "macro_rules! make_method { () => {"
        "  #[cfg(unix)] default fn generated() {}"
        "  #[cfg(windows)] fn wrong() {}"
        "}; }"
        "struct Thing; impl Thing { make_method!(); }";
    static const char forbidden[] =
        "struct Thing; impl Thing { "
        "macro_rules! nested { () => { fn generated() {} }; } }";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 1u
        || plan.root_count != 2u
        || plan.roots[1].child_kind != CM_EXPANDED_CHILD_IMPL
        || plan.roots[1].child_count != 1u
        || !plan.roots[1].children[0].is_generated
        || !cm_ast_get_item(&ast,
            plan.roots[1].children[0].item_id)->is_default
        || !planned_name_is(&ast, &plan.roots[1].children[0],
            "generated")) {
        fail("impl-invocation",
            "impl item macro did not produce an associated item");
    }
    cleanup(&ast, &active, &plan);

    cm_cfg_set_init(&cfg);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = run_failure(forbidden, &options, &plan);
    if (result.status != CM_MACRO_UNSUPPORTED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO) {
        fail("impl-definition",
            "impl-local named macro definition was accepted");
    }
    cm_item_macro_plan_destroy(&plan);
}

static void test_generated_cfg_and_provenance(void)
{
    static const char source[] =
        "macro_rules! make { () => {"
        "  #[cfg(windows)] unknown_disabled!();"
        "  #[cfg_attr(unix, doc = \"generated\")] struct Kept;"
        "}; }"
        "#[cfg_attr(unix, doc = \"source\")] struct Source;"
        "make!();";
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions options;
    CmCfgSet cfg;
    CmAstItemId definition;
    CmAstItemId invocation;

    if (!parse_active(&ast, &active, &cfg, source)) {
        return;
    }
    definition = root_item(&ast, 0u);
    invocation = root_item(&ast, 2u);
    cm_item_macro_plan_init(&plan);
    result = cm_plan_item_macros(&active, &ast, NULL, &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_VALIDATE
        || plan.roots != NULL || plan.root_count != 0u) {
        fail("explicit-cfg",
            "planner accepted generated expansion without an explicit cfg set");
    }
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || plan.root_count != 2u
        || !planned_name_is(&ast, &plan.roots[0], "Source")
        || plan.roots[0].is_generated
        || !item_ref_is(plan.roots[0].source_invocation,
            CM_ITEM_MACRO_AST_OWNER_NONE, CM_AST_ITEM_NONE)
        || !item_ref_is(plan.roots[0].invocation,
            CM_ITEM_MACRO_AST_OWNER_NONE, CM_AST_ITEM_NONE)
        || !item_ref_is(plan.roots[0].definition,
            CM_ITEM_MACRO_AST_OWNER_NONE, CM_AST_ITEM_NONE)
        || plan.roots[0].expansion_depth != 0u
        || plan.roots[0].attribute_count != 1u
        || !planned_attribute_is(&plan.roots[0], 0u,
            "doc = \"source\"")
        || !planned_name_is(&ast, &plan.roots[1], "Kept")
        || !plan.roots[1].is_generated
        || !item_ref_is(plan.roots[1].source_invocation, 1u, invocation)
        || !item_ref_is(plan.roots[1].invocation, 1u, invocation)
        || !item_ref_is(plan.roots[1].definition, 1u, definition)
        || plan.roots[1].expansion_depth != 1u
        || plan.roots[1].attribute_count != 1u
        || !planned_attribute_is(&plan.roots[1], 0u,
            "doc = \"generated\"")
        || plan.roots[1].attributes[0].expansion_depth != 1u) {
        fail("generated-cfg",
            "generated cfg filtering, attributes, or provenance differ");
    }
    cleanup(&ast, &active, &plan);
}

static CmItemMacroPlanResult run_failure(const char *source,
    const CmItemMacroPlanOptions *options, CmItemMacroPlan *plan_out)
{
    CmAst ast;
    CmExpandedAst active;
    CmItemMacroPlanResult result;
    CmItemMacroPlanOptions effective_options;
    CmCfgSet cfg;

    memset(&result, 0, sizeof(result));
    cm_item_macro_plan_init(plan_out);
    if (!parse_active(&ast, &active, &cfg, source)) {
        result.status = CM_MACRO_INVALID_ARGUMENT;
        return result;
    }
    if (options == NULL) {
        cm_item_macro_plan_options_init(&effective_options, &cfg);
    } else {
        effective_options = *options;
        effective_options.cfg = &cfg;
    }
    result = cm_plan_item_macros(&active, &ast, &effective_options,
        plan_out);
    if (plan_out->roots != NULL || plan_out->root_count != 0u
        || plan_out->root_items != NULL || plan_out->root_item_count != 0u) {
        fail("failure-output", "failed plan retained partial public output");
    }
    if (plan_out->expansions != NULL || plan_out->expansion_count != 0u) {
        fail("failure-mappings", "failed plan retained expansion mappings");
    }
    if (plan_out->pending_invocations != NULL
        || plan_out->pending_invocation_count != 0u) {
        fail("failure-pending", "failed plan retained pending invocations");
    }
    cm_item_macro_plan_destroy(plan_out);
    cm_expanded_ast_destroy(&active);
    cm_ast_destroy(&ast);
    cm_item_macro_plan_init(plan_out);
    return result;
}

static void expect_resolution_failure(const char *test, const char *source,
    CmItemMacroPlanDiagnosticKind expected)
{
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;

    result = run_failure(source, NULL, &plan);
    if (result.status == CM_MACRO_OK || result.kind != expected
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_RESOLVE
        || result.message == NULL || result.message[0] == '\0'
        || result.item_id == CM_AST_ITEM_NONE) {
        fprintf(stderr, "item-plan/%s: expected %s, received %s\n", test,
            cm_item_macro_plan_diagnostic_kind_name(expected),
            cm_item_macro_plan_diagnostic_kind_name(result.kind));
        failures += 1;
    }
    cm_item_macro_plan_destroy(&plan);
}

static void test_resolution_diagnostics(void)
{
    expect_resolution_failure("forward",
        "later!(); macro_rules! later { () => { struct X; }; }",
        CM_ITEM_MACRO_PLAN_DIAG_FORWARD_MACRO);
    expect_resolution_failure("out-of-scope",
        "mod child { macro_rules! private { () => { struct X; }; } }"
        "private!();",
        CM_ITEM_MACRO_PLAN_DIAG_OUT_OF_SCOPE_MACRO);
    expect_resolution_failure("ambiguous",
        "mod a { macro_rules! same { () => { struct A; }; } }"
        "mod b { macro_rules! same { () => { struct B; }; } }"
        "same!();",
        CM_ITEM_MACRO_PLAN_DIAG_AMBIGUOUS_MACRO);
    expect_resolution_failure("qualified",
        "macro_rules! local { () => { struct X; }; } foo::local!();",
        CM_ITEM_MACRO_PLAN_DIAG_QUALIFIED_MACRO);
    expect_resolution_failure("unsupported", "unknown_macro!();",
        CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO);
    expect_resolution_failure("declarative-invocation",
        "pub macro opaque($token:tt) { $token } opaque!(value);",
        CM_ITEM_MACRO_PLAN_DIAG_UNSUPPORTED_MACRO);
}

static void test_declaration_retention(void)
{
    static const char source[] =
        "#[rustc_builtin_macro]\n"
        "#[macro_export]\n"
        "macro_rules! include { ($file:expr) => {}; }\n"
        "mod nested {\n"
        "  #[doc = \"opaque\"]\n"
        "  pub macro opaque($token:tt) { $token }\n"
        "}\n"
        "#[cfg(windows)] macro_rules! hidden { () => {}; }\n";
    CmAst ast;
    CmExpandedAst active;
    CmCfgSet cfg;
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmAstItemId include_item;
    CmAstItemId nested_item;
    const CmAstItem *opaque_item;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    include_item = root_item(&ast, 0u);
    nested_item = root_item(&ast, 1u);
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    opaque_item = plan.declaration_count == 2u
        ? cm_ast_get_item(&ast, plan.declarations[1].item_id) : NULL;
    if (result.status != CM_MACRO_OK || plan.root_count != 1u
        || plan.declaration_count != 2u
        || plan.declarations[0].item_id != include_item
        || plan.declarations[0].container_item != CM_AST_ITEM_NONE
        || plan.declarations[0].form != CM_AST_MACRO_RULES_DEFINITION
        || plan.declarations[0].attribute_count != 2u
        || !declaration_attribute_is(&plan.declarations[0], 0u,
            "rustc_builtin_macro")
        || !declaration_attribute_is(&plan.declarations[0], 1u,
            "macro_export")
        || plan.declarations[1].container_item != nested_item
        || plan.declarations[1].form
            != CM_AST_MACRO_DECLARATIVE_DEFINITION
        || !declaration_attribute_is(&plan.declarations[1], 0u,
            "doc = \"opaque\"")
        || opaque_item == NULL || !name_is(&ast, opaque_item->name, "opaque")
        || plan.declarations[0].span.start
            == plan.declarations[0].span.end
        || plan.declarations[1].span.start
            == plan.declarations[1].span.end) {
        fail("declarations",
            "cfg-active macro declaration metadata was not retained");
    }
    cleanup(&ast, &active, &plan);
}

static void test_reparse_diagnostics(void)
{
    static const char malformed[] =
        "macro_rules! bad { () => { fn }; } bad!();";
    static const char no_match[] =
        "macro_rules! ident { ($x:ident) => { struct X; }; } ident!(1);";
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;

    result = run_failure(malformed, NULL, &plan);
    if (result.status != CM_MACRO_SYNTAX_ERROR
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_REPARSE
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_REPARSE
        || result.reparse.stage != CM_MACRO_REPARSE_STAGE_PARSE
        || result.reparse.reparse.error_count == 0u) {
        fail("malformed", "malformed generated Rust lost reparse diagnostics");
    }
    cm_item_macro_plan_destroy(&plan);
    result = run_failure(no_match, NULL, &plan);
    if (result.status != CM_MACRO_NO_MATCH
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_REPARSE
        || result.reparse.stage != CM_MACRO_REPARSE_STAGE_EXPAND
        || result.reparse.expansion.stage
            != CM_MACRO_SYNTAX_STAGE_RULES_MATCH) {
        fail("no-match", "macro no-match lost expansion-stage diagnostics");
    }
    cm_item_macro_plan_destroy(&plan);
}

static void test_generated_cfg_diagnostic(void)
{
    static const char malformed[] =
        "macro_rules! bad { () => {"
        "#[cfg_attr(unix)] struct Bad;"
        "}; } bad!();";
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;

    result = run_failure(malformed, NULL, &plan);
    if (result.status != CM_MACRO_SYNTAX_ERROR
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_CFG
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_CFG
        || result.cfg.diagnostic.code
            != CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE
        || result.cfg.diagnostic.item_id == CM_AST_ITEM_NONE) {
        fail("generated-cfg-diagnostic",
            "generated cfg failure lost its transactional diagnostic");
    }
    cm_item_macro_plan_destroy(&plan);
}

static void test_exact_resolved_invocations(void)
{
    static const char definition_source[] =
        "macro_rules! imported {"
        "  ($name:ident) => { imported!(@emit $name); };"
        "  (@emit $name:ident) => { type $name = $crate::Marker; };"
        "}";
    static const char consumer_source[] =
        "alias!(ImportedOne); path::alias!(ImportedTwo);"
        "#[cfg(windows)] alias!(Disabled);";
    CmAst definition_ast;
    CmAst consumer_ast;
    CmExpandedAst definition_active;
    CmExpandedAst consumer_active;
    CmCfgSet definition_cfg;
    CmCfgSet consumer_cfg;
    CmItemMacroResolvedInvocation resolved[3];
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmAstItemId definition;
    CmAstItemId first_invocation;
    CmAstItemId second_invocation;
    CmAstItemId disabled_invocation;

    if (!parse_active(&definition_ast, &definition_active, &definition_cfg,
            definition_source)) return;
    if (!parse_active(&consumer_ast, &consumer_active, &consumer_cfg,
            consumer_source)) {
        cm_expanded_ast_destroy(&definition_active);
        cm_ast_destroy(&definition_ast);
        return;
    }
    definition = root_item(&definition_ast, 0u);
    first_invocation = root_item(&consumer_ast, 0u);
    second_invocation = root_item(&consumer_ast, 1u);
    disabled_invocation = root_item(&consumer_ast, 2u);
    memset(resolved, 0, sizeof(resolved));
    resolved[0].invocation.owner = 22u;
    resolved[0].invocation.item = first_invocation;
    resolved[0].definition.owner = 11u;
    resolved[0].definition.item = definition;
    resolved[0].definition_ast = &definition_ast;
    resolved[0].crate_identifier = "rust_core";
    resolved[1] = resolved[0];
    resolved[1].invocation.item = second_invocation;
    resolved[2] = resolved[0];
    resolved[2].invocation.item = disabled_invocation;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &consumer_cfg);
    options.current_owner = 22u;
    options.resolved_invocations = resolved;
    options.resolved_invocation_count = 2u;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 4u
        || plan.root_count != 2u || plan.expansion_count != 4u
        || !planned_name_is(&consumer_ast, &plan.roots[0], "ImportedOne")
        || !planned_name_is(&consumer_ast, &plan.roots[1], "ImportedTwo")
        || !planned_type_path_starts_with(&consumer_ast, &plan.roots[0],
            "rust_core")
        || !planned_type_path_starts_with(&consumer_ast, &plan.roots[1],
            "rust_core")
        || !item_ref_is(plan.expansions[0].invocation, 22u,
            first_invocation)
        || !item_ref_is(plan.expansions[2].invocation, 22u,
            second_invocation)
        || !item_ref_is(plan.expansions[0].definition, 11u, definition)
        || !item_ref_is(plan.expansions[1].definition, 11u, definition)
        || !item_ref_is(plan.expansions[2].definition, 11u, definition)
        || !item_ref_is(plan.expansions[3].definition, 11u, definition)) {
        fail("resolved",
            "exact imported binding or its recursive expansion was not used");
    }
    options.resolved_invocation_count = 3u;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_RESOLVE
        || result.kind
            != CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING
        || plan.root_count != 0u || plan.expansion_count != 0u) {
        fail("resolved-inactive",
            "unused exact invocation binding did not reject atomically");
    }
    options.resolved_invocation_count = 2u;
    resolved[1].invocation.item = first_invocation;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_VALIDATE
        || result.kind
            != CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING
        || plan.root_count != 0u || plan.expansion_count != 0u) {
        fail("resolved-duplicate",
            "duplicate exact invocation binding did not reject atomically");
    }
    resolved[1].invocation.item = second_invocation;
    resolved[0].crate_identifier = "core::guessed";
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_REPARSE
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_REPARSE
        || plan.root_count != 0u || plan.expansion_count != 0u) {
        fail("resolved-crate-invalid",
            "malformed defining-crate identifier was accepted");
    }
    resolved[0].crate_identifier = NULL;
    resolved[1].crate_identifier = NULL;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_OK || plan.root_count != 2u
        || !planned_type_path_starts_with(&consumer_ast, &plan.roots[0],
            "crate")
        || !planned_type_path_starts_with(&consumer_ast, &plan.roots[1],
            "crate")) {
        fail("resolved-crate-default",
            "null defining-crate identifier did not retain local default");
    }
    cleanup(&consumer_ast, &consumer_active, &plan);
    cm_expanded_ast_destroy(&definition_active);
    cm_ast_destroy(&definition_ast);
}

typedef struct GeneratedResolverFixture {
    const CmItemMacroPathSegment *expected;
    size_t expected_count;
    CmItemMacroResolvedGeneratedTarget target;
    size_t call_count;
} GeneratedResolverFixture;

static CmItemMacroGeneratedLookupStatus resolve_generated_fixture(
    void *context, const CmItemMacroPathSegment *segments,
    size_t segment_count, CmItemMacroResolvedGeneratedTarget *out_target)
{
    GeneratedResolverFixture *fixture;
    size_t index;

    fixture = (GeneratedResolverFixture *)context;
    fixture->call_count += 1u;
    if (segment_count != fixture->expected_count)
        return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
    for (index = 0u; index < segment_count; ++index) {
        if (segments[index].length != fixture->expected[index].length
            || memcmp(segments[index].bytes, fixture->expected[index].bytes,
                segments[index].length) != 0) {
            return CM_ITEM_MACRO_GENERATED_LOOKUP_NOT_FOUND;
        }
    }
    *out_target = fixture->target;
    return CM_ITEM_MACRO_GENERATED_LOOKUP_OK;
}

static void test_resolved_generated_qualified_path(void)
{
    static const char definition_source[] =
        "macro_rules! outer {"
        "  ($name:ident) => { $crate::api::helper!($name); };"
        "}"
        "macro_rules! helper {"
        "  ($name:ident) => { type $name = $crate::Marker; };"
        "}";
    static const char consumer_source[] = "alias!(GeneratedQualified);";
    static const unsigned char rust_core[] = "rust_core";
    static const unsigned char api[] = "api";
    static const unsigned char helper[] = "helper";
    CmItemMacroPathSegment segments[3];
    CmItemMacroResolvedGeneratedPath generated_paths[2];
    GeneratedResolverFixture resolver_fixture;
    CmItemMacroResolvedInvocation resolved;
    CmAst definition_ast;
    CmAst consumer_ast;
    CmExpandedAst definition_active;
    CmExpandedAst consumer_active;
    CmCfgSet definition_cfg;
    CmCfgSet consumer_cfg;
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmAstItemId outer_definition;
    CmAstItemId helper_definition;
    CmAstItemId invocation;

    if (!parse_active(&definition_ast, &definition_active, &definition_cfg,
            definition_source)) return;
    if (!parse_active(&consumer_ast, &consumer_active, &consumer_cfg,
            consumer_source)) {
        cm_expanded_ast_destroy(&definition_active);
        cm_ast_destroy(&definition_ast);
        return;
    }
    outer_definition = root_item(&definition_ast, 0u);
    helper_definition = root_item(&definition_ast, 1u);
    invocation = root_item(&consumer_ast, 0u);
    segments[0].bytes = rust_core;
    segments[0].length = sizeof(rust_core) - 1u;
    segments[1].bytes = api;
    segments[1].length = sizeof(api) - 1u;
    segments[2].bytes = helper;
    segments[2].length = sizeof(helper) - 1u;
    memset(&resolved, 0, sizeof(resolved));
    resolved.invocation.owner = 22u;
    resolved.invocation.item = invocation;
    resolved.definition.owner = 11u;
    resolved.definition.item = outer_definition;
    resolved.definition_ast = &definition_ast;
    resolved.crate_identifier = "rust_core";
    memset(generated_paths, 0, sizeof(generated_paths));
    generated_paths[0].segments = segments;
    generated_paths[0].segment_count = 3u;
    generated_paths[0].definition.owner = 11u;
    generated_paths[0].definition.item = helper_definition;
    generated_paths[0].definition_ast = &definition_ast;
    generated_paths[0].crate_identifier = "rust_core";
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &consumer_cfg);
    options.current_owner = 22u;
    options.resolved_invocations = &resolved;
    options.resolved_invocation_count = 1u;
    options.resolved_generated_paths = generated_paths;
    options.resolved_generated_path_count = 1u;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 2u
        || plan.root_count != 1u || plan.expansion_count != 2u
        || !planned_name_is(&consumer_ast, &plan.roots[0],
            "GeneratedQualified")
        || !planned_type_path_starts_with(&consumer_ast, &plan.roots[0],
            "rust_core")
        || !item_ref_is(plan.expansions[0].definition, 11u,
            outer_definition)
        || !item_ref_is(plan.expansions[1].definition, 11u,
            helper_definition)
        || !item_ref_is(plan.expansions[1].invocation, 22u,
            plan.expansions[0].generated_items[0])) {
        fail("resolved-generated-qualified",
            "generated qualified path lost its exact distinct definition");
    }
    memset(&resolver_fixture, 0, sizeof(resolver_fixture));
    resolver_fixture.expected = segments;
    resolver_fixture.expected_count = 3u;
    resolver_fixture.target.definition = generated_paths[0].definition;
    resolver_fixture.target.definition_ast = &definition_ast;
    resolver_fixture.target.crate_identifier = "rust_core";
    options.resolved_generated_path_count = 0u;
    options.resolve_generated_path = resolve_generated_fixture;
    options.resolve_generated_path_context = &resolver_fixture;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 2u
        || resolver_fixture.call_count != 1u
        || plan.expansion_count != 2u
        || !item_ref_is(plan.expansions[1].definition, 11u,
            helper_definition)) {
        fail("resolved-generated-callback",
            "generated path callback did not return the exact helper");
    }
    options.resolve_generated_path = NULL;
    options.resolve_generated_path_context = NULL;
    generated_paths[1] = generated_paths[0];
    options.resolved_generated_path_count = 2u;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_VALIDATE
        || result.kind
            != CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING
        || plan.root_count != 0u || plan.expansion_count != 0u) {
        fail("resolved-generated-duplicate",
            "duplicate generated qualified path did not reject atomically");
    }
    options.resolved_generated_path_count = 1u;
    generated_paths[0].crate_identifier = "core::guessed";
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_INVALID_ARGUMENT
        || result.stage != CM_ITEM_MACRO_PLAN_STAGE_VALIDATE
        || result.kind
            != CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING
        || plan.root_count != 0u || plan.expansion_count != 0u) {
        fail("resolved-generated-crate-invalid",
            "malformed generated-path crate identity was accepted");
    }
    cleanup(&consumer_ast, &consumer_active, &plan);
    cm_expanded_ast_destroy(&definition_active);
    cm_ast_destroy(&definition_ast);
}

static void test_resolver_certified_cfg_select(void)
{
    static const char definition_source[] =
        "pub macro cfg_select($($tt:tt)*) { /* compiler built-in */ }";
    static const char success_source[] =
        "path::select! {"
        "  windows => { struct Wrong; }"
        "  unix => { struct FirstTrue; }"
        "  _ => { struct WrongFallback; }"
        "}"
        "path::select! {"
        "  unix => { struct Ordered; }"
        "  target_os = \"linux\" => { struct TooLate; }"
        "  _ => { struct AlsoLate; }"
        "}"
        "path::select! {"
        "  windows => { struct WrongAgain; }"
        "  _ => { struct Fallback; }"
        "}";
    static const char *rejected_sources[] = {
        "path::select! { windows => { struct Never; } }",
        "path::select! { unix { struct MissingArrow; } }",
        "path::select! { _ => { struct Fallback; } unix => { struct Late; } }"
    };
    CmAst definition_ast;
    CmExpandedAst definition_active;
    CmCfgSet definition_cfg;
    CmAstItemId definition;
    CmAst consumer_ast;
    CmExpandedAst consumer_active;
    CmCfgSet consumer_cfg;
    CmItemMacroResolvedInvocation resolved[3];
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    size_t index;

    if (!parse_active(&definition_ast, &definition_active, &definition_cfg,
            definition_source)) return;
    definition = root_item(&definition_ast, 0u);
    if (!parse_active(&consumer_ast, &consumer_active, &consumer_cfg,
            success_source)) {
        cm_expanded_ast_destroy(&definition_active);
        cm_ast_destroy(&definition_ast);
        return;
    }
    memset(resolved, 0, sizeof(resolved));
    for (index = 0u; index < 3u; ++index) {
        resolved[index].invocation.owner = 22u;
        resolved[index].invocation.item = root_item(&consumer_ast,
            (uint32_t)index);
        resolved[index].definition.owner = 11u;
        resolved[index].definition.item = definition;
        resolved[index].definition_ast = &definition_ast;
        resolved[index].builtin =
            CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT;
    }
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &consumer_cfg);
    options.current_owner = 22u;
    options.resolved_invocations = resolved;
    options.resolved_invocation_count = 3u;
    result = cm_plan_item_macros(&consumer_active, &consumer_ast, &options,
        &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 3u
        || plan.root_count != 3u
        || !planned_name_is(&consumer_ast, &plan.roots[0], "FirstTrue")
        || !planned_name_is(&consumer_ast, &plan.roots[1], "Ordered")
        || !planned_name_is(&consumer_ast, &plan.roots[2], "Fallback")) {
        fail("cfg-select",
            "certified builtin did not preserve ordered cfg selection");
    }
    cleanup(&consumer_ast, &consumer_active, &plan);

    for (index = 0u; index < CM_ARRAY_LEN(rejected_sources); ++index) {
        CmItemMacroResolvedInvocation exact;

        if (!parse_active(&consumer_ast, &consumer_active, &consumer_cfg,
                rejected_sources[index])) continue;
        memset(&exact, 0, sizeof(exact));
        exact.invocation.owner = 22u;
        exact.invocation.item = root_item(&consumer_ast, 0u);
        exact.definition.owner = 11u;
        exact.definition.item = definition;
        exact.definition_ast = &definition_ast;
        exact.builtin = CM_ITEM_MACRO_RESOLVED_BUILTIN_CFG_SELECT;
        cm_item_macro_plan_init(&plan);
        cm_item_macro_plan_options_init(&options, &consumer_cfg);
        options.current_owner = 22u;
        options.resolved_invocations = &exact;
        options.resolved_invocation_count = 1u;
        result = cm_plan_item_macros(&consumer_active, &consumer_ast,
            &options, &plan);
        if (result.status == CM_MACRO_OK
            || result.stage != CM_ITEM_MACRO_PLAN_STAGE_REPARSE
            || result.kind != CM_ITEM_MACRO_PLAN_DIAG_REPARSE) {
            fail("cfg-select-reject",
                "malformed or unmatched cfg_select input was accepted");
        }
        cleanup(&consumer_ast, &consumer_active, &plan);
    }

    if (parse_active(&consumer_ast, &consumer_active, &consumer_cfg,
            "path::select! { _ => { struct Spoofed; } }")) {
        CmItemMacroResolvedInvocation spoofed;

        memset(&spoofed, 0, sizeof(spoofed));
        spoofed.invocation.owner = 22u;
        spoofed.invocation.item = root_item(&consumer_ast, 0u);
        spoofed.definition.owner = 11u;
        spoofed.definition.item = definition;
        spoofed.definition_ast = &definition_ast;
        spoofed.builtin = (CmItemMacroResolvedBuiltin)99;
        cm_item_macro_plan_init(&plan);
        cm_item_macro_plan_options_init(&options, &consumer_cfg);
        options.current_owner = 22u;
        options.resolved_invocations = &spoofed;
        options.resolved_invocation_count = 1u;
        result = cm_plan_item_macros(&consumer_active, &consumer_ast,
            &options, &plan);
        if (result.status != CM_MACRO_INVALID_ARGUMENT
            || result.stage != CM_ITEM_MACRO_PLAN_STAGE_VALIDATE
            || result.kind
                != CM_ITEM_MACRO_PLAN_DIAG_INVALID_RESOLVED_BINDING) {
            fail("cfg-select-certificate",
                "invalid builtin certificate was not rejected");
        }
        cleanup(&consumer_ast, &consumer_active, &plan);
    }
    cm_expanded_ast_destroy(&definition_active);
    cm_ast_destroy(&definition_ast);
}

static void test_deferred_source_invocations(void)
{
    static const char source[] =
        "struct Before; unknown!(); path::also!(); struct After;";
    CmAst ast;
    CmExpandedAst active;
    CmCfgSet cfg;
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmItemMacroResolvedInvocation exact;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    options.current_owner = 7u;
    options.defer_source_invocations = 1;
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || result.expansions != 0u
        || plan.root_count != 2u
        || !planned_name_is(&ast, &plan.roots[0], "Before")
        || !planned_name_is(&ast, &plan.roots[1], "After")
        || plan.pending_invocation_count != 2u
        || !item_ref_is(plan.pending_invocations[0].invocation, 7u,
            root_item(&ast, 1u))
        || plan.pending_invocations[0].is_generated
        || !item_ref_is(plan.pending_invocations[0].source_invocation, 7u,
            root_item(&ast, 1u))
        || plan.pending_invocations[0].is_qualified
        || !item_ref_is(plan.pending_invocations[1].invocation, 7u,
            root_item(&ast, 2u))
        || plan.pending_invocations[1].is_generated
        || !item_ref_is(plan.pending_invocations[1].source_invocation, 7u,
            root_item(&ast, 2u))
        || !plan.pending_invocations[1].is_qualified) {
        fail("deferred", "source skeleton did not retain exact pending calls");
    }
    cleanup(&ast, &active, &plan);

    if (!parse_active(&ast, &active, &cfg,
            "macro_rules! outer { () => { external!(); struct Kept; }; }"
            "outer!();")) return;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    options.current_owner = 7u;
    options.defer_source_invocations = 1;
    memset(&exact, 0, sizeof(exact));
    exact.invocation.owner = 7u;
    exact.invocation.item = root_item(&ast, 1u);
    exact.definition.owner = 7u;
    exact.definition.item = root_item(&ast, 0u);
    exact.definition_ast = &ast;
    options.resolved_invocations = &exact;
    options.resolved_invocation_count = 1u;
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK || plan.root_count != 1u
        || !planned_name_is(&ast, &plan.roots[0], "Kept")
        || plan.pending_invocation_count != 1u
        || !plan.pending_invocations[0].is_generated
        || !item_ref_is(plan.pending_invocations[0].source_invocation, 7u,
            root_item(&ast, 1u))
        || plan.pending_invocations[0].invocation.item
            == root_item(&ast, 1u)
        || plan.pending_invocations[0].span.start
            != cm_ast_get_item(&ast, root_item(&ast, 1u))->span.start
        || plan.pending_invocations[0].span.end
            != cm_ast_get_item(&ast, root_item(&ast, 1u))->span.end) {
        fail("deferred-generated",
            "generated pending call lost its stable source anchor");
    }
    cleanup(&ast, &active, &plan);

    cm_cfg_set_init(&cfg);
    cm_item_macro_plan_options_init(&options, &cfg);
    options.defer_source_invocations = 1;
    result = run_failure(
        "later!(); macro_rules! later { () => { struct X; }; }",
        &options, &plan);
    if (result.kind != CM_ITEM_MACRO_PLAN_DIAG_FORWARD_MACRO) {
        fail("deferred-forward", "defer mode accepted a local forward macro");
    }
    cm_item_macro_plan_destroy(&plan);

    if (!parse_active(&ast, &active, &cfg,
            "struct Thing; mod nested { impl Thing { unknown!(); } }")) {
        return;
    }
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    options.current_owner = 9u;
    options.defer_source_invocations = 1;
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    if (result.status != CM_MACRO_OK
        || plan.pending_invocation_count != 1u
        || plan.pending_invocations[0].container_item
            != root_item(&ast, 1u)) {
        fail("deferred-container",
            "non-module source call lost its nearest module container");
    }
    cleanup(&ast, &active, &plan);
}

static void test_cross_ast_inherited_scope(void)
{
    static const char parent_source[] =
        "macro_rules! inherited { () => { struct FromParent; } }";
    static const char child_source[] =
        "struct Padding; inherited!();"
        "mod early;"
        "macro_rules! inherited { () => { struct FromChild; } }"
        "inherited!(); mod later;";
    CmAst parent_ast;
    CmAst child_ast;
    CmExpandedAst parent_active;
    CmExpandedAst child_active;
    CmCfgSet parent_cfg;
    CmCfgSet child_cfg;
    CmItemMacroScopeSeed seed;
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmAstItemId parent_definition;
    CmAstItemId child_definition;

    if (!parse_active(&parent_ast, &parent_active, &parent_cfg,
            parent_source)) return;
    if (!parse_active(&child_ast, &child_active, &child_cfg, child_source)) {
        cm_expanded_ast_destroy(&parent_active);
        cm_ast_destroy(&parent_ast);
        return;
    }
    parent_definition = root_item(&parent_ast, 0u);
    child_definition = root_item(&child_ast, 3u);
    memset(&seed, 0, sizeof(seed));
    seed.definition.owner = 11u;
    seed.definition.item = parent_definition;
    seed.definition_ast = &parent_ast;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &child_cfg);
    options.current_owner = 22u;
    options.initial_scope = &seed;
    options.initial_scope_count = 1u;
    result = cm_plan_item_macros(&child_active, &child_ast, &options, &plan);
    if (result.status != CM_MACRO_OK || plan.owner != 22u
        || plan.root_count != 5u
        || !planned_name_is(&child_ast, &plan.roots[1], "FromParent")
        || !item_ref_is(plan.roots[1].source_invocation, 22u,
            root_item(&child_ast, 1u))
        || !item_ref_is(plan.roots[1].invocation, 22u,
            root_item(&child_ast, 1u))
        || !item_ref_is(plan.roots[1].definition, 11u,
            parent_definition)
        || plan.roots[2].external_scope_count != 1u
        || !item_ref_is(plan.roots[2].external_scope[0], 11u,
            parent_definition)
        || !planned_name_is(&child_ast, &plan.roots[3], "FromChild")
        || !item_ref_is(plan.roots[3].definition, 22u,
            child_definition)
        || plan.roots[4].external_scope_count != 2u
        || !item_ref_is(plan.roots[4].external_scope[0], 11u,
            parent_definition)
        || !item_ref_is(plan.roots[4].external_scope[1], 22u,
            child_definition)) {
        fail("cross-ast-scope",
            "inherited lookup, local shadowing, or external snapshot differs");
    }
    cleanup(&child_ast, &child_active, &plan);
    cm_expanded_ast_destroy(&parent_active);
    cm_ast_destroy(&parent_ast);
}

static void test_crate_activity_is_retained(void)
{
    static const char active_empty[] = "";
    static const char inactive_empty[] = "#![cfg(windows)]";
    const char *sources[2];
    int expected[2];
    size_t index;

    sources[0] = active_empty;
    sources[1] = inactive_empty;
    expected[0] = 1;
    expected[1] = 0;
    for (index = 0u; index < 2u; ++index) {
        CmAst ast;
        CmExpandedAst active;
        CmCfgSet cfg;
        CmItemMacroPlanOptions options;
        CmItemMacroPlan plan;
        CmItemMacroPlanResult result;

        if (!parse_active(&ast, &active, &cfg, sources[index])) continue;
        cm_item_macro_plan_init(&plan);
        cm_item_macro_plan_options_init(&options, &cfg);
        options.current_owner = 1u;
        result = cm_plan_item_macros(&active, &ast, &options, &plan);
        if (result.status != CM_MACRO_OK || plan.root_count != 0u
            || plan.crate_is_active != expected[index]) {
            fail("crate-activity",
                "planner conflated an active empty crate with inactive cfg");
        }
        cleanup(&ast, &active, &plan);
    }
}

static void test_impossible_inner_attribute_view_is_rejected(void)
{
    static const char source[] = "#![allow(dead_code)] struct Plain;";
    CmAst ast;
    CmExpandedAst active;
    CmCfgSet cfg;
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;

    if (!parse_active(&ast, &active, &cfg, source)) return;
    if (active.root_item_count != 1u
        || active.crate_attribute_count != 1u) {
        fail("inner-placement", "fixture did not retain its crate attribute");
        cm_expanded_ast_destroy(&active);
        cm_ast_destroy(&ast);
        return;
    }
    active.root_items[0].inner_attributes = active.crate_attributes;
    active.root_items[0].inner_attribute_count = 1u;
    cm_item_macro_plan_init(&plan);
    cm_item_macro_plan_options_init(&options, &cfg);
    options.current_owner = 1u;
    result = cm_plan_item_macros(&active, &ast, &options, &plan);
    active.root_items[0].inner_attributes = NULL;
    active.root_items[0].inner_attribute_count = 0u;
    if (result.status != CM_MACRO_SYNTAX_ERROR
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_INVALID_ACTIVE_VIEW) {
        fail("inner-placement",
            "planner accepted an inner attribute on a non-module item");
    }
    cleanup(&ast, &active, &plan);
}

static void test_limits(void)
{
    static const char recursive[] =
        "macro_rules! again { () => { again!(); }; } again!();";
    static const char two_items[] =
        "macro_rules! make { () => { struct A; }; } struct Existing;";
    static const char nested_expansion[] =
        "macro_rules! inner { () => { struct Done; }; }"
        "macro_rules! outer { () => { inner!(); }; } outer!();";
    static const char later_limit[] =
        "macro_rules! make { () => { struct Generated; }; }"
        "make!(); struct Later;";
    CmItemMacroPlanOptions options;
    CmItemMacroPlan plan;
    CmItemMacroPlanResult result;
    CmCfgSet cfg;

    cm_cfg_set_init(&cfg);

    cm_item_macro_plan_options_init(&options, &cfg);
    options.maximum_nesting = 2u;
    result = run_failure(recursive, &options, &plan);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_NESTING_LIMIT) {
        fail("depth-limit", "recursive expansion depth was not bounded");
    }
    cm_item_macro_plan_destroy(&plan);

    cm_item_macro_plan_options_init(&options, &cfg);
    options.maximum_items = 3u;
    result = run_failure(later_limit, &options, &plan);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT
        || result.item_id == CM_AST_ITEM_NONE
        || !item_ref_is(result.source_invocation, 1u, result.item_id)) {
        fail("failure-anchor",
            "later source failure retained an earlier macro invocation span");
    }
    cm_item_macro_plan_destroy(&plan);

    cm_item_macro_plan_options_init(&options, &cfg);
    options.maximum_items = 1u;
    result = run_failure(two_items, &options, &plan);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_ITEM_LIMIT) {
        fail("item-limit", "planner item traversal was not bounded");
    }
    cm_item_macro_plan_destroy(&plan);

    cm_item_macro_plan_options_init(&options, &cfg);
    options.maximum_expansions = 1u;
    result = run_failure(nested_expansion, &options, &plan);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_EXPANSION_LIMIT) {
        fail("expansion-limit", "recursive expansion count was not bounded");
    }
    cm_item_macro_plan_destroy(&plan);

    cm_item_macro_plan_options_init(&options, &cfg);
    options.maximum_generated_bytes = 1u;
    result = run_failure(nested_expansion, &options, &plan);
    if (result.status != CM_MACRO_LIMIT_EXCEEDED
        || result.kind != CM_ITEM_MACRO_PLAN_DIAG_OUTPUT_LIMIT) {
        fail("output-limit", "total generated source was not bounded");
    }
    cm_item_macro_plan_destroy(&plan);
}

int main(void)
{
    test_lexical_scope_and_cfg();
    test_recursive_generated_items();
    test_rules_style_declarative_macro();
    test_local_parameterized_declarative_macro();
    test_impl_item_macro_invocation();
    test_generated_cfg_and_provenance();
    test_declaration_retention();
    test_resolution_diagnostics();
    test_reparse_diagnostics();
    test_generated_cfg_diagnostic();
    test_exact_resolved_invocations();
    test_resolved_generated_qualified_path();
    test_resolver_certified_cfg_select();
    test_deferred_source_invocations();
    test_cross_ast_inherited_scope();
    test_crate_activity_is_retained();
    test_impossible_inner_attribute_view_is_rejected();
    test_limits();
    if (strcmp(cm_item_macro_plan_stage_name(
        CM_ITEM_MACRO_PLAN_STAGE_COMPLETE), "complete") != 0) {
        fail("names", "planner stage names are unstable");
    }
    if (failures != 0) {
        fprintf(stderr, "item macro planner tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("item macro planner tests: ok");
    return 0;
}
