#include "cm/macro_rules.h"

#include "cm/syntax/parser.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmRulesMatchState CmRulesMatchState;

typedef int (*CmRulesContinuation)(CmRulesMatchState *state,
    cm_tt_id input, cm_tt_id parent, void *context);

struct CmRulesMatchState {
    const CmMacroRulesDefinition *definition;
    const struct cm_token_tree *input_tree;
    const char *input_source;
    size_t input_source_length;
    CmMacroCaptureSet *captures;
    size_t backtrack_steps;
    size_t repetition_indices[CM_MACRO_RULES_ABSOLUTE_MAX_NESTING];
    CmMacroStatus status;
    CmMacroDiagnostic diagnostic;
};

typedef struct CmRulesGroupContinuation {
    CmMacroPatternId outer_next;
    cm_tt_id outer_input;
    cm_tt_id outer_parent;
    unsigned int nesting;
    unsigned int repetition_depth;
    CmRulesContinuation continuation;
    void *continuation_context;
} CmRulesGroupContinuation;

typedef struct CmRulesRepeatContinuation {
    const CmMacroPatternNode *repetition;
    CmMacroPatternId outer_next;
    cm_tt_id body_input;
    cm_tt_id parent;
    size_t iteration;
    unsigned int nesting;
    unsigned int repetition_depth;
    CmRulesContinuation continuation;
    void *continuation_context;
} CmRulesRepeatContinuation;

static int cm_rules_match_nodes(CmRulesMatchState *state,
    CmMacroPatternId pattern_id, cm_tt_id input, cm_tt_id parent,
    unsigned int nesting, unsigned int repetition_depth,
    CmRulesContinuation continuation, void *continuation_context);

static int cm_rules_match_repetition(CmRulesMatchState *state,
    const CmMacroPatternNode *repetition,
    CmMacroPatternId outer_next, cm_tt_id input, cm_tt_id parent,
    size_t iteration, unsigned int nesting,
    unsigned int repetition_depth,
    CmRulesContinuation continuation, void *continuation_context);

static size_t cm_rules_pattern_offset(
    const CmMacroRulesDefinition *definition,
    const CmMacroPatternNode *pattern)
{
    if (pattern == NULL) {
        return 0;
    }
    if (pattern->kind == CM_MACRO_PATTERN_TOKEN) {
        return pattern->data.token.source_start;
    }
    if (pattern->kind == CM_MACRO_PATTERN_REPETITION) {
        return pattern->data.repetition.source_start;
    }
    if (pattern->kind == CM_MACRO_PATTERN_GROUP) {
        return pattern->data.group.source_start;
    }
    if (pattern->kind == CM_MACRO_PATTERN_METAVARIABLE) {
        const CmMacroBinding *binding;

        binding = cm_macro_rules_binding(definition,
            pattern->data.metavariable.binding);
        return binding == NULL ? 0 : binding->name_start;
    }
    return 0;
}

static void cm_rules_match_error(CmRulesMatchState *state,
    CmMacroStatus status, CmMacroDiagnosticCode code,
    size_t offset, const char *message)
{
    if (state->status != CM_MACRO_OK) {
        return;
    }
    state->status = status;
    state->diagnostic.code = code;
    state->diagnostic.offset = offset;
    state->diagnostic.message = message;
}

static int cm_rules_match_step(CmRulesMatchState *state, size_t offset)
{
    if (state->status != CM_MACRO_OK) {
        return 0;
    }
    if (state->backtrack_steps
        >= state->definition->limits.max_backtrack_steps) {
        cm_rules_match_error(state, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_BACKTRACK_LIMIT, offset,
            "macro_rules backtracking step limit exceeded");
        return 0;
    }
    state->backtrack_steps += 1;
    return 1;
}

static int cm_rules_input_range_valid(const CmRulesMatchState *state,
    size_t start, size_t length)
{
    return start <= state->input_source_length
        && length <= state->input_source_length - start;
}

static int cm_rules_definition_range_valid(
    const CmMacroRulesDefinition *definition,
    size_t start, size_t length)
{
    return start <= definition->source_length
        && length <= definition->source_length - start;
}

static int cm_rules_token_matches(CmRulesMatchState *state,
    const CmMacroTokenPattern *pattern, cm_tt_id input)
{
    const struct cm_tt_node *node;

    node = cm_token_tree_node(state->input_tree, input);
    if (node == NULL || node->kind != CM_TT_NODE_TOKEN
        || node->data.token.kind != pattern->kind
        || node->data.token.length != pattern->source_length
        || !cm_rules_definition_range_valid(state->definition,
            pattern->source_start, pattern->source_length)
        || !cm_rules_input_range_valid(state,
            node->data.token.start, node->data.token.length)) {
        return 0;
    }
    return memcmp(state->definition->source + pattern->source_start,
        state->input_source + node->data.token.start,
        pattern->source_length) == 0;
}

static size_t cm_rules_sibling_count(const struct cm_token_tree *tree,
    cm_tt_id first)
{
    size_t count;
    const struct cm_tt_node *node;

    count = 0;
    node = cm_token_tree_node(tree, first);
    while (node != NULL) {
        count += 1;
        node = cm_token_tree_node(tree, node->next_sibling);
    }
    return count;
}

static cm_tt_id cm_rules_advance(const struct cm_token_tree *tree,
    cm_tt_id first, size_t count)
{
    const struct cm_tt_node *node;

    node = cm_token_tree_node(tree, first);
    while (node != NULL && count != 0) {
        count -= 1;
        node = cm_token_tree_node(tree, node->next_sibling);
    }
    return node == NULL ? CM_TT_ID_NONE : node->id;
}

static cm_tt_id cm_rules_last_consumed(const struct cm_token_tree *tree,
    cm_tt_id first, size_t count)
{
    const struct cm_tt_node *node;

    if (count == 0) {
        return CM_TT_ID_NONE;
    }
    node = cm_token_tree_node(tree, first);
    while (node != NULL && count > 1) {
        count -= 1;
        node = cm_token_tree_node(tree, node->next_sibling);
    }
    return node == NULL ? CM_TT_ID_NONE : node->id;
}

static int cm_rules_token_is_literal(const struct cm_tt_node *node)
{
    if (node == NULL || node->kind != CM_TT_NODE_TOKEN) {
        return 0;
    }
    switch (node->data.token.kind) {
    case CM_TOKEN_INTEGER:
    case CM_TOKEN_FLOAT:
    case CM_TOKEN_CHAR:
    case CM_TOKEN_BYTE_CHAR:
    case CM_TOKEN_STRING:
    case CM_TOKEN_BYTE_STRING:
    case CM_TOKEN_C_STRING:
    case CM_TOKEN_RAW_STRING:
    case CM_TOKEN_RAW_BYTE_STRING:
    case CM_TOKEN_RAW_C_STRING:
        return 1;
    case CM_TOKEN_IDENT:
        return node->data.token.keyword == CM_KW_TRUE
            || node->data.token.keyword == CM_KW_FALSE;
    default:
        return 0;
    }
}

static int cm_rules_input_token_text(CmRulesMatchState *state,
    const struct cm_tt_node *node, const char *text)
{
    size_t length;

    if (node == NULL || node->kind != CM_TT_NODE_TOKEN) {
        return 0;
    }
    length = strlen(text);
    return node->data.token.length == length
        && cm_rules_input_range_valid(state,
            node->data.token.start, node->data.token.length)
        && memcmp(state->input_source + node->data.token.start,
            text, length) == 0;
}

static int cm_rules_fragment_fixed_length(CmRulesMatchState *state,
    CmMacroFragmentKind fragment, cm_tt_id input,
    size_t *minimum, size_t *maximum)
{
    const struct cm_tt_node *node;
    const struct cm_tt_node *second;
    size_t available;

    node = cm_token_tree_node(state->input_tree, input);
    available = cm_rules_sibling_count(state->input_tree, input);
    *minimum = 1;
    *maximum = available;
    switch (fragment) {
    case CM_MACRO_FRAGMENT_IDENT:
        *maximum = 1;
        return node != NULL && node->kind == CM_TT_NODE_TOKEN
            && (node->data.token.kind == CM_TOKEN_IDENT
                || node->data.token.kind == CM_TOKEN_RAW_IDENT);
    case CM_MACRO_FRAGMENT_TT:
        *maximum = 1;
        return node != NULL;
    case CM_MACRO_FRAGMENT_BLOCK:
        *maximum = 1;
        return node != NULL && node->kind == CM_TT_NODE_GROUP
            && node->data.group.delimiter == CM_TT_DELIMITER_BRACE;
    case CM_MACRO_FRAGMENT_LITERAL:
        *maximum = 1;
        if (cm_rules_token_is_literal(node)) {
            return 1;
        }
        if (node != NULL && node->kind == CM_TT_NODE_TOKEN
            && node->data.token.kind == CM_TOKEN_MINUS) {
            second = cm_token_tree_node(state->input_tree,
                node->next_sibling);
            if (cm_rules_token_is_literal(second)) {
                *minimum = 2;
                *maximum = 2;
                return 1;
            }
        }
        return 0;
    case CM_MACRO_FRAGMENT_LIFETIME:
        *maximum = 1;
        return node != NULL && node->kind == CM_TT_NODE_TOKEN
            && node->data.token.kind == CM_TOKEN_LIFETIME;
    case CM_MACRO_FRAGMENT_VIS:
        *minimum = 0;
        *maximum = 0;
        if (cm_rules_input_token_text(state, node, "pub")) {
            *maximum = 1;
            second = cm_token_tree_node(state->input_tree,
                node->next_sibling);
            if (second != NULL && second->kind == CM_TT_NODE_GROUP
                && second->data.group.delimiter
                    == CM_TT_DELIMITER_PAREN) {
                *maximum = 2;
            }
        }
        return 1;
    case CM_MACRO_FRAGMENT_EXPR:
    case CM_MACRO_FRAGMENT_TY:
    case CM_MACRO_FRAGMENT_PAT:
    case CM_MACRO_FRAGMENT_PATH:
    case CM_MACRO_FRAGMENT_ITEM:
    case CM_MACRO_FRAGMENT_META:
        return node != NULL;
    }
    return 0;
}

static int cm_rules_fragment_is_opaque(CmMacroFragmentKind fragment)
{
    return fragment == CM_MACRO_FRAGMENT_EXPR
        || fragment == CM_MACRO_FRAGMENT_TY
        || fragment == CM_MACRO_FRAGMENT_PAT
        || fragment == CM_MACRO_FRAGMENT_PATH
        || fragment == CM_MACRO_FRAGMENT_ITEM
        || fragment == CM_MACRO_FRAGMENT_META;
}

static int cm_rules_type_or_path_candidate_valid(
    const CmRulesMatchState *state, cm_tt_id input, size_t count)
{
    const struct cm_tt_node *node;
    size_t angle_depth;

    angle_depth = 0u;
    node = cm_token_tree_node(state->input_tree, input);
    while (node != NULL && count != 0u) {
        if (node->kind == CM_TT_NODE_TOKEN) {
            if (node->data.token.kind == CM_TOKEN_LT) {
                angle_depth += 1u;
            } else if (node->data.token.kind == CM_TOKEN_GT) {
                if (angle_depth != 0u) angle_depth -= 1u;
            } else if (node->data.token.kind == CM_TOKEN_SHR) {
                if (angle_depth > 1u) {
                    angle_depth -= 2u;
                } else {
                    angle_depth = 0u;
                }
            } else if (node->data.token.kind == CM_TOKEN_COMMA
                && angle_depth == 0u) {
                return 0;
            }
        }
        count -= 1u;
        node = cm_token_tree_node(state->input_tree, node->next_sibling);
    }
    return count == 0u;
}

static size_t cm_rules_node_start(const struct cm_tt_node *node)
{
    return node->kind == CM_TT_NODE_TOKEN
        ? node->data.token.start : node->data.group.open_span.start;
}

static size_t cm_rules_node_end(const struct cm_tt_node *node)
{
    return node->kind == CM_TT_NODE_TOKEN
        ? node->data.token.start + node->data.token.length
        : node->data.group.close_span.start
            + node->data.group.close_span.length;
}

static int cm_rules_type_candidate_parses(CmRulesMatchState *state,
    cm_tt_id input, size_t count)
{
    const struct cm_tt_node *first;
    const struct cm_tt_node *last;
    cm_tt_id last_id;
    size_t start;
    size_t end;
    CmAst ast;
    CmTypeFragment fragment;
    int valid;

    if (count == 0u) return 0;
    first = cm_token_tree_node(state->input_tree, input);
    last_id = cm_rules_last_consumed(state->input_tree, input, count);
    last = cm_token_tree_node(state->input_tree, last_id);
    if (first == NULL || last == NULL) return 0;
    start = cm_rules_node_start(first);
    end = cm_rules_node_end(last);
    if (start > end || end > state->input_source_length) return 0;
    cm_ast_init(&ast);
    fragment = cm_parse_type_fragment(&ast,
        state->input_source + start, end - start, CM_EDITION_2024);
    valid = fragment.parse.error_count == 0u
        && fragment.type != CM_AST_TYPE_NONE;
    cm_ast_destroy(&ast);
    return valid;
}

static int cm_rules_fragment_candidate_valid(CmRulesMatchState *state,
    CmMacroFragmentKind fragment, cm_tt_id input, size_t count)
{
    if (fragment == CM_MACRO_FRAGMENT_TY) {
        return cm_rules_type_candidate_parses(state, input, count);
    }
    if (fragment == CM_MACRO_FRAGMENT_PATH) {
        return cm_rules_type_or_path_candidate_valid(state, input, count);
    }
    return 1;
}

static void cm_rules_capture_push(CmRulesMatchState *state,
    const CmMacroPatternNode *pattern, cm_tt_id parent,
    cm_tt_id first, cm_tt_id last, unsigned int repetition_depth)
{
    CmMacroCapture *capture;
    unsigned int index;

    capture = (CmMacroCapture *)cm_vec_push_uninit(
        &state->captures->captures);
    memset(capture, 0, sizeof(*capture));
    capture->binding = pattern->data.metavariable.binding;
    capture->fragment = pattern->data.metavariable.fragment;
    capture->repetition_depth = repetition_depth;
    for (index = 0; index < repetition_depth; index += 1) {
        capture->repetition_indices[index] =
            state->repetition_indices[index];
    }
    capture->parent = parent;
    capture->first_node = first;
    capture->last_node = last;
}

static int cm_rules_end_continuation(CmRulesMatchState *state,
    cm_tt_id input, cm_tt_id parent, void *context)
{
    (void)state;
    (void)parent;
    (void)context;
    return input == CM_TT_ID_NONE;
}

static int cm_rules_group_continuation(CmRulesMatchState *state,
    cm_tt_id input, cm_tt_id parent, void *context)
{
    CmRulesGroupContinuation *group;

    (void)parent;
    group = (CmRulesGroupContinuation *)context;
    if (input != CM_TT_ID_NONE) {
        return 0;
    }
    return cm_rules_match_nodes(state, group->outer_next,
        group->outer_input, group->outer_parent,
        group->nesting, group->repetition_depth,
        group->continuation, group->continuation_context);
}

static int cm_rules_repeat_body_continuation(CmRulesMatchState *state,
    cm_tt_id input, cm_tt_id parent, void *context)
{
    CmRulesRepeatContinuation *repeat;

    repeat = (CmRulesRepeatContinuation *)context;
    if (input == repeat->body_input) {
        /* A zero-width choice can be a normal nested-repetition
         * backtracking branch. Reject just this branch so that a consuming
         * choice can still be tried; recursing here would make no progress. */
        return 0;
    }
    return cm_rules_match_repetition(state, repeat->repetition,
        repeat->outer_next, input, parent, repeat->iteration + 1,
        repeat->nesting, repeat->repetition_depth,
        repeat->continuation, repeat->continuation_context);
}

static int cm_rules_match_repetition(CmRulesMatchState *state,
    const CmMacroPatternNode *repetition,
    CmMacroPatternId outer_next, cm_tt_id input, cm_tt_id parent,
    size_t iteration, unsigned int nesting,
    unsigned int repetition_depth,
    CmRulesContinuation continuation, void *continuation_context)
{
    size_t minimum;
    size_t maximum;
    size_t capture_snapshot;
    cm_tt_id body_input;
    const struct cm_tt_node *separator_node;
    int body_available;
    CmRulesRepeatContinuation body_context;

    minimum = repetition->data.repetition.operator_kind
        == CM_MACRO_REPETITION_ONE_OR_MORE ? 1u : 0u;
    maximum = repetition->data.repetition.operator_kind
        == CM_MACRO_REPETITION_ZERO_OR_ONE ? 1u : (size_t)-1;
    if (iteration >= state->definition->limits.max_repetition_iterations) {
        capture_snapshot = state->captures->captures.len;
        if (iteration >= minimum && cm_rules_match_nodes(state,
            outer_next, input, parent, nesting, repetition_depth,
            continuation, continuation_context)) {
            return 1;
        }
        state->captures->captures.len = capture_snapshot;
        if (state->status == CM_MACRO_OK) {
            cm_rules_match_error(state, CM_MACRO_LIMIT_EXCEEDED,
                CM_MACRO_DIAG_RULES_REPETITION_LIMIT,
                cm_rules_pattern_offset(state->definition, repetition),
                "macro repetition iteration limit exceeded");
        }
        return 0;
    }

    if (iteration < maximum && cm_rules_match_step(state,
        cm_rules_pattern_offset(state->definition, repetition))) {
        capture_snapshot = state->captures->captures.len;
        body_input = input;
        body_available = 1;
        if (iteration != 0 && repetition->data.repetition.has_separator) {
            if (cm_rules_token_matches(state,
                &repetition->data.repetition.separator, body_input)) {
                separator_node = cm_token_tree_node(state->input_tree,
                    body_input);
                body_input = separator_node->next_sibling;
            } else {
                body_available = 0;
            }
        }
        if (body_available
            && repetition_depth
                < state->definition->limits.max_nesting
            && repetition_depth
                < CM_MACRO_RULES_ABSOLUTE_MAX_NESTING) {
            state->repetition_indices[repetition_depth] = iteration;
            body_context.repetition = repetition;
            body_context.outer_next = outer_next;
            /* A separator counts as progress even if a nested body is empty. */
            body_context.body_input = input;
            body_context.parent = parent;
            body_context.iteration = iteration;
            body_context.nesting = nesting;
            body_context.repetition_depth = repetition_depth;
            body_context.continuation = continuation;
            body_context.continuation_context = continuation_context;
            if (cm_rules_match_nodes(state, repetition->first_child,
                body_input, parent, nesting + 1u,
                repetition_depth + 1u,
                cm_rules_repeat_body_continuation, &body_context)) {
                return 1;
            }
        }
        state->captures->captures.len = capture_snapshot;
    }
    if (state->status != CM_MACRO_OK) {
        return 0;
    }
    if (iteration >= minimum && cm_rules_match_step(state,
        cm_rules_pattern_offset(state->definition, repetition))) {
        capture_snapshot = state->captures->captures.len;
        if (cm_rules_match_nodes(state, outer_next,
            input, parent, nesting, repetition_depth,
            continuation, continuation_context)) {
            return 1;
        }
        state->captures->captures.len = capture_snapshot;
    }
    return 0;
}

static int cm_rules_match_nodes(CmRulesMatchState *state,
    CmMacroPatternId pattern_id, cm_tt_id input, cm_tt_id parent,
    unsigned int nesting, unsigned int repetition_depth,
    CmRulesContinuation continuation, void *continuation_context)
{
    const CmMacroPatternNode *pattern;
    const struct cm_tt_node *input_node;
    size_t minimum;
    size_t maximum;
    size_t consumed;
    size_t capture_snapshot;
    size_t repetition_indices_snapshot[CM_MACRO_RULES_ABSOLUTE_MAX_NESTING];
    cm_tt_id after;
    cm_tt_id last;
    CmRulesGroupContinuation group_context;

    if (state->status != CM_MACRO_OK) {
        return 0;
    }
    if (pattern_id == CM_MACRO_PATTERN_NONE) {
        return continuation(state, input, parent, continuation_context);
    }
    pattern = cm_macro_rules_pattern(state->definition, pattern_id);
    if (pattern == NULL || !cm_rules_match_step(state,
        cm_rules_pattern_offset(state->definition, pattern))) {
        return 0;
    }
    input_node = cm_token_tree_node(state->input_tree, input);
    if (pattern->kind == CM_MACRO_PATTERN_TOKEN) {
        if (!cm_rules_token_matches(state, &pattern->data.token, input)) {
            return 0;
        }
        return cm_rules_match_nodes(state, pattern->next_sibling,
            input_node->next_sibling, parent, nesting, repetition_depth,
            continuation, continuation_context);
    }
    if (pattern->kind == CM_MACRO_PATTERN_GROUP) {
        if (input_node == NULL || input_node->kind != CM_TT_NODE_GROUP
            || input_node->data.group.delimiter
                != pattern->data.group.delimiter) {
            return 0;
        }
        if (nesting >= state->definition->limits.max_nesting) {
            cm_rules_match_error(state, CM_MACRO_LIMIT_EXCEEDED,
                CM_MACRO_DIAG_RULES_NESTING_LIMIT,
                cm_rules_pattern_offset(state->definition, pattern),
                "macro_rules match nesting limit exceeded");
            return 0;
        }
        group_context.outer_next = pattern->next_sibling;
        group_context.outer_input = input_node->next_sibling;
        group_context.outer_parent = parent;
        group_context.nesting = nesting;
        group_context.repetition_depth = repetition_depth;
        group_context.continuation = continuation;
        group_context.continuation_context = continuation_context;
        return cm_rules_match_nodes(state, pattern->first_child,
            input_node->first_child, input_node->id, nesting + 1u,
            repetition_depth, cm_rules_group_continuation, &group_context);
    }
    if (pattern->kind == CM_MACRO_PATTERN_REPETITION) {
        if (nesting >= state->definition->limits.max_nesting
            || repetition_depth >= state->definition->limits.max_nesting
            || repetition_depth
                >= CM_MACRO_RULES_ABSOLUTE_MAX_NESTING) {
            cm_rules_match_error(state, CM_MACRO_LIMIT_EXCEEDED,
                CM_MACRO_DIAG_RULES_NESTING_LIMIT,
                cm_rules_pattern_offset(state->definition, pattern),
                "macro_rules repetition nesting limit exceeded");
            return 0;
        }
        return cm_rules_match_repetition(state, pattern,
            pattern->next_sibling, input, parent, 0,
            nesting, repetition_depth,
            continuation, continuation_context);
    }

    if (!cm_rules_fragment_fixed_length(state,
        pattern->data.metavariable.fragment, input,
        &minimum, &maximum)) {
        return 0;
    }
    if (repetition_depth != 0u) {
        memcpy(repetition_indices_snapshot, state->repetition_indices,
            repetition_depth * sizeof(size_t));
    }
    consumed = cm_rules_fragment_is_opaque(
        pattern->data.metavariable.fragment) ? minimum : maximum;
    for (;;) {
        if (!cm_rules_match_step(state,
            cm_rules_pattern_offset(state->definition, pattern))) {
            return 0;
        }
        after = cm_rules_advance(state->input_tree, input, consumed);
        last = cm_rules_last_consumed(state->input_tree, input, consumed);
        if (cm_rules_fragment_candidate_valid(state,
                pattern->data.metavariable.fragment, input, consumed)) {
            capture_snapshot = state->captures->captures.len;
            cm_rules_capture_push(state, pattern, parent,
                consumed == 0 ? CM_TT_ID_NONE : input,
                last, repetition_depth);
            if (cm_rules_match_nodes(state, pattern->next_sibling,
                after, parent, nesting, repetition_depth,
                continuation, continuation_context)) {
                return 1;
            }
            state->captures->captures.len = capture_snapshot;
            if (repetition_depth != 0u) {
                memcpy(state->repetition_indices,
                    repetition_indices_snapshot,
                    repetition_depth * sizeof(size_t));
            }
        }
        if (state->status != CM_MACRO_OK) {
            break;
        }
        if (cm_rules_fragment_is_opaque(
                pattern->data.metavariable.fragment)) {
            if (consumed == maximum) break;
            consumed += 1;
        } else {
            if (consumed == minimum) break;
            consumed -= 1;
        }
    }
    return 0;
}

void cm_macro_capture_set_init(CmMacroCaptureSet *captures)
{
    if (captures == NULL) {
        return;
    }
    captures->input_tree = NULL;
    captures->input_source = NULL;
    captures->input_source_length = 0;
    captures->arm_index = (size_t)-1;
    cm_vec_init(&captures->captures, sizeof(CmMacroCapture));
}

void cm_macro_capture_set_destroy(CmMacroCaptureSet *captures)
{
    if (captures == NULL) {
        return;
    }
    cm_vec_destroy(&captures->captures);
    captures->input_tree = NULL;
    captures->input_source = NULL;
    captures->input_source_length = 0;
    captures->arm_index = (size_t)-1;
}

const CmMacroCapture *cm_macro_capture(
    const CmMacroCaptureSet *captures, size_t index)
{
    if (captures == NULL) {
        return NULL;
    }
    return (const CmMacroCapture *)cm_vec_at_const(&captures->captures, index);
}

static CmMacroRulesMatchResult cm_rules_match_invalid(void)
{
    CmMacroRulesMatchResult result;

    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.arm_index = (size_t)-1;
    result.backtrack_steps = 0;
    result.diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    result.diagnostic.offset = 0;
    result.diagnostic.message = "invalid macro_rules matcher argument";
    return result;
}

CmMacroRulesMatchResult cm_macro_rules_match(
    const CmMacroRulesDefinition *definition,
    const struct cm_token_tree *input_tree,
    const char *input_source,
    size_t input_source_length,
    cm_tt_id input,
    CmMacroCaptureSet *captures)
{
    CmMacroRulesMatchResult result;
    CmRulesMatchState state;
    const struct cm_tt_node *input_node;
    const CmMacroRuleArm *arm;
    cm_tt_id first_input;
    cm_tt_id parent;
    size_t arm_index;

    if (definition == NULL || input_tree == NULL || captures == NULL
        || (input_source == NULL && input_source_length != 0)
        || definition->arms.len == 0) {
        return cm_rules_match_invalid();
    }
    if (input_tree->errors.len != 0) {
        const struct cm_tt_error *tree_error;

        result.status = CM_MACRO_SYNTAX_ERROR;
        result.arm_index = (size_t)-1;
        result.backtrack_steps = 0;
        tree_error = cm_token_tree_error(input_tree, 0);
        result.diagnostic.code = CM_MACRO_DIAG_RULES_INVALID_TREE;
        result.diagnostic.offset = tree_error == NULL
            ? 0 : tree_error->span.start;
        result.diagnostic.message =
            "macro invocation has invalid token-tree delimiters";
        return result;
    }
    input_node = cm_token_tree_node(input_tree, input);
    if (input_node == NULL || (input_node->kind != CM_TT_NODE_ROOT
        && input_node->kind != CM_TT_NODE_GROUP)) {
        return cm_rules_match_invalid();
    }
    captures->input_tree = NULL;
    captures->input_source = NULL;
    captures->input_source_length = 0;
    captures->arm_index = (size_t)-1;
    cm_vec_clear(&captures->captures);
    state.definition = definition;
    state.input_tree = input_tree;
    state.input_source = input_source == NULL ? "" : input_source;
    state.input_source_length = input_source_length;
    state.captures = captures;
    state.backtrack_steps = 0;
    memset(state.repetition_indices, 0, sizeof(state.repetition_indices));
    state.status = CM_MACRO_OK;
    state.diagnostic.code = CM_MACRO_DIAG_NONE;
    state.diagnostic.offset = 0;
    state.diagnostic.message = "";
    first_input = input_node->first_child;
    parent = input_node->id;
    for (arm_index = 0; arm_index < definition->arms.len;
        arm_index += 1) {
        cm_vec_clear(&captures->captures);
        arm = cm_macro_rules_arm(definition, arm_index);
        if (cm_rules_match_nodes(&state, arm->matcher_first,
            first_input, parent, 0u, 0u,
            cm_rules_end_continuation, NULL)) {
            captures->input_tree = input_tree;
            captures->input_source = state.input_source;
            captures->input_source_length = input_source_length;
            captures->arm_index = arm_index;
            result.status = CM_MACRO_OK;
            result.arm_index = arm_index;
            result.backtrack_steps = state.backtrack_steps;
            result.diagnostic.code = CM_MACRO_DIAG_NONE;
            result.diagnostic.offset = 0;
            result.diagnostic.message = "";
            return result;
        }
        if (state.status != CM_MACRO_OK) {
            break;
        }
    }
    cm_vec_clear(&captures->captures);
    result.status = state.status == CM_MACRO_OK
        ? CM_MACRO_NO_MATCH : state.status;
    result.arm_index = (size_t)-1;
    result.backtrack_steps = state.backtrack_steps;
    if (state.status == CM_MACRO_OK) {
        result.diagnostic.code = CM_MACRO_DIAG_RULES_NO_MATCH;
        result.diagnostic.offset = input_node->kind == CM_TT_NODE_GROUP
            ? input_node->data.group.open_span.start : 0;
        result.diagnostic.message = "no macro_rules arm matched the input";
    } else {
        result.diagnostic = state.diagnostic;
    }
    return result;
}
