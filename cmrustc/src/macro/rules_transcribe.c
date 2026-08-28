#include "cm/macro_rules.h"

#include <stdio.h>
#include <string.h>

typedef struct CmRulesTranscribeState {
    const CmMacroRulesDefinition *definition;
    const CmMacroCaptureSet *captures;
    const char *crate_identifier;
    CmStrBuf *output;
    size_t repetition_indices[CM_MACRO_RULES_ABSOLUTE_MAX_NESTING];
    size_t emitted_repetitions;
    int need_space;
    CmMacroStatus status;
    CmMacroDiagnostic diagnostic;
} CmRulesTranscribeState;

static void cm_rules_transcribe_error(CmRulesTranscribeState *state,
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

static int cm_rules_transcribe_source_valid(
    const CmMacroRulesDefinition *definition,
    size_t start, size_t length)
{
    return start <= definition->source_length
        && length <= definition->source_length - start;
}

static int cm_rules_capture_source_valid(
    const CmMacroCaptureSet *captures,
    size_t start, size_t length)
{
    return start <= captures->input_source_length
        && length <= captures->input_source_length - start;
}

static size_t cm_rules_transcribe_pattern_offset(
    const CmMacroRulesDefinition *definition,
    const CmMacroPatternNode *pattern)
{
    const CmMacroBinding *binding;

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
    if (pattern->kind == CM_MACRO_PATTERN_CONCAT) {
        return pattern->data.concat.source_start;
    }
    if (pattern->kind == CM_MACRO_PATTERN_CRATE
        || pattern->kind == CM_MACRO_PATTERN_IGNORE
        || pattern->kind == CM_MACRO_PATTERN_INDEX
        || pattern->kind == CM_MACRO_PATTERN_COUNT) {
        return pattern->data.metavariable_expression.source_start;
    }
    if (pattern->kind == CM_MACRO_PATTERN_METAVARIABLE) {
        binding = cm_macro_rules_binding(definition,
            pattern->data.metavariable.binding);
        return binding == NULL ? 0 : binding->name_start;
    }
    return 0;
}

static void cm_rules_emit_space(CmRulesTranscribeState *state)
{
    if (state->need_space) {
        cm_str_buf_push(state->output, ' ');
    }
}

static int cm_rules_emit_definition_token(CmRulesTranscribeState *state,
    const CmMacroTokenPattern *token)
{
    if (!cm_rules_transcribe_source_valid(state->definition,
        token->source_start, token->source_length)) {
        cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_DIAG_RULES_INVALID_TREE, token->source_start,
            "macro transcriber token is outside its source buffer");
        return 0;
    }
    cm_rules_emit_space(state);
    cm_str_buf_append_n(state->output,
        state->definition->source + token->source_start,
        token->source_length);
    state->need_space = 1;
    return 1;
}

static int cm_rules_crate_identifier_valid(const char *identifier)
{
    const unsigned char *bytes;
    size_t index;

    if (identifier == NULL || identifier[0] == 0) return 0;
    bytes = (const unsigned char *)identifier;
    if (!((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) {
        return 0;
    }
    for (index = 1u; bytes[index] != 0; ++index) {
        if (!((bytes[index] >= (unsigned char)'a'
                    && bytes[index] <= (unsigned char)'z')
                || (bytes[index] >= (unsigned char)'A'
                    && bytes[index] <= (unsigned char)'Z')
                || (bytes[index] >= (unsigned char)'0'
                    && bytes[index] <= (unsigned char)'9')
                || bytes[index] == (unsigned char)'_')) {
            return 0;
        }
    }
    return 1;
}

static int cm_rules_emit_crate(CmRulesTranscribeState *state)
{
    cm_rules_emit_space(state);
    cm_str_buf_append_n(state->output, state->crate_identifier,
        strlen(state->crate_identifier));
    state->need_space = 1;
    return 1;
}

static int cm_rules_capture_prefix_matches(
    const CmRulesTranscribeState *state,
    const CmMacroCapture *capture, unsigned int depth)
{
    unsigned int index;

    if (capture->repetition_depth < depth) {
        return 0;
    }
    for (index = 0; index < depth; index += 1) {
        if (capture->repetition_indices[index]
            != state->repetition_indices[index]) {
            return 0;
        }
    }
    return 1;
}

static const CmMacroCapture *cm_rules_find_capture(
    const CmRulesTranscribeState *state,
    CmMacroBindingId binding, unsigned int depth)
{
    size_t index;
    const CmMacroCapture *capture;

    for (index = 0; index < state->captures->captures.len; index += 1) {
        capture = cm_macro_capture(state->captures, index);
        if (capture->binding == binding
            && capture->repetition_depth <= depth
            && cm_rules_capture_prefix_matches(state, capture,
                capture->repetition_depth)) {
            return capture;
        }
    }
    return NULL;
}

static size_t cm_rules_input_node_start(
    const struct cm_tt_node *node)
{
    if (node->kind == CM_TT_NODE_TOKEN) {
        return node->data.token.start;
    }
    return node->data.group.open_span.start;
}

static size_t cm_rules_input_node_end(const struct cm_tt_node *node)
{
    if (node->kind == CM_TT_NODE_TOKEN) {
        return node->data.token.start + node->data.token.length;
    }
    return node->data.group.close_span.start
        + node->data.group.close_span.length;
}

static int cm_rules_emit_capture(CmRulesTranscribeState *state,
    const CmMacroCapture *capture, int leading_space)
{
    const struct cm_tt_node *first;
    const struct cm_tt_node *last;
    size_t start;
    size_t end;

    if (capture->first_node == CM_TT_ID_NONE) {
        return 1;
    }
    first = cm_token_tree_node(state->captures->input_tree,
        capture->first_node);
    last = cm_token_tree_node(state->captures->input_tree,
        capture->last_node);
    if (first == NULL || last == NULL) {
        cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_DIAG_RULES_INVALID_TREE, 0,
            "macro capture references a missing token-tree node");
        return 0;
    }
    start = cm_rules_input_node_start(first);
    end = cm_rules_input_node_end(last);
    if (end < start || !cm_rules_capture_source_valid(state->captures,
        start, end - start)) {
        cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_DIAG_RULES_INVALID_TREE, start,
            "macro capture is outside its input source buffer");
        return 0;
    }
    if (leading_space) cm_rules_emit_space(state);
    cm_str_buf_append_n(state->output,
        state->captures->input_source + start, end - start);
    state->need_space = 1;
    return 1;
}

static int cm_rules_binding_iteration_count(
    CmRulesTranscribeState *state, CmMacroBindingId binding,
    unsigned int depth, size_t *count, int *exact)
{
    size_t index;
    size_t candidate_count;
    size_t expected_index;
    int found;
    int index_found;
    const CmMacroCapture *capture;
    const CmMacroBinding *binding_definition;

    binding_definition = cm_macro_rules_binding(state->definition, binding);
    if (binding_definition == NULL
        || binding_definition->repetition_depth <= depth) {
        return 0;
    }
    *exact = binding_definition->repetition_depth == depth + 1u;
    found = 0;
    candidate_count = 0;
    for (index = 0; index < state->captures->captures.len; index += 1) {
        capture = cm_macro_capture(state->captures, index);
        if (capture->binding != binding
            || capture->repetition_depth <= depth
            || !cm_rules_capture_prefix_matches(state, capture, depth)) {
            continue;
        }
        found = 1;
        if (capture->repetition_indices[depth] >= candidate_count) {
            candidate_count = capture->repetition_indices[depth] + 1;
        }
    }
    if (!found) {
        *count = 0;
        return 1;
    }
    for (expected_index = 0; *exact && expected_index < candidate_count;
        expected_index += 1) {
        index_found = 0;
        for (index = 0; index < state->captures->captures.len; index += 1) {
            capture = cm_macro_capture(state->captures, index);
            if (capture->binding == binding
                && capture->repetition_depth > depth
                && cm_rules_capture_prefix_matches(state, capture, depth)
                && capture->repetition_indices[depth] == expected_index) {
                index_found = 1;
                break;
            }
        }
        if (!index_found) {
            cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
                CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION, 0,
                "macro repetition capture indices are not contiguous");
            return 0;
        }
    }
    *count = candidate_count;
    return 1;
}

static int cm_rules_repetition_count_nodes(
    CmRulesTranscribeState *state, CmMacroPatternId first,
    unsigned int depth, int *found_driver, int *found_exact,
    size_t *count)
{
    const CmMacroPatternNode *pattern;
    size_t binding_count;
    int binding_exact;

    pattern = cm_macro_rules_pattern(state->definition, first);
    while (pattern != NULL) {
        if (pattern->kind == CM_MACRO_PATTERN_METAVARIABLE) {
            if (cm_rules_binding_iteration_count(state,
                pattern->data.metavariable.binding,
                depth, &binding_count, &binding_exact)) {
                if (!*found_driver) {
                    *found_driver = 1;
                    *count = binding_count;
                    *found_exact = binding_exact;
                } else if (binding_exact && *found_exact
                    && *count != binding_count) {
                    cm_rules_transcribe_error(state,
                        CM_MACRO_SYNTAX_ERROR,
                        CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
                        cm_rules_transcribe_pattern_offset(
                            state->definition, pattern),
                        "metavariables repeat with incompatible lengths");
                    return 0;
                } else if (binding_exact) {
                    if (*count > binding_count) {
                        cm_rules_transcribe_error(state,
                            CM_MACRO_SYNTAX_ERROR,
                            CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
                            cm_rules_transcribe_pattern_offset(
                                state->definition, pattern),
                            "nested metavariable exceeds outer repetition "
                            "length");
                        return 0;
                    }
                    *count = binding_count;
                    *found_exact = 1;
                } else if (!*found_exact && binding_count > *count) {
                    *count = binding_count;
                } else if (*found_exact && binding_count > *count) {
                    cm_rules_transcribe_error(state,
                        CM_MACRO_SYNTAX_ERROR,
                        CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
                        cm_rules_transcribe_pattern_offset(
                            state->definition, pattern),
                        "nested metavariable exceeds outer repetition "
                        "length");
                    return 0;
                }
            }
        }
        if (pattern->first_child != CM_MACRO_PATTERN_NONE
            && !cm_rules_repetition_count_nodes(state,
                pattern->first_child, depth, found_driver, found_exact,
                count)) {
            return 0;
        }
        pattern = cm_macro_rules_pattern(state->definition,
            pattern->next_sibling);
    }
    return state->status == CM_MACRO_OK;
}

static int cm_rules_emit_nodes(CmRulesTranscribeState *state,
    CmMacroPatternId first, unsigned int nesting,
    unsigned int repetition_depth);

static char cm_rules_open_delimiter(enum cm_tt_delimiter delimiter)
{
    switch (delimiter) {
    case CM_TT_DELIMITER_PAREN:
        return '(';
    case CM_TT_DELIMITER_BRACKET:
        return '[';
    case CM_TT_DELIMITER_BRACE:
        return '{';
    case CM_TT_DELIMITER_NONE:
        break;
    }
    return '\0';
}

static char cm_rules_close_delimiter(enum cm_tt_delimiter delimiter)
{
    switch (delimiter) {
    case CM_TT_DELIMITER_PAREN:
        return ')';
    case CM_TT_DELIMITER_BRACKET:
        return ']';
    case CM_TT_DELIMITER_BRACE:
        return '}';
    case CM_TT_DELIMITER_NONE:
        break;
    }
    return '\0';
}

static int cm_rules_emit_group(CmRulesTranscribeState *state,
    const CmMacroPatternNode *pattern, unsigned int nesting,
    unsigned int repetition_depth)
{
    char open;
    char close;

    if (nesting >= state->definition->limits.max_nesting) {
        cm_rules_transcribe_error(state, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_NESTING_LIMIT,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "macro transcriber nesting limit exceeded");
        return 0;
    }
    open = cm_rules_open_delimiter(pattern->data.group.delimiter);
    close = cm_rules_close_delimiter(pattern->data.group.delimiter);
    if (open == '\0' || close == '\0') {
        cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
            CM_MACRO_DIAG_RULES_INVALID_TREE, 0,
            "macro transcriber has an invalid delimiter");
        return 0;
    }
    cm_rules_emit_space(state);
    cm_str_buf_push(state->output, open);
    state->need_space = 0;
    if (!cm_rules_emit_nodes(state, pattern->first_child,
        nesting + 1u, repetition_depth)) {
        return 0;
    }
    cm_str_buf_push(state->output, close);
    state->need_space = 1;
    return 1;
}

static int cm_rules_emit_repetition(CmRulesTranscribeState *state,
    const CmMacroPatternNode *pattern, unsigned int nesting,
    unsigned int repetition_depth)
{
    int found_driver;
    int found_exact;
    size_t count;
    size_t index;

    if (nesting >= state->definition->limits.max_nesting
        || repetition_depth >= state->definition->limits.max_nesting
        || repetition_depth >= CM_MACRO_RULES_ABSOLUTE_MAX_NESTING) {
        cm_rules_transcribe_error(state, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_NESTING_LIMIT,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "macro transcriber repetition nesting limit exceeded");
        return 0;
    }
    found_driver = 0;
    found_exact = 0;
    count = 0;
    if (!cm_rules_repetition_count_nodes(state, pattern->first_child,
        repetition_depth, &found_driver, &found_exact, &count)) {
        return 0;
    }
    if (!found_driver) {
        cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "macro transcriber repetition has no repeated metavariable");
        return 0;
    }
    if (count > state->definition->limits.max_repetition_iterations) {
        cm_rules_transcribe_error(state, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_REPETITION_LIMIT,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "macro transcriber repetition iteration limit exceeded");
        return 0;
    }
    if (pattern->data.repetition.operator_kind
            == CM_MACRO_REPETITION_ZERO_OR_ONE
        && count > 1) {
        cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "'?' transcriber repetition expanded more than once");
        return 0;
    }
    for (index = 0; index < count; index += 1) {
        if (index != 0 && pattern->data.repetition.has_separator
            && !cm_rules_emit_definition_token(state,
                &pattern->data.repetition.separator)) {
            return 0;
        }
        state->repetition_indices[repetition_depth] = index;
        state->emitted_repetitions += 1;
        if (!cm_rules_emit_nodes(state, pattern->first_child,
            nesting + 1u, repetition_depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_rules_emit_concat(CmRulesTranscribeState *state,
    const CmMacroPatternNode *pattern, unsigned int repetition_depth)
{
    const CmMacroPatternNode *part;
    const CmMacroCapture *capture;

    cm_rules_emit_space(state);
    state->need_space = 0;
    part = cm_macro_rules_pattern(state->definition, pattern->first_child);
    while (part != NULL) {
        if (part->kind == CM_MACRO_PATTERN_TOKEN) {
            if (!cm_rules_transcribe_source_valid(state->definition,
                    part->data.token.source_start,
                    part->data.token.source_length)) {
                cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
                    CM_MACRO_DIAG_RULES_INVALID_TREE,
                    part->data.token.source_start,
                    "concat identifier is outside its source buffer");
                return 0;
            }
            cm_str_buf_append_n(state->output,
                state->definition->source + part->data.token.source_start,
                part->data.token.source_length);
        } else if (part->kind == CM_MACRO_PATTERN_METAVARIABLE) {
            capture = cm_rules_find_capture(state,
                part->data.metavariable.binding, repetition_depth);
            if (capture == NULL
                || !cm_rules_emit_capture(state, capture, 0)) {
                if (capture == NULL) {
                    cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
                        CM_MACRO_DIAG_RULES_UNKNOWN_BINDING,
                        cm_rules_transcribe_pattern_offset(
                            state->definition, part),
                        "concat capture has an incompatible repetition depth");
                }
                return 0;
            }
        } else {
            cm_rules_transcribe_error(state, CM_MACRO_INVALID_ARGUMENT,
                CM_MACRO_DIAG_RULES_INVALID_TREE,
                cm_rules_transcribe_pattern_offset(
                    state->definition, part),
                "concat contains an invalid pattern part");
            return 0;
        }
        part = cm_macro_rules_pattern(state->definition,
            part->next_sibling);
    }
    state->need_space = 1;
    return 1;
}

static int cm_rules_emit_number(CmRulesTranscribeState *state,
    size_t value, const CmMacroPatternNode *pattern)
{
    char text[32];
    int length;

    length = snprintf(text, sizeof(text), "%lu", (unsigned long)value);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        cm_rules_transcribe_error(state, CM_MACRO_LIMIT_EXCEEDED,
            CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "metavariable expression result exceeds its output buffer");
        return 0;
    }
    cm_rules_emit_space(state);
    cm_str_buf_append_n(state->output, text, (size_t)length);
    state->need_space = 1;
    return 1;
}

static int cm_rules_emit_count(CmRulesTranscribeState *state,
    const CmMacroPatternNode *pattern, unsigned int repetition_depth)
{
    const CmMacroPatternNode *child;
    size_t count;
    int exact;

    child = cm_macro_rules_pattern(state->definition, pattern->first_child);
    if (child == NULL || child->kind != CM_MACRO_PATTERN_METAVARIABLE
        || !cm_rules_binding_iteration_count(state,
            child->data.metavariable.binding, repetition_depth,
            &count, &exact)) {
        cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
            CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
            cm_rules_transcribe_pattern_offset(state->definition, pattern),
            "count capture has no repetition at this depth");
        return 0;
    }
    (void)exact;
    return cm_rules_emit_number(state, count, pattern);
}

static int cm_rules_emit_nodes(CmRulesTranscribeState *state,
    CmMacroPatternId first, unsigned int nesting,
    unsigned int repetition_depth)
{
    const CmMacroPatternNode *pattern;
    const CmMacroCapture *capture;

    pattern = cm_macro_rules_pattern(state->definition, first);
    while (pattern != NULL) {
        switch (pattern->kind) {
        case CM_MACRO_PATTERN_TOKEN:
            if (!cm_rules_emit_definition_token(state,
                &pattern->data.token)) {
                return 0;
            }
            break;
        case CM_MACRO_PATTERN_GROUP:
            if (!cm_rules_emit_group(state, pattern,
                nesting, repetition_depth)) {
                return 0;
            }
            break;
        case CM_MACRO_PATTERN_METAVARIABLE:
            capture = cm_rules_find_capture(state,
                pattern->data.metavariable.binding, repetition_depth);
            if (capture == NULL) {
                cm_rules_transcribe_error(state, CM_MACRO_SYNTAX_ERROR,
                    CM_MACRO_DIAG_RULES_UNKNOWN_BINDING,
                    cm_rules_transcribe_pattern_offset(
                        state->definition, pattern),
                    "metavariable is used at an incompatible repetition depth");
                return 0;
            }
            {
                /* rustc splices `$x:expr` captures as one invisible group;
                 * textual transcription must parenthesize them so
                 * `$f(*self)` with `$f = |x| x == 0` does not reparse as
                 * calling the literal `0` (alloc's impl_is_zero!). */
                const CmMacroBinding *metavariable_binding =
                    cm_macro_rules_binding(state->definition,
                        pattern->data.metavariable.binding);
                const CmMacroPatternNode *next_pattern =
                    cm_macro_rules_pattern(state->definition,
                        pattern->next_sibling);
                int wrap = metavariable_binding != NULL
                    && metavariable_binding->fragment
                        == CM_MACRO_FRAGMENT_EXPR
                    && capture->first_node != CM_TT_ID_NONE
                    /* Single-token captures stay bare: they are already
                     * unambiguous and nested macro matchers expect the
                     * raw token, not a parenthesized group. */
                    && capture->first_node != capture->last_node
                    /* Wrap only in call position (`$f(args)`): nested
                     * macro forwarding (`inner!($e)`, `$e,`) must see the
                     * raw tokens, and only a following paren group can
                     * reparse the capture's tail as a call. */
                    && next_pattern != NULL
                    && next_pattern->kind == CM_MACRO_PATTERN_GROUP
                    && next_pattern->data.group.delimiter
                        == CM_TT_DELIMITER_PAREN;
                if (wrap) {
                    cm_rules_emit_space(state);
                    cm_str_buf_append(state->output, "(");
                    state->need_space = 0;
                }
                if (!cm_rules_emit_capture(state, capture, !wrap)) {
                    return 0;
                }
                if (wrap) {
                    cm_str_buf_append(state->output, ")");
                    state->need_space = 1;
                }
            }
            break;
        case CM_MACRO_PATTERN_REPETITION:
            if (!cm_rules_emit_repetition(state, pattern,
                nesting, repetition_depth)) {
                return 0;
            }
            break;
        case CM_MACRO_PATTERN_CRATE:
            if (!cm_rules_emit_crate(state)) return 0;
            break;
        case CM_MACRO_PATTERN_CONCAT:
            if (!cm_rules_emit_concat(state, pattern, repetition_depth)) {
                return 0;
            }
            break;
        case CM_MACRO_PATTERN_IGNORE:
            break;
        case CM_MACRO_PATTERN_INDEX:
            if (repetition_depth == 0u
                || !cm_rules_emit_number(state,
                    state->repetition_indices[repetition_depth - 1u],
                    pattern)) {
                if (repetition_depth == 0u) {
                    cm_rules_transcribe_error(state,
                        CM_MACRO_SYNTAX_ERROR,
                        CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION,
                        cm_rules_transcribe_pattern_offset(
                            state->definition, pattern),
                        "index is outside a transcriber repetition");
                }
                return 0;
            }
            break;
        case CM_MACRO_PATTERN_COUNT:
            if (!cm_rules_emit_count(state, pattern, repetition_depth)) {
                return 0;
            }
            break;
        }
        pattern = cm_macro_rules_pattern(state->definition,
            pattern->next_sibling);
    }
    return 1;
}

static CmMacroRulesTranscribeResult cm_rules_transcribe_invalid(void)
{
    CmMacroRulesTranscribeResult result;

    result.status = CM_MACRO_INVALID_ARGUMENT;
    result.emitted_repetitions = 0;
    result.diagnostic.code = CM_MACRO_DIAG_INVALID_ARGUMENT;
    result.diagnostic.offset = 0;
    result.diagnostic.message = "invalid macro_rules transcriber argument";
    return result;
}

CmMacroRulesTranscribeResult cm_macro_rules_transcribe_with_crate(
    const CmMacroRulesDefinition *definition,
    const CmMacroCaptureSet *captures,
    const char *crate_identifier,
    CmStrBuf *output)
{
    CmMacroRulesTranscribeResult result;
    CmRulesTranscribeState state;
    const CmMacroRuleArm *arm;

    if (output != NULL) {
        cm_str_buf_clear(output);
    }
    if (definition == NULL || captures == NULL || output == NULL
        || !cm_rules_crate_identifier_valid(crate_identifier)
        || captures->input_tree == NULL || captures->input_source == NULL
        || captures->arm_index >= definition->arms.len) {
        return cm_rules_transcribe_invalid();
    }
    arm = cm_macro_rules_arm(definition, captures->arm_index);
    state.definition = definition;
    state.captures = captures;
    state.crate_identifier = crate_identifier;
    state.output = output;
    memset(state.repetition_indices, 0, sizeof(state.repetition_indices));
    state.emitted_repetitions = 0;
    state.need_space = 0;
    state.status = CM_MACRO_OK;
    state.diagnostic.code = CM_MACRO_DIAG_NONE;
    state.diagnostic.offset = 0;
    state.diagnostic.message = "";
    if (!cm_rules_emit_nodes(&state, arm->transcriber_first, 0u, 0u)) {
        cm_str_buf_clear(output);
    }
    result.status = state.status;
    result.emitted_repetitions = state.emitted_repetitions;
    result.diagnostic = state.diagnostic;
    return result;
}

CmMacroRulesTranscribeResult cm_macro_rules_transcribe(
    const CmMacroRulesDefinition *definition,
    const CmMacroCaptureSet *captures,
    CmStrBuf *output)
{
    return cm_macro_rules_transcribe_with_crate(definition, captures,
        "crate", output);
}
