#include "cm/macro_rules.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

typedef enum CmMacroPatternMode {
    CM_MACRO_PATTERN_MATCHER = 0,
    CM_MACRO_PATTERN_TRANSCRIBER
} CmMacroPatternMode;

typedef struct CmNestedMacroBinding {
    size_t name_start;
    size_t name_length;
    size_t scope_start;
    size_t scope_end;
} CmNestedMacroBinding;

typedef struct CmMacroRulesParser {
    CmMacroRulesDefinition *definition;
    size_t binding_start;
    CmVec nested_bindings;
    unsigned int nested_macro_depth;
    CmMacroStatus status;
    CmMacroDiagnostic diagnostic;
} CmMacroRulesParser;

static void cm_rules_parse_error(CmMacroRulesParser *parser,
    CmMacroStatus status, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    if (parser->status != CM_MACRO_OK) {
        return;
    }
    parser->status = status;
    parser->diagnostic.code = code;
    parser->diagnostic.offset = offset;
    parser->diagnostic.message = message;
}

static size_t cm_rules_node_offset(const struct cm_tt_node *node)
{
    if (node == NULL) {
        return 0;
    }
    if (node->kind == CM_TT_NODE_TOKEN) {
        return node->data.token.start;
    }
    if (node->kind == CM_TT_NODE_GROUP) {
        return node->data.group.open_span.start;
    }
    return 0;
}

static int cm_rules_source_range_valid(const CmMacroRulesDefinition *definition,
    size_t start, size_t length)
{
    return start <= definition->source_length
        && length <= definition->source_length - start;
}

static int cm_rules_token_text_equal(
    const CmMacroRulesDefinition *definition,
    const struct cm_tt_node *node,
    const char *text)
{
    size_t length;

    if (node == NULL || node->kind != CM_TT_NODE_TOKEN) {
        return 0;
    }
    length = strlen(text);
    return node->data.token.length == length
        && cm_rules_source_range_valid(definition,
            node->data.token.start, node->data.token.length)
        && memcmp(definition->source + node->data.token.start,
            text, length) == 0;
}

static CmMacroPatternNode *cm_rules_pattern_mut(
    CmMacroRulesDefinition *definition, CmMacroPatternId id)
{
    if (id == CM_MACRO_PATTERN_NONE) {
        return NULL;
    }
    return (CmMacroPatternNode *)cm_vec_at(&definition->nodes,
        (size_t)id - 1);
}

const CmMacroPatternNode *cm_macro_rules_pattern(
    const CmMacroRulesDefinition *definition, CmMacroPatternId id)
{
    if (definition == NULL || id == CM_MACRO_PATTERN_NONE) {
        return NULL;
    }
    return (const CmMacroPatternNode *)cm_vec_at_const(&definition->nodes,
        (size_t)id - 1);
}

const CmMacroBinding *cm_macro_rules_binding(
    const CmMacroRulesDefinition *definition, CmMacroBindingId id)
{
    if (definition == NULL || id == CM_MACRO_BINDING_NONE) {
        return NULL;
    }
    return (const CmMacroBinding *)cm_vec_at_const(&definition->bindings,
        (size_t)id - 1);
}

const CmMacroRuleArm *cm_macro_rules_arm(
    const CmMacroRulesDefinition *definition, size_t index)
{
    if (definition == NULL) {
        return NULL;
    }
    return (const CmMacroRuleArm *)cm_vec_at_const(&definition->arms, index);
}

static CmMacroPatternId cm_rules_new_pattern(
    CmMacroRulesDefinition *definition, CmMacroPatternKind kind)
{
    CmMacroPatternNode *node;
    size_t next_id;

    if (!cm_size_add(definition->nodes.len, 1, &next_id)
        || next_id > (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    node = (CmMacroPatternNode *)cm_vec_push_uninit(&definition->nodes);
    memset(node, 0, sizeof(*node));
    node->id = (CmMacroPatternId)next_id;
    node->kind = kind;
    return node->id;
}

static CmMacroPatternId cm_rules_new_token_pattern(
    CmMacroRulesDefinition *definition, const struct cm_tt_node *token)
{
    CmMacroPatternId id;
    CmMacroPatternNode *pattern;

    id = cm_rules_new_pattern(definition, CM_MACRO_PATTERN_TOKEN);
    pattern = cm_rules_pattern_mut(definition, id);
    pattern->data.token.kind = token->data.token.kind;
    pattern->data.token.source_start = token->data.token.start;
    pattern->data.token.source_length = token->data.token.length;
    return id;
}

static void cm_rules_link_pattern(CmMacroRulesDefinition *definition,
    CmMacroPatternId *first, CmMacroPatternId *last,
    CmMacroPatternId child)
{
    CmMacroPatternNode *previous;

    if (*last == CM_MACRO_PATTERN_NONE) {
        *first = child;
    } else {
        previous = cm_rules_pattern_mut(definition, *last);
        if (previous == NULL) {
            abort();
        }
        previous->next_sibling = child;
    }
    *last = child;
}

static int cm_rules_fragment_from_node(
    const CmMacroRulesDefinition *definition,
    const struct cm_tt_node *node,
    CmMacroFragmentKind *fragment)
{
    struct FragmentName {
        const char *name;
        CmMacroFragmentKind fragment;
    } names[] = {
        { "ident", CM_MACRO_FRAGMENT_IDENT },
        { "expr", CM_MACRO_FRAGMENT_EXPR },
        { "ty", CM_MACRO_FRAGMENT_TY },
        { "pat", CM_MACRO_FRAGMENT_PAT },
        { "path", CM_MACRO_FRAGMENT_PATH },
        { "tt", CM_MACRO_FRAGMENT_TT },
        { "item", CM_MACRO_FRAGMENT_ITEM },
        { "block", CM_MACRO_FRAGMENT_BLOCK },
        { "literal", CM_MACRO_FRAGMENT_LITERAL },
        { "vis", CM_MACRO_FRAGMENT_VIS },
        { "lifetime", CM_MACRO_FRAGMENT_LIFETIME },
        { "meta", CM_MACRO_FRAGMENT_META }
    };
    size_t index;

    for (index = 0; index < CM_ARRAY_LEN(names); index += 1) {
        if (cm_rules_token_text_equal(definition, node, names[index].name)) {
            *fragment = names[index].fragment;
            return 1;
        }
    }
    return 0;
}

static int cm_rules_binding_name_equal(
    const CmMacroRulesDefinition *definition,
    const CmMacroBinding *binding,
    const struct cm_tt_node *name)
{
    if (binding == NULL || name == NULL
        || name->kind != CM_TT_NODE_TOKEN
        || !cm_rules_source_range_valid(definition,
            binding->name_start, binding->name_length)
        || !cm_rules_source_range_valid(definition,
            name->data.token.start, name->data.token.length)) {
        return 0;
    }
    return binding->name_length == name->data.token.length
        && memcmp(definition->source + binding->name_start,
            definition->source + name->data.token.start,
            binding->name_length) == 0;
}

static CmMacroBindingId cm_rules_find_binding(
    const CmMacroRulesParser *parser, const struct cm_tt_node *name)
{
    size_t index;
    const CmMacroBinding *binding;

    for (index = parser->binding_start;
        index < parser->definition->bindings.len; index += 1) {
        binding = (const CmMacroBinding *)cm_vec_at_const(
            &parser->definition->bindings, index);
        if (cm_rules_binding_name_equal(parser->definition, binding, name)) {
            return binding->id;
        }
    }
    return CM_MACRO_BINDING_NONE;
}

static int cm_rules_nested_binding_name_equal(
    const CmMacroRulesParser *parser,
    const CmNestedMacroBinding *binding,
    const struct cm_tt_node *name)
{
    return binding != NULL && name != NULL
        && name->kind == CM_TT_NODE_TOKEN
        && name->data.token.start >= binding->scope_start
        && name->data.token.start < binding->scope_end
        && binding->name_length == name->data.token.length
        && cm_rules_source_range_valid(parser->definition,
            binding->name_start, binding->name_length)
        && cm_rules_source_range_valid(parser->definition,
            name->data.token.start, name->data.token.length)
        && memcmp(parser->definition->source + binding->name_start,
            parser->definition->source + name->data.token.start,
            binding->name_length) == 0;
}

static int cm_rules_nested_binding_exists(
    const CmMacroRulesParser *parser, const struct cm_tt_node *name)
{
    size_t index;
    const CmNestedMacroBinding *binding;

    for (index = parser->nested_bindings.len; index != 0u; --index) {
        binding = (const CmNestedMacroBinding *)cm_vec_at_const(
            &parser->nested_bindings, index - 1u);
        if (cm_rules_nested_binding_name_equal(parser, binding, name)) {
            return 1;
        }
    }
    return 0;
}

static void cm_rules_collect_nested_matcher_bindings(
    CmMacroRulesParser *parser, cm_tt_id input, unsigned int nesting,
    size_t scope_start, size_t scope_end)
{
    const struct cm_tt_node *node;
    const struct cm_tt_node *name;
    const struct cm_tt_node *colon;
    CmNestedMacroBinding binding;

    if (nesting > parser->definition->limits.max_nesting) return;
    node = cm_token_tree_node(parser->definition->tree, input);
    while (node != NULL) {
        if (node->kind == CM_TT_NODE_GROUP) {
            cm_rules_collect_nested_matcher_bindings(parser,
                node->first_child, nesting + 1u,
                scope_start, scope_end);
        } else if (node->kind == CM_TT_NODE_TOKEN
            && node->data.token.kind == CM_TOKEN_DOLLAR) {
            name = cm_token_tree_node(parser->definition->tree,
                node->next_sibling);
            colon = name == NULL ? NULL
                : cm_token_tree_node(parser->definition->tree,
                    name->next_sibling);
            if (name != NULL && name->kind == CM_TT_NODE_TOKEN
                && (name->data.token.kind == CM_TOKEN_IDENT
                    || name->data.token.kind == CM_TOKEN_RAW_IDENT)
                && colon != NULL && colon->kind == CM_TT_NODE_TOKEN
                && colon->data.token.kind == CM_TOKEN_COLON
                && !cm_rules_nested_binding_exists(parser, name)) {
                binding.name_start = name->data.token.start;
                binding.name_length = name->data.token.length;
                binding.scope_start = scope_start;
                binding.scope_end = scope_end;
                (void)cm_vec_push(&parser->nested_bindings, &binding);
            }
        }
        node = cm_token_tree_node(parser->definition->tree,
            node->next_sibling);
    }
}

static void cm_rules_collect_nested_bindings(CmMacroRulesParser *parser,
    cm_tt_id input)
{
    const struct cm_tt_node *matcher;
    const struct cm_tt_node *arrow;
    const struct cm_tt_node *transcriber;
    size_t scope_end;

    matcher = cm_token_tree_node(parser->definition->tree, input);
    while (matcher != NULL) {
        if (matcher->kind == CM_TT_NODE_TOKEN
            && (matcher->data.token.kind == CM_TOKEN_SEMICOLON
                || matcher->data.token.kind == CM_TOKEN_COMMA)) {
            matcher = cm_token_tree_node(parser->definition->tree,
                matcher->next_sibling);
            continue;
        }
        if (matcher->kind != CM_TT_NODE_GROUP) break;
        arrow = cm_token_tree_node(parser->definition->tree,
            matcher->next_sibling);
        transcriber = arrow == NULL ? NULL
            : cm_token_tree_node(parser->definition->tree,
                arrow->next_sibling);
        if (arrow == NULL || arrow->kind != CM_TT_NODE_TOKEN
            || arrow->data.token.kind != CM_TOKEN_FAT_ARROW
            || transcriber == NULL
            || transcriber->kind != CM_TT_NODE_GROUP) {
            break;
        }
        scope_end = transcriber->data.group.close_span.start
            + transcriber->data.group.close_span.length;
        cm_rules_collect_nested_matcher_bindings(parser,
            matcher->first_child, 0u,
            matcher->data.group.open_span.start, scope_end);
        matcher = cm_token_tree_node(parser->definition->tree,
            transcriber->next_sibling);
    }
}

static int cm_rules_nested_macro_parts(CmMacroRulesParser *parser,
    const struct cm_tt_node *node, const struct cm_tt_node **bang,
    const struct cm_tt_node **name, const struct cm_tt_node **body)
{
    if (node == NULL || node->kind != CM_TT_NODE_TOKEN
        || !cm_rules_token_text_equal(parser->definition,
            node, "macro_rules")) {
        return 0;
    }
    *bang = cm_token_tree_node(parser->definition->tree,
        node->next_sibling);
    *name = *bang == NULL ? NULL
        : cm_token_tree_node(parser->definition->tree,
            (*bang)->next_sibling);
    *body = *name == NULL ? NULL
        : cm_token_tree_node(parser->definition->tree,
            (*name)->next_sibling);
    return *bang != NULL && (*bang)->kind == CM_TT_NODE_TOKEN
        && (*bang)->data.token.kind == CM_TOKEN_BANG
        && *name != NULL && (*name)->kind == CM_TT_NODE_TOKEN
        && ((*name)->data.token.kind == CM_TOKEN_IDENT
            || (*name)->data.token.kind == CM_TOKEN_RAW_IDENT)
        && *body != NULL && (*body)->kind == CM_TT_NODE_GROUP;
}

static CmMacroBindingId cm_rules_add_binding(CmMacroRulesParser *parser,
    const struct cm_tt_node *name, CmMacroFragmentKind fragment,
    unsigned int repetition_depth)
{
    CmMacroBinding *binding;
    size_t next_id;

    if (cm_rules_find_binding(parser, name) != CM_MACRO_BINDING_NONE) {
        cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_DUPLICATE_BINDING,
            cm_rules_node_offset(name),
            "duplicate metavariable binding in macro matcher");
        return CM_MACRO_BINDING_NONE;
    }
    if (!cm_size_add(parser->definition->bindings.len, 1, &next_id)
        || next_id > (size_t)UINT32_MAX) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    binding = (CmMacroBinding *)cm_vec_push_uninit(
        &parser->definition->bindings);
    binding->id = (CmMacroBindingId)next_id;
    binding->name_start = name->data.token.start;
    binding->name_length = name->data.token.length;
    binding->fragment = fragment;
    binding->repetition_depth = repetition_depth;
    return binding->id;
}

static int cm_rules_repetition_operator(const struct cm_tt_node *node,
    CmMacroRepetitionOperator *operator_kind)
{
    if (node == NULL || node->kind != CM_TT_NODE_TOKEN) {
        return 0;
    }
    if (node->data.token.kind == CM_TOKEN_STAR) {
        *operator_kind = CM_MACRO_REPETITION_ZERO_OR_MORE;
        return 1;
    }
    if (node->data.token.kind == CM_TOKEN_PLUS) {
        *operator_kind = CM_MACRO_REPETITION_ONE_OR_MORE;
        return 1;
    }
    if (node->data.token.kind == CM_TOKEN_QUESTION) {
        *operator_kind = CM_MACRO_REPETITION_ZERO_OR_ONE;
        return 1;
    }
    return 0;
}

static int cm_rules_parse_sequence(CmMacroRulesParser *parser,
    cm_tt_id input, CmMacroPatternMode mode,
    unsigned int nesting, unsigned int repetition_depth,
    CmMacroPatternId *first, CmMacroPatternId *last);

static int cm_rules_parse_group_pattern(CmMacroRulesParser *parser,
    const struct cm_tt_node *group, CmMacroPatternMode mode,
    unsigned int nesting, unsigned int repetition_depth,
    CmMacroPatternId *out_id)
{
    CmMacroPatternId id;
    CmMacroPatternId child_first;
    CmMacroPatternId child_last;
    CmMacroPatternNode *pattern;

    if (nesting >= parser->definition->limits.max_nesting) {
        cm_rules_parse_error(parser, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_NESTING_LIMIT,
            cm_rules_node_offset(group),
            "macro_rules pattern nesting limit exceeded");
        return 0;
    }
    id = cm_rules_new_pattern(parser->definition, CM_MACRO_PATTERN_GROUP);
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->data.group.delimiter = group->data.group.delimiter;
    pattern->data.group.source_start = group->data.group.open_span.start;
    child_first = CM_MACRO_PATTERN_NONE;
    child_last = CM_MACRO_PATTERN_NONE;
    if (!cm_rules_parse_sequence(parser, group->first_child, mode,
        nesting + 1u, repetition_depth, &child_first, &child_last)) {
        return 0;
    }
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->first_child = child_first;
    pattern->last_child = child_last;
    *out_id = id;
    return 1;
}

static int cm_rules_parse_repetition(CmMacroRulesParser *parser,
    const struct cm_tt_node *group, CmMacroPatternMode mode,
    unsigned int nesting, unsigned int repetition_depth,
    const struct cm_tt_node **after, CmMacroPatternId *out_id)
{
    const struct cm_tt_node *candidate;
    const struct cm_tt_node *operator_node;
    const struct cm_tt_node *separator;
    CmMacroRepetitionOperator operator_kind;
    CmMacroPatternId id;
    CmMacroPatternId child_first;
    CmMacroPatternId child_last;
    CmMacroPatternNode *pattern;

    if (nesting >= parser->definition->limits.max_nesting
        || repetition_depth >= parser->definition->limits.max_nesting
        || repetition_depth >= CM_MACRO_RULES_ABSOLUTE_MAX_NESTING) {
        cm_rules_parse_error(parser, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_NESTING_LIMIT,
            cm_rules_node_offset(group),
            "macro_rules repetition nesting limit exceeded");
        return 0;
    }
    candidate = cm_token_tree_node(parser->definition->tree,
        group->next_sibling);
    separator = NULL;
    operator_node = candidate;
    if (!cm_rules_repetition_operator(operator_node, &operator_kind)) {
        separator = candidate;
        if (separator == NULL || separator->kind != CM_TT_NODE_TOKEN) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_REPEAT_OPERATOR,
                cm_rules_node_offset(candidate),
                "expected a repetition operator after $(...)");
            return 0;
        }
        operator_node = cm_token_tree_node(parser->definition->tree,
            separator->next_sibling);
        if (!cm_rules_repetition_operator(operator_node, &operator_kind)) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_REPEAT_OPERATOR,
                cm_rules_node_offset(operator_node),
                "expected '*', '+', or '?' after repetition separator");
            return 0;
        }
    }
    if (operator_kind == CM_MACRO_REPETITION_ZERO_OR_ONE
        && separator != NULL) {
        cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_INVALID_SEPARATOR,
            cm_rules_node_offset(separator),
            "the '?' repetition operator cannot have a separator");
        return 0;
    }

    id = cm_rules_new_pattern(parser->definition,
        CM_MACRO_PATTERN_REPETITION);
    child_first = CM_MACRO_PATTERN_NONE;
    child_last = CM_MACRO_PATTERN_NONE;
    if (!cm_rules_parse_sequence(parser, group->first_child, mode,
        nesting + 1u, repetition_depth + 1u,
        &child_first, &child_last)) {
        return 0;
    }
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->first_child = child_first;
    pattern->last_child = child_last;
    pattern->data.repetition.operator_kind = operator_kind;
    pattern->data.repetition.source_start = group->data.group.open_span.start;
    pattern->data.repetition.has_separator = separator != NULL;
    if (separator != NULL) {
        pattern->data.repetition.separator.kind = separator->data.token.kind;
        pattern->data.repetition.separator.source_start =
            separator->data.token.start;
        pattern->data.repetition.separator.source_length =
            separator->data.token.length;
    }
    *after = cm_token_tree_node(parser->definition->tree,
        operator_node->next_sibling);
    *out_id = id;
    return 1;
}

static int cm_rules_parse_metavariable(CmMacroRulesParser *parser,
    const struct cm_tt_node *name, CmMacroPatternMode mode,
    unsigned int repetition_depth, const struct cm_tt_node **after,
    CmMacroPatternId *out_id)
{
    const struct cm_tt_node *colon;
    const struct cm_tt_node *fragment_node;
    CmMacroFragmentKind fragment;
    CmMacroBindingId binding;
    CmMacroPatternId id;
    CmMacroPatternNode *pattern;

    if (name == NULL || name->kind != CM_TT_NODE_TOKEN
        || (name->data.token.kind != CM_TOKEN_IDENT
            && name->data.token.kind != CM_TOKEN_RAW_IDENT)) {
        cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_EXPECTED_FRAGMENT,
            cm_rules_node_offset(name),
            "expected a metavariable name after '$'");
        return 0;
    }
    if (mode == CM_MACRO_PATTERN_MATCHER) {
        colon = cm_token_tree_node(parser->definition->tree,
            name->next_sibling);
        if (colon == NULL || colon->kind != CM_TT_NODE_TOKEN
            || colon->data.token.kind != CM_TOKEN_COLON) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_FRAGMENT,
                cm_rules_node_offset(colon),
                "expected ':' and a fragment kind after metavariable name");
            return 0;
        }
        fragment_node = cm_token_tree_node(parser->definition->tree,
            colon->next_sibling);
        if (fragment_node == NULL || fragment_node->kind != CM_TT_NODE_TOKEN) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_FRAGMENT,
                cm_rules_node_offset(fragment_node),
                "expected a macro fragment kind");
            return 0;
        }
        if (!cm_rules_fragment_from_node(parser->definition,
            fragment_node, &fragment)) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_UNKNOWN_FRAGMENT,
                cm_rules_node_offset(fragment_node),
                "unknown macro fragment kind");
            return 0;
        }
        binding = cm_rules_add_binding(parser, name, fragment,
            repetition_depth);
        if (binding == CM_MACRO_BINDING_NONE) {
            return 0;
        }
        *after = cm_token_tree_node(parser->definition->tree,
            fragment_node->next_sibling);
    } else {
        binding = cm_rules_find_binding(parser, name);
        if (binding == CM_MACRO_BINDING_NONE) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_UNKNOWN_BINDING,
                cm_rules_node_offset(name),
                "unknown metavariable in macro transcriber");
            return 0;
        }
        fragment = cm_macro_rules_binding(parser->definition,
            binding)->fragment;
        *after = cm_token_tree_node(parser->definition->tree,
            name->next_sibling);
    }
    id = cm_rules_new_pattern(parser->definition,
        CM_MACRO_PATTERN_METAVARIABLE);
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->data.metavariable.binding = binding;
    pattern->data.metavariable.fragment = fragment;
    *out_id = id;
    return 1;
}

static int cm_rules_is_named_metavariable_expression(
    CmMacroRulesParser *parser, const struct cm_tt_node *group,
    const char *expected)
{
    const struct cm_tt_node *operation;
    const struct cm_tt_node *arguments;

    if (group == NULL || group->kind != CM_TT_NODE_GROUP
        || group->data.group.delimiter != CM_TT_DELIMITER_BRACE) {
        return 0;
    }
    operation = cm_token_tree_node(parser->definition->tree,
        group->first_child);
    arguments = operation == NULL ? NULL : cm_token_tree_node(
        parser->definition->tree, operation->next_sibling);
    return cm_rules_token_text_equal(parser->definition,
            operation, expected)
        && arguments != NULL && arguments->kind == CM_TT_NODE_GROUP
        && arguments->data.group.delimiter == CM_TT_DELIMITER_PAREN
        && arguments->next_sibling == CM_TT_ID_NONE;
}

static int cm_rules_is_concat_expression(CmMacroRulesParser *parser,
    const struct cm_tt_node *group)
{
    return cm_rules_is_named_metavariable_expression(parser, group,
        "concat");
}

static int cm_rules_parse_concat_expression(CmMacroRulesParser *parser,
    const struct cm_tt_node *group, unsigned int nesting,
    unsigned int repetition_depth, CmMacroPatternId *out_id)
{
    const struct cm_tt_node *operation;
    const struct cm_tt_node *arguments;
    const struct cm_tt_node *part;
    const struct cm_tt_node *name;
    const struct cm_tt_node *after;
    CmMacroPatternId id;
    CmMacroPatternId child;
    CmMacroPatternId child_first;
    CmMacroPatternId child_last;
    const CmMacroBinding *binding;
    CmMacroPatternNode *pattern;
    size_t part_count;
    int expect_part;

    if (nesting >= parser->definition->limits.max_nesting) {
        cm_rules_parse_error(parser, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_NESTING_LIMIT,
            cm_rules_node_offset(group),
            "macro concat expression nesting limit exceeded");
        return 0;
    }
    operation = cm_token_tree_node(parser->definition->tree,
        group->first_child);
    arguments = cm_token_tree_node(parser->definition->tree,
        operation->next_sibling);
    id = cm_rules_new_pattern(parser->definition, CM_MACRO_PATTERN_CONCAT);
    child_first = CM_MACRO_PATTERN_NONE;
    child_last = CM_MACRO_PATTERN_NONE;
    part_count = 0u;
    expect_part = 1;
    part = cm_token_tree_node(parser->definition->tree,
        arguments->first_child);
    while (part != NULL) {
        if (!expect_part) {
            if (part->kind != CM_TT_NODE_TOKEN
                || part->data.token.kind != CM_TOKEN_COMMA) {
                cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                    CM_MACRO_DIAG_RULES_INVALID_TREE,
                    cm_rules_node_offset(part),
                    "expected ',' between concat identifier parts");
                return 0;
            }
            expect_part = 1;
            part = cm_token_tree_node(parser->definition->tree,
                part->next_sibling);
            continue;
        }
        if (part->kind == CM_TT_NODE_TOKEN
            && (part->data.token.kind == CM_TOKEN_IDENT
                || part->data.token.kind == CM_TOKEN_RAW_IDENT)) {
            child = cm_rules_new_token_pattern(parser->definition, part);
            after = cm_token_tree_node(parser->definition->tree,
                part->next_sibling);
        } else if (part->kind == CM_TT_NODE_TOKEN
            && part->data.token.kind == CM_TOKEN_DOLLAR) {
            name = cm_token_tree_node(parser->definition->tree,
                part->next_sibling);
            if (!cm_rules_parse_metavariable(parser, name,
                    CM_MACRO_PATTERN_TRANSCRIBER, repetition_depth,
                    &after, &child)) {
                return 0;
            }
            pattern = cm_rules_pattern_mut(parser->definition, child);
            binding = pattern == NULL ? NULL : cm_macro_rules_binding(
                parser->definition, pattern->data.metavariable.binding);
            if (binding == NULL
                || binding->fragment != CM_MACRO_FRAGMENT_IDENT) {
                cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                    CM_MACRO_DIAG_RULES_INVALID_TREE,
                    cm_rules_node_offset(name),
                    "concat supports only ident metavariable captures");
                return 0;
            }
        } else {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_INVALID_TREE,
                cm_rules_node_offset(part),
                "concat supports only identifier literals and captures");
            return 0;
        }
        cm_rules_link_pattern(parser->definition,
            &child_first, &child_last, child);
        part_count += 1u;
        expect_part = 0;
        part = after;
    }
    if (expect_part || part_count < 2u) {
        cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_INVALID_TREE,
            cm_rules_node_offset(arguments),
            "concat requires at least two identifier parts");
        return 0;
    }
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->first_child = child_first;
    pattern->last_child = child_last;
    pattern->data.concat.source_start = group->data.group.open_span.start;
    *out_id = id;
    return 1;
}

static int cm_rules_parse_simple_metavariable_expression(
    CmMacroRulesParser *parser, const struct cm_tt_node *group,
    CmMacroPatternKind kind, unsigned int repetition_depth,
    CmMacroPatternId *out_id)
{
    const struct cm_tt_node *operation;
    const struct cm_tt_node *arguments;
    const struct cm_tt_node *dollar;
    const struct cm_tt_node *name;
    const struct cm_tt_node *after;
    CmMacroPatternId id;
    CmMacroPatternId child;
    CmMacroPatternNode *pattern;

    operation = cm_token_tree_node(parser->definition->tree,
        group->first_child);
    arguments = cm_token_tree_node(parser->definition->tree,
        operation->next_sibling);
    dollar = cm_token_tree_node(parser->definition->tree,
        arguments->first_child);
    child = CM_MACRO_PATTERN_NONE;
    if (kind == CM_MACRO_PATTERN_INDEX) {
        if (dollar != NULL) {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_INVALID_TREE,
                cm_rules_node_offset(dollar),
                "index takes no metavariable argument");
            return 0;
        }
    } else {
        name = dollar == NULL ? NULL : cm_token_tree_node(
            parser->definition->tree, dollar->next_sibling);
        if (dollar == NULL || dollar->kind != CM_TT_NODE_TOKEN
            || dollar->data.token.kind != CM_TOKEN_DOLLAR
            || name == NULL
            || !cm_rules_parse_metavariable(parser, name,
                CM_MACRO_PATTERN_TRANSCRIBER, repetition_depth,
                &after, &child)
            || after != NULL) {
            if (parser->status == CM_MACRO_OK) {
                cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                    CM_MACRO_DIAG_RULES_INVALID_TREE,
                    cm_rules_node_offset(arguments),
                    "metavariable expression expects one capture");
            }
            return 0;
        }
    }
    id = cm_rules_new_pattern(parser->definition, kind);
    pattern = cm_rules_pattern_mut(parser->definition, id);
    pattern->first_child = child;
    pattern->last_child = child;
    pattern->data.metavariable_expression.source_start =
        group->data.group.open_span.start;
    *out_id = id;
    return 1;
}

static int cm_rules_parse_sequence(CmMacroRulesParser *parser,
    cm_tt_id input, CmMacroPatternMode mode,
    unsigned int nesting, unsigned int repetition_depth,
    CmMacroPatternId *first, CmMacroPatternId *last)
{
    const struct cm_tt_node *node;
    const struct cm_tt_node *next;
    const struct cm_tt_node *after;
    const struct cm_tt_node *nested_bang;
    const struct cm_tt_node *nested_name;
    const struct cm_tt_node *nested_body;
    CmMacroPatternId id;
    size_t nested_binding_snapshot;
    int parsed_nested;

    *first = CM_MACRO_PATTERN_NONE;
    *last = CM_MACRO_PATTERN_NONE;
    node = cm_token_tree_node(parser->definition->tree, input);
    while (node != NULL) {
        next = cm_token_tree_node(parser->definition->tree,
            node->next_sibling);
        if (mode == CM_MACRO_PATTERN_TRANSCRIBER
            && cm_rules_nested_macro_parts(parser, node,
                &nested_bang, &nested_name, &nested_body)) {
            id = cm_rules_new_token_pattern(parser->definition, node);
            cm_rules_link_pattern(parser->definition, first, last, id);
            id = cm_rules_new_token_pattern(parser->definition,
                nested_bang);
            cm_rules_link_pattern(parser->definition, first, last, id);
            id = cm_rules_new_token_pattern(parser->definition,
                nested_name);
            cm_rules_link_pattern(parser->definition, first, last, id);
            nested_binding_snapshot = parser->nested_bindings.len;
            cm_rules_collect_nested_bindings(parser,
                nested_body->first_child);
            parser->nested_macro_depth += 1u;
            parsed_nested = cm_rules_parse_group_pattern(parser,
                nested_body, mode, nesting, repetition_depth, &id);
            parser->nested_macro_depth -= 1u;
            parser->nested_bindings.len = nested_binding_snapshot;
            if (!parsed_nested) return 0;
            cm_rules_link_pattern(parser->definition, first, last, id);
            node = cm_token_tree_node(parser->definition->tree,
                nested_body->next_sibling);
            continue;
        }
        if (node->kind == CM_TT_NODE_TOKEN
            && node->data.token.kind == CM_TOKEN_DOLLAR) {
            if (parser->nested_macro_depth != 0u
                && (next == NULL || next->kind == CM_TT_NODE_GROUP
                    || cm_rules_nested_binding_exists(parser, next)
                    || cm_rules_find_binding(parser, next)
                        == CM_MACRO_BINDING_NONE)) {
                id = cm_rules_new_token_pattern(parser->definition, node);
                cm_rules_link_pattern(parser->definition,
                    first, last, id);
                node = next;
                continue;
            }
            if (mode == CM_MACRO_PATTERN_TRANSCRIBER
                && (cm_rules_is_concat_expression(parser, next)
                    || cm_rules_is_named_metavariable_expression(
                        parser, next, "ignore")
                    || cm_rules_is_named_metavariable_expression(
                        parser, next, "index")
                    || cm_rules_is_named_metavariable_expression(
                        parser, next, "count"))) {
                if (cm_rules_is_concat_expression(parser, next)) {
                    if (!cm_rules_parse_concat_expression(parser, next,
                        nesting, repetition_depth, &id)) {
                        return 0;
                    }
                } else if (!cm_rules_parse_simple_metavariable_expression(
                    parser, next,
                    cm_rules_is_named_metavariable_expression(
                            parser, next, "ignore")
                        ? CM_MACRO_PATTERN_IGNORE
                        : (cm_rules_is_named_metavariable_expression(
                                parser, next, "index")
                            ? CM_MACRO_PATTERN_INDEX
                            : CM_MACRO_PATTERN_COUNT),
                    repetition_depth, &id)) {
                    return 0;
                }
                after = cm_token_tree_node(parser->definition->tree,
                    next->next_sibling);
            } else if (next != NULL && next->kind == CM_TT_NODE_GROUP) {
                if (!cm_rules_parse_repetition(parser, next, mode,
                    nesting, repetition_depth, &after, &id)) {
                    return 0;
                }
            } else if (mode == CM_MACRO_PATTERN_TRANSCRIBER
                && cm_rules_token_text_equal(parser->definition,
                    next, "crate")) {
                CmMacroPatternNode *crate_pattern;

                id = cm_rules_new_pattern(parser->definition,
                    CM_MACRO_PATTERN_CRATE);
                crate_pattern = cm_rules_pattern_mut(parser->definition, id);
                crate_pattern->data.metavariable_expression.source_start =
                    cm_rules_node_offset(node);
                after = cm_token_tree_node(parser->definition->tree,
                    next->next_sibling);
            } else if (mode == CM_MACRO_PATTERN_TRANSCRIBER
                && next != NULL && next->kind == CM_TT_NODE_TOKEN
                && (next->data.token.kind == CM_TOKEN_IDENT
                    || next->data.token.kind == CM_TOKEN_RAW_IDENT)
                && cm_rules_find_binding(parser, next)
                    == CM_MACRO_BINDING_NONE) {
                /* `$t` of a nested `macro_rules! { ($t:tt,) => .. }`
                 * written inside the outer transcriber (std_detect's
                 * `features!`): an unbound metavariable passes through
                 * as the tokens `$` `t`. */
                id = cm_rules_new_token_pattern(parser->definition, node);
                cm_rules_link_pattern(parser->definition, first, last, id);
                id = cm_rules_new_token_pattern(parser->definition, next);
                after = cm_token_tree_node(parser->definition->tree,
                    next->next_sibling);
            } else {
                if (!cm_rules_parse_metavariable(parser, next, mode,
                    repetition_depth, &after, &id)) {
                    return 0;
                }
            }
            cm_rules_link_pattern(parser->definition, first, last, id);
            node = after;
            continue;
        }
        if (node->kind == CM_TT_NODE_GROUP) {
            if (!cm_rules_parse_group_pattern(parser, node, mode,
                nesting, repetition_depth, &id)) {
                return 0;
            }
        } else if (node->kind == CM_TT_NODE_TOKEN) {
            id = cm_rules_new_token_pattern(parser->definition, node);
        } else {
            cm_rules_parse_error(parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_INVALID_TREE,
                cm_rules_node_offset(node),
                "unexpected root node inside macro rule");
            return 0;
        }
        cm_rules_link_pattern(parser->definition, first, last, id);
        node = next;
    }
    return 1;
}

void cm_macro_rules_limits_init(CmMacroRulesLimits *limits)
{
    if (limits == NULL) {
        return;
    }
    limits->max_nesting = CM_MACRO_RULES_DEFAULT_MAX_NESTING;
    limits->max_backtrack_steps =
        CM_MACRO_RULES_DEFAULT_BACKTRACK_STEPS;
    limits->max_repetition_iterations =
        CM_MACRO_RULES_DEFAULT_REPETITION_ITERATIONS;
}

const char *cm_macro_fragment_kind_name(CmMacroFragmentKind kind)
{
    switch (kind) {
    case CM_MACRO_FRAGMENT_IDENT:
        return "ident";
    case CM_MACRO_FRAGMENT_EXPR:
        return "expr";
    case CM_MACRO_FRAGMENT_TY:
        return "ty";
    case CM_MACRO_FRAGMENT_PAT:
        return "pat";
    case CM_MACRO_FRAGMENT_PATH:
        return "path";
    case CM_MACRO_FRAGMENT_TT:
        return "tt";
    case CM_MACRO_FRAGMENT_ITEM:
        return "item";
    case CM_MACRO_FRAGMENT_BLOCK:
        return "block";
    case CM_MACRO_FRAGMENT_LITERAL:
        return "literal";
    case CM_MACRO_FRAGMENT_VIS:
        return "vis";
    case CM_MACRO_FRAGMENT_LIFETIME:
        return "lifetime";
    case CM_MACRO_FRAGMENT_META:
        return "meta";
    }
    return "unknown";
}

void cm_macro_rules_definition_init(CmMacroRulesDefinition *definition)
{
    if (definition == NULL) {
        return;
    }
    definition->tree = NULL;
    definition->source = NULL;
    definition->source_length = 0;
    definition->body = CM_TT_ID_NONE;
    cm_macro_rules_limits_init(&definition->limits);
    cm_vec_init(&definition->nodes, sizeof(CmMacroPatternNode));
    cm_vec_init(&definition->bindings, sizeof(CmMacroBinding));
    cm_vec_init(&definition->arms, sizeof(CmMacroRuleArm));
}

void cm_macro_rules_definition_destroy(CmMacroRulesDefinition *definition)
{
    if (definition == NULL) {
        return;
    }
    cm_vec_destroy(&definition->arms);
    cm_vec_destroy(&definition->bindings);
    cm_vec_destroy(&definition->nodes);
    definition->tree = NULL;
    definition->source = NULL;
    definition->source_length = 0;
    definition->body = CM_TT_ID_NONE;
    cm_macro_rules_limits_init(&definition->limits);
}

static CmMacroRulesParseResult cm_rules_parse_invalid(void)
{
    CmMacroRulesParseResult result;

    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.arm_count = 0;
    result.binding_count = 0;
    result.diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    result.diagnostic.offset = 0;
    result.diagnostic.message = "invalid macro_rules parser argument";
    return result;
}

CmMacroRulesParseResult cm_macro_rules_parse(
    CmMacroRulesDefinition *definition,
    const struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    cm_tt_id body,
    const CmMacroRulesLimits *limits)
{
    CmMacroRulesParseResult result;
    CmMacroRulesParser parser;
    const struct cm_tt_node *body_node;
    const struct cm_tt_node *cursor;
    const struct cm_tt_node *arrow;
    const struct cm_tt_node *transcriber;
    const struct cm_tt_node *separator;
    CmMacroRuleArm *arm;
    CmMacroPatternId matcher_first;
    CmMacroPatternId matcher_last;
    CmMacroPatternId transcriber_first;
    CmMacroPatternId transcriber_last;
    size_t arm_binding_start;

    if (definition == NULL || tree == NULL
        || (source == NULL && source_length != 0)
        || definition->nodes.len != 0 || definition->bindings.len != 0
        || definition->arms.len != 0) {
        return cm_rules_parse_invalid();
    }
    if (tree->errors.len != 0) {
        const struct cm_tt_error *tree_error;

        tree_error = cm_token_tree_error(tree, 0);
        result.status = CM_MACRO_SYNTAX_ERROR;
        result.arm_count = 0;
        result.binding_count = 0;
        result.diagnostic.code = CM_MACRO_DIAG_RULES_INVALID_TREE;
        result.diagnostic.offset = tree_error == NULL
            ? 0 : tree_error->span.start;
        result.diagnostic.message =
            "macro_rules definition has invalid token-tree delimiters";
        return result;
    }
    definition->limits = limits == NULL ? definition->limits : *limits;
    if (definition->limits.max_nesting == 0
        || definition->limits.max_nesting
            > CM_MACRO_RULES_ABSOLUTE_MAX_NESTING
        || definition->limits.max_backtrack_steps == 0
        || definition->limits.max_repetition_iterations == 0) {
        return cm_rules_parse_invalid();
    }
    body_node = cm_token_tree_node(tree, body);
    if (body_node == NULL || (body_node->kind != CM_TT_NODE_ROOT
        && body_node->kind != CM_TT_NODE_GROUP)) {
        return cm_rules_parse_invalid();
    }
    definition->tree = tree;
    definition->source = source == NULL ? "" : source;
    definition->source_length = source_length;
    definition->body = body;
    parser.definition = definition;
    parser.binding_start = 0;
    cm_vec_init(&parser.nested_bindings, sizeof(CmNestedMacroBinding));
    parser.nested_macro_depth = 0u;
    parser.status = CM_MACRO_OK;
    parser.diagnostic.code = CM_MACRO_DIAG_NONE;
    parser.diagnostic.offset = 0;
    parser.diagnostic.message = "";
    cursor = cm_token_tree_node(tree, body_node->first_child);
    while (cursor != NULL) {
        if (cursor->kind == CM_TT_NODE_TOKEN
            && (cursor->data.token.kind == CM_TOKEN_SEMICOLON
                || cursor->data.token.kind == CM_TOKEN_COMMA)) {
            cursor = cm_token_tree_node(tree, cursor->next_sibling);
            continue;
        }
        if (cursor->kind != CM_TT_NODE_GROUP) {
            cm_rules_parse_error(&parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_MATCHER,
                cm_rules_node_offset(cursor),
                "expected a delimited macro rule matcher");
            break;
        }
        arrow = cm_token_tree_node(tree, cursor->next_sibling);
        if (arrow == NULL || arrow->kind != CM_TT_NODE_TOKEN
            || arrow->data.token.kind != CM_TOKEN_FAT_ARROW) {
            cm_rules_parse_error(&parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_ARROW,
                cm_rules_node_offset(arrow),
                "expected '=>' after macro rule matcher");
            break;
        }
        transcriber = cm_token_tree_node(tree, arrow->next_sibling);
        if (transcriber == NULL || transcriber->kind != CM_TT_NODE_GROUP) {
            cm_rules_parse_error(&parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_TRANSCRIBER,
                cm_rules_node_offset(transcriber),
                "expected a delimited macro rule transcriber");
            break;
        }
        arm_binding_start = definition->bindings.len;
        parser.binding_start = arm_binding_start;
        if (!cm_rules_parse_sequence(&parser, cursor->first_child,
            CM_MACRO_PATTERN_MATCHER, 0u, 0u,
            &matcher_first, &matcher_last)) {
            break;
        }
        if (!cm_rules_parse_sequence(&parser, transcriber->first_child,
            CM_MACRO_PATTERN_TRANSCRIBER, 0u, 0u,
            &transcriber_first, &transcriber_last)) {
            break;
        }
        arm = (CmMacroRuleArm *)cm_vec_push_uninit(&definition->arms);
        arm->index = definition->arms.len - 1;
        arm->matcher_first = matcher_first;
        arm->transcriber_first = transcriber_first;
        arm->binding_count = definition->bindings.len - arm_binding_start;
        arm->first_binding = arm->binding_count == 0
            ? CM_MACRO_BINDING_NONE
            : (CmMacroBindingId)(arm_binding_start + 1);

        separator = cm_token_tree_node(tree, transcriber->next_sibling);
        if (separator == NULL) {
            cursor = NULL;
        } else if (separator->kind == CM_TT_NODE_TOKEN
            && (separator->data.token.kind == CM_TOKEN_SEMICOLON
                || separator->data.token.kind == CM_TOKEN_COMMA)) {
            cursor = cm_token_tree_node(tree, separator->next_sibling);
        } else {
            cm_rules_parse_error(&parser, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_EXPECTED_MATCHER,
                cm_rules_node_offset(separator),
                "expected ';' or ',' between macro rule arms");
            break;
        }
    }
    if (parser.status == CM_MACRO_OK && definition->arms.len == 0) {
        cm_rules_parse_error(&parser, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_EXPECTED_MATCHER,
            cm_rules_node_offset(body_node),
            "macro_rules definition must contain at least one rule arm");
    }
    result.status = parser.status;
    result.arm_count = definition->arms.len;
    result.binding_count = definition->bindings.len;
    result.diagnostic = parser.diagnostic;
    cm_vec_destroy(&parser.nested_bindings);
    return result;
}
