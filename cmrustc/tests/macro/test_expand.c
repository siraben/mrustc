#include "cm/macro/expand.h"
#include "cm/syntax/parser.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "expand/%s: %s\n", test, message);
    failures += 1;
}

static int parse_source(CmAst *ast, const char *source)
{
    CmParseResult result;

    cm_ast_init(ast);
    result = cm_parse_crate(ast, source, strlen(source), CM_EDITION_2024);
    if (result.error_count != 0u) {
        fprintf(stderr, "expand/parse: %lu:%lu: %s\n",
            (unsigned long)result.first_error.line,
            (unsigned long)result.first_error.column,
            result.first_error.message);
        cm_ast_destroy(ast);
        return 0;
    }
    return 1;
}

static int interned_is(const CmAst *ast, CmInternId id, const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_ast_get_string(ast, id);
    length = strlen(text);
    return string != NULL && string->len == length
        && memcmp(string->bytes, text, length) == 0;
}

static int meta_is(const CmEffectiveAttribute *attribute, const char *text)
{
    size_t length;

    length = strlen(text);
    return attribute->meta_length == length
        && memcmp(attribute->meta, text, length) == 0;
}

static const CmAstItem *source_item(const CmAst *ast,
    const CmExpandedItem *item)
{
    return cm_ast_get_item(ast, item->source_id);
}

static const CmExpandedItem *find_child(const CmAst *ast,
    const CmExpandedItem *items, size_t count, const char *name)
{
    size_t index;
    const CmAstItem *item;

    for (index = 0u; index < count; ++index) {
        item = source_item(ast, &items[index]);
        if (item != NULL && interned_is(ast, item->name, name)) {
            return &items[index];
        }
    }
    return NULL;
}

static int effective_spans_match_source(const char *source,
    const CmEffectiveAttribute *attributes, size_t count)
{
    size_t index;
    size_t length;

    length = strlen(source);
    for (index = 0u; index < count; ++index) {
        if (attributes[index].span.end < attributes[index].span.start
            || (size_t)attributes[index].span.end > length
            || (size_t)(attributes[index].span.end
                - attributes[index].span.start)
                != attributes[index].meta_length
            || memcmp(source + attributes[index].span.start,
                attributes[index].meta,
                attributes[index].meta_length) != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_structural_view(void)
{
    static const char source[] =
        "#![cfg_attr(unix, allow(dead_code), "
            "cfg_attr(feature = \"extra\", doc = \"nested\"))]\n"
        "#[doc = \"before\"]\n"
        "#[cfg(unix)]\n"
        "#[cfg_attr(all(unix, target_os = \"linux\"), "
            "repr(C), derive(Clone),)]\n"
        "#[doc = \"after\"]\n"
        "struct Keep;\n"
        "#[cfg(windows)] struct Drop;\n"
        "#[cfg(unix)]\n"
        "mod inner {\n"
        "    #![cfg_attr(unix, allow(dead_code))]\n"
        "    #[cfg(feature = \"extra\")] fn feature_on() {}\n"
        "    #[cfg_attr(not(feature = \"extra\"), inline)] "
            "fn still_present() {}\n"
        "    extern \"C\" {\n"
        "        #[cfg(unix)] fn kept_foreign();\n"
        "        #[cfg(windows)] fn dropped_foreign();\n"
        "    }\n"
        "}\n";
    static const char *features[] = { "extra" };
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedAst expanded;
    CmExpandResult result;
    const CmExpandedItem *keep;
    const CmExpandedItem *inner;
    const CmExpandedItem *feature_on;
    const CmExpandedItem *still_present;
    const CmExpandedItem *foreign;
    const CmAstItem *keep_source;
    size_t item_count_before;
    size_t attribute_count_before;
    uint32_t keep_attribute_count;

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    item_count_before = ast.items.len;
    attribute_count_before = ast.attributes.len;
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cfg.environment.target_os = "linux";
    cfg.environment.features = features;
    cfg.environment.feature_count = CM_ARRAY_LEN(features);
    cm_expand_options_init(&options, &cfg);
    cm_expanded_ast_init(&expanded);
    result = cm_expand_cfg_view(&ast, &options, &expanded);
    if (result.status != CM_MACRO_OK) {
        fprintf(stderr, "expand/structural: %s: %s\n",
            cm_expand_diagnostic_code_name(result.diagnostic.code),
            result.diagnostic.message);
        failures += 1;
        cm_expanded_ast_destroy(&expanded);
        cm_ast_destroy(&ast);
        return;
    }
    if (!expanded.crate_is_active || expanded.root_item_count != 2u) {
        fail("root-filter", "disabled root item was not filtered");
    }
    if (expanded.crate_attribute_count != 2u
        || !meta_is(&expanded.crate_attributes[0], "allow(dead_code)")
        || !meta_is(&expanded.crate_attributes[1], "doc = \"nested\"")
        || expanded.crate_attributes[0].expansion_depth != 1u
        || expanded.crate_attributes[1].expansion_depth != 2u
        || expanded.crate_attributes[0].source_id
            != expanded.crate_attributes[1].source_id) {
        fail("crate-cfg-attr", "nested crate cfg_attr order is wrong");
    }
    keep = find_child(&ast, expanded.root_items,
        expanded.root_item_count, "Keep");
    inner = find_child(&ast, expanded.root_items,
        expanded.root_item_count, "inner");
    if (keep == NULL || inner == NULL
        || find_child(&ast, expanded.root_items,
            expanded.root_item_count, "Drop") != NULL) {
        fail("root-identity", "active root IDs do not match source items");
    } else {
        keep_source = source_item(&ast, keep);
        keep_attribute_count = keep_source == NULL
            ? 0u : keep_source->attribute_count;
        if (keep->attribute_count != 4u
            || !meta_is(&keep->attributes[0], "doc = \"before\"")
            || !meta_is(&keep->attributes[1], "repr(C)")
            || !meta_is(&keep->attributes[2], "derive(Clone)")
            || !meta_is(&keep->attributes[3], "doc = \"after\"")
            || keep->attributes[1].source_id
                != keep->attributes[2].source_id
            || keep->attributes[0].source_id
                == keep->attributes[1].source_id
            || keep->span.start != keep_source->span.start
            || keep->span.end != keep_source->span.end) {
            fail("attribute-order", "effective item attributes lost order or provenance");
        }
        if (!effective_spans_match_source(source, keep->attributes,
            keep->attribute_count)
            || !effective_spans_match_source(source,
                expanded.crate_attributes,
                expanded.crate_attribute_count)) {
            fail("attribute-spans", "effective attribute span is not exact");
        }
        if (keep_attribute_count != 4u) {
            fail("source-count", "unexpected parser attribute count");
        }
    }
    if (inner != NULL) {
        if (inner->child_kind != CM_EXPANDED_CHILD_MODULE
            || inner->child_count != 3u
            || inner->inner_attribute_count != 1u
            || !meta_is(&inner->inner_attributes[0], "allow(dead_code)")
            || inner->inner_attributes[0].expansion_depth != 1u) {
            fail("inline-module", "inline module active view is incomplete");
        } else {
            feature_on = find_child(&ast, inner->children,
                inner->child_count, "feature_on");
            still_present = find_child(&ast, inner->children,
                inner->child_count, "still_present");
            foreign = NULL;
            if (inner->children[2].child_kind
                == CM_EXPANDED_CHILD_EXTERN_BLOCK) {
                foreign = &inner->children[2];
            }
            if (feature_on == NULL || still_present == NULL
                || still_present->attribute_count != 0u) {
                fail("nested-functions", "nested cfg or false cfg_attr behaved incorrectly");
            }
            if (foreign == NULL || foreign->child_count != 1u
                || foreign->children[0].source_id == CM_AST_ITEM_NONE
                || source_item(&ast, &foreign->children[0]) == NULL
                || !interned_is(&ast,
                    source_item(&ast, &foreign->children[0])->name,
                    "kept_foreign")) {
                fail("extern-members", "extern-block members were not filtered");
            }
        }
    }
    if (ast.items.len != item_count_before
        || ast.attributes.len != attribute_count_before) {
        fail("immutable-source", "cfg application mutated AST tables");
    }
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void expect_diagnostic(const char *test, const char *source,
    CmExpandDiagnosticCode code, CmMacroDiagnosticCode cfg_code,
    unsigned int maximum_nesting, size_t maximum_items,
    size_t maximum_attributes)
{
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedAst expanded;
    CmExpandResult result;

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_expand_options_init(&options, &cfg);
    options.maximum_nesting = maximum_nesting;
    options.maximum_items = maximum_items;
    options.maximum_attribute_expansions = maximum_attributes;
    cm_expanded_ast_init(&expanded);
    result = cm_expand_cfg_view(&ast, &options, &expanded);
    if (result.status == CM_MACRO_OK || result.diagnostic.code != code
        || result.diagnostic.message == NULL
        || result.diagnostic.message[0] == '\0'
        || (cfg_code != CM_MACRO_DIAG_NONE
            && result.diagnostic.cfg_diagnostic.code != cfg_code)) {
        fprintf(stderr, "expand/%s: expected %s, received %s\n", test,
            cm_expand_diagnostic_code_name(code),
            cm_expand_diagnostic_code_name(result.diagnostic.code));
        failures += 1;
    }
    if (expanded.root_items != NULL || expanded.root_item_count != 0u
        || expanded.crate_attributes != NULL) {
        fail(test, "failed expansion retained a partial view");
    }
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_diagnostics_and_limits(void)
{
    expect_diagnostic("predicate",
        "#[cfg(not(unix, windows))] struct Bad;",
        CM_EXPAND_DIAG_CFG_PREDICATE, CM_MACRO_DIAG_CFG_NOT_ARITY,
        64u, 100u, 100u);
    expect_diagnostic("cfg-without-predicate",
        "#[cfg] struct Bad;",
        CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, CM_MACRO_DIAG_NONE,
        64u, 100u, 100u);
    expect_diagnostic("missing-payload",
        "#[cfg_attr(unix)] struct Bad;",
        CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, CM_MACRO_DIAG_NONE,
        64u, 100u, 100u);
    expect_diagnostic("unsupported-payload",
        "#[cfg_attr(unix, = broken)] struct Bad;",
        CM_EXPAND_DIAG_UNSUPPORTED_ATTRIBUTE, CM_MACRO_DIAG_NONE,
        64u, 100u, 100u);
    expect_diagnostic("cfg-attr-depth",
        "#[cfg_attr(unix, cfg_attr(unix, inline))] fn deep() {}",
        CM_EXPAND_DIAG_NESTING_LIMIT, CM_MACRO_DIAG_NONE,
        1u, 100u, 100u);
    expect_diagnostic("item-limit", "struct One; struct Two;",
        CM_EXPAND_DIAG_ITEM_LIMIT, CM_MACRO_DIAG_NONE, 64u, 1u, 100u);
    expect_diagnostic("attribute-limit",
        "#[inline] #[cold] fn too_many() {}",
        CM_EXPAND_DIAG_ATTRIBUTE_LIMIT, CM_MACRO_DIAG_NONE,
        64u, 100u, 1u);
}

static void test_inactive_crate(void)
{
    static const char source[] =
        "#![cfg(windows)]\n"
        "#[cfg_attr(unix, inline)] fn hidden() {}\n";
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedAst expanded;
    CmExpandResult result;

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_expand_options_init(&options, &cfg);
    cm_expanded_ast_init(&expanded);
    result = cm_expand_cfg_view(&ast, &options, &expanded);
    if (result.status != CM_MACRO_OK || expanded.crate_is_active
        || expanded.root_item_count != 0u) {
        fail("inactive-crate", "crate cfg did not suppress root view");
    }
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_arbitrary_item_sequence(void)
{
    static const char source[] =
        "struct Unselected;\n"
        "#[cfg(windows)] struct Drop;\n"
        "#[cfg_attr(unix, doc = \"kept\")] struct Keep;\n"
        "#[cfg_attr(unix)] struct Bad;\n";
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedItemSequence expanded;
    CmExpandResult result;
    CmAstItemId selected[2];
    CmAstItemId bad;

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    selected[0] = *(const CmAstItemId *)cm_vec_at_const(
        &ast.root_items, 2u);
    selected[1] = *(const CmAstItemId *)cm_vec_at_const(
        &ast.root_items, 1u);
    bad = *(const CmAstItemId *)cm_vec_at_const(&ast.root_items, 3u);
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_expand_options_init(&options, &cfg);
    cm_expanded_item_sequence_init(&expanded);
    result = cm_expand_cfg_item_sequence(&ast, selected,
        CM_ARRAY_LEN(selected), &options, &expanded);
    if (result.status != CM_MACRO_OK || expanded.item_count != 1u
        || source_item(&ast, &expanded.items[0]) == NULL
        || !interned_is(&ast,
            source_item(&ast, &expanded.items[0])->name, "Keep")
        || expanded.items[0].attribute_count != 1u
        || !meta_is(&expanded.items[0].attributes[0],
            "doc = \"kept\"")
        || expanded.items[0].attributes[0].expansion_depth != 1u) {
        fail("item-sequence",
            "arbitrary item sequence was not filtered into an owning view");
    }
    result = cm_expand_cfg_item_sequence(&ast, &bad, 1u, &options,
        &expanded);
    if (result.status == CM_MACRO_OK
        || result.diagnostic.code != CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE
        || expanded.items != NULL || expanded.item_count != 0u) {
        fail("item-sequence-transaction",
            "failed sequence expansion retained the previous view");
    }
    cm_expanded_item_sequence_destroy(&expanded);
    cm_ast_destroy(&ast);
}

/* libc's `s!` emits `#[::core::prelude::v1::derive(..)]` in token-spaced
 * generated text: a path-headed attribute is kept, not rejected. */
static void test_path_headed_attribute(void)
{
    static const char source[] =
        "#[:: core :: prelude :: v1 :: derive (:: core :: clone :: Clone)]\n"
        "#[cfg_attr(unix, ::core::prelude::v1::derive(Copy))]\n"
        "struct Keep;\n";
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedItemSequence expanded;
    CmExpandResult result;
    CmAstItemId selected[1];

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    selected[0] = *(const CmAstItemId *)cm_vec_at_const(
        &ast.root_items, 0u);
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_expand_options_init(&options, &cfg);
    cm_expanded_item_sequence_init(&expanded);
    result = cm_expand_cfg_item_sequence(&ast, selected, 1u, &options,
        &expanded);
    if (result.status != CM_MACRO_OK || expanded.item_count != 1u
        || expanded.items[0].attribute_count != 2u
        || !meta_is(&expanded.items[0].attributes[1],
            "::core::prelude::v1::derive(Copy)")) {
        fail("path-headed-attribute",
            "a path-headed attribute was rejected or dropped");
    }
    cm_expanded_item_sequence_destroy(&expanded);
    cm_ast_destroy(&ast);
}

static void test_inline_module_inner_cfg(void)
{
    static const char source[] =
        "mod Hidden { #![cfg(windows)] struct Child; }\n"
        "mod Shown { #![cfg(unix)] struct Child; }\n";
    CmAst ast;
    CmCfgSet cfg;
    CmExpandOptions options;
    CmExpandedAst expanded;
    CmExpandResult result;
    const CmExpandedItem *shown;

    if (!parse_source(&ast, source)) {
        failures += 1;
        return;
    }
    cm_cfg_set_init(&cfg);
    cfg.environment.target_family = "unix";
    cm_expand_options_init(&options, &cfg);
    cm_expanded_ast_init(&expanded);
    result = cm_expand_cfg_view(&ast, &options, &expanded);
    shown = find_child(&ast, expanded.root_items,
        expanded.root_item_count, "Shown");
    if (result.status != CM_MACRO_OK || expanded.root_item_count != 1u
        || shown == NULL || shown->child_count != 1u
        || shown->inner_attribute_count != 0u
        || find_child(&ast, expanded.root_items,
            expanded.root_item_count, "Hidden") != NULL) {
        fail("module-inner-cfg",
            "inline-module inner cfg did not configure its module owner");
    }
    cm_expanded_ast_destroy(&expanded);
    cm_ast_destroy(&ast);
}

int main(void)
{
    test_structural_view();
    test_diagnostics_and_limits();
    test_inactive_crate();
    test_arbitrary_item_sequence();
    test_path_headed_attribute();
    test_inline_module_inner_cfg();
    if (failures != 0) {
        fprintf(stderr, "cfg expansion tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("cfg expansion tests: ok");
    return 0;
}
