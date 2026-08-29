#include "cm/macro/expand.h"

#include "cm/alloc.h"
#include "cm/vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CM_EXPAND_DEFAULT_NESTING 64u
#define CM_EXPAND_DEFAULT_ITEMS ((size_t)1000000u)
#define CM_EXPAND_DEFAULT_ATTRIBUTES ((size_t)1000000u)
#define CM_EXPAND_DELIMITER_LIMIT 64u

typedef struct CmMetaSlice {
    const unsigned char *bytes;
    size_t length;
    size_t source_offset;
} CmMetaSlice;

typedef struct CmExpandContext {
    const CmAst *ast;
    const CmExpandOptions *options;
    CmExpandResult result;
    size_t items_seen;
    size_t attribute_expansions;
} CmExpandContext;

static void cm_expanded_item_destroy(CmExpandedItem *item)
{
    size_t index;

    for (index = 0u; index < item->child_count; ++index) {
        cm_expanded_item_destroy(&item->children[index]);
    }
    cm_free(item->children);
    cm_free(item->inner_attributes);
    cm_free(item->attributes);
    memset(item, 0, sizeof(*item));
}

void cm_cfg_set_init(CmCfgSet *set)
{
    if (set == NULL) {
        return;
    }
    cm_cfg_environment_init(&set->environment);
}

void cm_expand_options_init(CmExpandOptions *options,
    const CmCfgSet *cfg)
{
    if (options == NULL) {
        return;
    }
    options->cfg = cfg;
    options->maximum_nesting = CM_EXPAND_DEFAULT_NESTING;
    options->maximum_items = CM_EXPAND_DEFAULT_ITEMS;
    options->maximum_attribute_expansions = CM_EXPAND_DEFAULT_ATTRIBUTES;
}

void cm_expanded_ast_init(CmExpandedAst *expanded)
{
    if (expanded != NULL) {
        memset(expanded, 0, sizeof(*expanded));
    }
}

void cm_expanded_ast_destroy(CmExpandedAst *expanded)
{
    size_t index;

    if (expanded == NULL) {
        return;
    }
    for (index = 0u; index < expanded->root_item_count; ++index) {
        cm_expanded_item_destroy(&expanded->root_items[index]);
    }
    cm_free(expanded->root_items);
    cm_free(expanded->crate_attributes);
    memset(expanded, 0, sizeof(*expanded));
}

void cm_expanded_item_sequence_init(CmExpandedItemSequence *expanded)
{
    if (expanded != NULL) {
        memset(expanded, 0, sizeof(*expanded));
    }
}

void cm_expanded_item_sequence_destroy(CmExpandedItemSequence *expanded)
{
    size_t index;

    if (expanded == NULL) {
        return;
    }
    for (index = 0u; index < expanded->item_count; ++index) {
        cm_expanded_item_destroy(&expanded->items[index]);
    }
    cm_free(expanded->items);
    memset(expanded, 0, sizeof(*expanded));
}

void cm_expanded_attribute_list_init(CmExpandedAttributeList *expanded)
{
    if (expanded != NULL) memset(expanded, 0, sizeof(*expanded));
}

void cm_expanded_attribute_list_destroy(CmExpandedAttributeList *expanded)
{
    if (expanded == NULL) return;
    cm_free(expanded->attributes);
    memset(expanded, 0, sizeof(*expanded));
}

static CmExpandResult cm_expand_ok(void)
{
    CmExpandResult result;

    memset(&result, 0, sizeof(result));
    result.status = CM_MACRO_OK;
    result.diagnostic.message = "";
    result.diagnostic.cfg_diagnostic.message = "";
    return result;
}

static void cm_expand_error(CmExpandContext *context, CmMacroStatus status,
    CmExpandDiagnosticCode code, CmAstItemId item_id,
    CmAstAttributeId attribute_id, CmAstSpan span, const char *message)
{
    if (context->result.status != CM_MACRO_OK) {
        return;
    }
    context->result.status = status;
    context->result.diagnostic.code = code;
    context->result.diagnostic.item_id = item_id;
    context->result.diagnostic.attribute_id = attribute_id;
    context->result.diagnostic.span = span;
    context->result.diagnostic.message = message;
}

static int cm_expand_is_space(unsigned char byte)
{
    return byte == (unsigned char)' ' || byte == (unsigned char)'\t'
        || byte == (unsigned char)'\r' || byte == (unsigned char)'\n';
}

static int cm_expand_is_ident_start(unsigned char byte)
{
    return (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
        || (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
        || byte == (unsigned char)'_';
}

static int cm_expand_is_ident_continue(unsigned char byte)
{
    return cm_expand_is_ident_start(byte)
        || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9');
}

static CmMetaSlice cm_expand_trim(CmMetaSlice slice)
{
    while (slice.length != 0u && cm_expand_is_space(slice.bytes[0])) {
        slice.bytes += 1;
        slice.length -= 1u;
        slice.source_offset += 1u;
    }
    while (slice.length != 0u
        && cm_expand_is_space(slice.bytes[slice.length - 1u])) {
        slice.length -= 1u;
    }
    return slice;
}

static CmAstSpan cm_expand_slice_span(const CmAstAttribute *attribute,
    CmMetaSlice slice)
{
    CmAstSpan span;
    size_t start;
    size_t end;

    start = (size_t)attribute->span.start + slice.source_offset;
    end = start + slice.length;
    span.start = start > (size_t)UINT32_MAX
        ? UINT32_MAX : (uint32_t)start;
    span.end = end > (size_t)UINT32_MAX ? UINT32_MAX : (uint32_t)end;
    return span;
}

static int cm_expand_extract_meta(CmExpandContext *context,
    CmAstItemId item_id, CmAstAttributeId attribute_id,
    const CmAstAttribute *attribute, CmMetaSlice *meta)
{
    const CmInternedString *text;
    CmMetaSlice whole;
    size_t position;
    size_t close;

    text = cm_ast_get_string(context->ast, attribute->text);
    if (text == NULL) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute has no source text");
        return 0;
    }
    whole.bytes = text->bytes;
    whole.length = text->len;
    whole.source_offset = 0u;
    whole = cm_expand_trim(whole);
    position = 0u;
    if (whole.length == 0u || whole.bytes[position] != (unsigned char)'#') {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute text must start with '#'");
        return 0;
    }
    position += 1u;
    while (position < whole.length
        && cm_expand_is_space(whole.bytes[position])) {
        position += 1u;
    }
    if (position < whole.length
        && whole.bytes[position] == (unsigned char)'!') {
        if (attribute->style != CM_AST_ATTR_INNER) {
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
                attribute->span, "attribute style disagrees with its text");
            return 0;
        }
        position += 1u;
    } else if (attribute->style != CM_AST_ATTR_OUTER) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute style disagrees with its text");
        return 0;
    }
    while (position < whole.length
        && cm_expand_is_space(whole.bytes[position])) {
        position += 1u;
    }
    if (position >= whole.length
        || whole.bytes[position] != (unsigned char)'[') {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute is missing '['");
        return 0;
    }
    position += 1u;
    close = whole.length;
    while (close > position && cm_expand_is_space(whole.bytes[close - 1u])) {
        close -= 1u;
    }
    if (close == position || whole.bytes[close - 1u] != (unsigned char)']') {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute is missing closing ']'");
        return 0;
    }
    meta->bytes = whole.bytes + position;
    meta->length = (close - 1u) - position;
    meta->source_offset = whole.source_offset + position;
    *meta = cm_expand_trim(*meta);
    if (meta->length == 0u) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            attribute->span, "attribute metadata is empty");
        return 0;
    }
    return 1;
}

static int cm_expand_skip_quoted(CmMetaSlice slice, size_t *position,
    unsigned char quote)
{
    size_t cursor;

    cursor = *position + 1u;
    while (cursor < slice.length) {
        if (slice.bytes[cursor] == (unsigned char)'\\') {
            cursor += 1u;
            if (cursor < slice.length) {
                cursor += 1u;
            }
        } else if (slice.bytes[cursor] == quote) {
            *position = cursor + 1u;
            return 1;
        } else {
            cursor += 1u;
        }
    }
    return 0;
}

static int cm_expand_skip_raw_string(CmMetaSlice slice, size_t *position)
{
    size_t cursor;
    size_t hashes;
    size_t closing;
    size_t index;

    cursor = *position;
    if (cursor < slice.length && (slice.bytes[cursor] == (unsigned char)'b'
        || slice.bytes[cursor] == (unsigned char)'c')) {
        cursor += 1u;
    }
    if (cursor >= slice.length || slice.bytes[cursor] != (unsigned char)'r') {
        return 0;
    }
    cursor += 1u;
    hashes = 0u;
    while (cursor < slice.length
        && slice.bytes[cursor] == (unsigned char)'#') {
        hashes += 1u;
        cursor += 1u;
    }
    if (cursor >= slice.length || slice.bytes[cursor] != (unsigned char)'"') {
        return 0;
    }
    cursor += 1u;
    while (cursor < slice.length) {
        if (slice.bytes[cursor] != (unsigned char)'"') {
            cursor += 1u;
            continue;
        }
        closing = cursor + 1u;
        if (slice.length - closing < hashes) {
            return 0;
        }
        for (index = 0u; index < hashes; ++index) {
            if (slice.bytes[closing + index] != (unsigned char)'#') {
                break;
            }
        }
        if (index == hashes) {
            *position = closing + hashes;
            return 1;
        }
        cursor += 1u;
    }
    return 0;
}

static int cm_expand_skip_comment(CmMetaSlice slice, size_t *position)
{
    size_t cursor;
    unsigned int depth;

    cursor = *position;
    if (slice.length - cursor < 2u
        || slice.bytes[cursor] != (unsigned char)'/') {
        return 0;
    }
    if (slice.bytes[cursor + 1u] == (unsigned char)'/') {
        cursor += 2u;
        while (cursor < slice.length
            && slice.bytes[cursor] != (unsigned char)'\n') {
            cursor += 1u;
        }
        *position = cursor;
        return 1;
    }
    if (slice.bytes[cursor + 1u] != (unsigned char)'*') {
        return 0;
    }
    cursor += 2u;
    depth = 1u;
    while (cursor < slice.length && depth != 0u) {
        if (slice.length - cursor >= 2u
            && slice.bytes[cursor] == (unsigned char)'/'
            && slice.bytes[cursor + 1u] == (unsigned char)'*') {
            depth += 1u;
            cursor += 2u;
        } else if (slice.length - cursor >= 2u
            && slice.bytes[cursor] == (unsigned char)'*'
            && slice.bytes[cursor + 1u] == (unsigned char)'/') {
            depth -= 1u;
            cursor += 2u;
        } else {
            cursor += 1u;
        }
    }
    if (depth != 0u) {
        return 0;
    }
    *position = cursor;
    return 1;
}

/* Finds a comma which is not nested in delimiters or literals. */
static int cm_expand_find_comma(CmMetaSlice slice, size_t start,
    size_t *comma, int *found, int *unsupported)
{
    unsigned char closes[CM_EXPAND_DELIMITER_LIMIT];
    size_t position;
    unsigned int depth;
    unsigned char byte;

    position = start;
    depth = 0u;
    *found = 0;
    *unsupported = 0;
    while (position < slice.length) {
        byte = slice.bytes[position];
        if (cm_expand_skip_comment(slice, &position)) {
            continue;
        }
        if ((byte == (unsigned char)'r' || byte == (unsigned char)'b'
            || byte == (unsigned char)'c')
            && cm_expand_skip_raw_string(slice, &position)) {
            continue;
        }
        if (byte == (unsigned char)'"') {
            if (!cm_expand_skip_quoted(slice, &position, byte)) {
                return 0;
            }
            continue;
        }
        if (byte == (unsigned char)'\''
            && cm_expand_skip_quoted(slice, &position, byte)) {
            continue;
        }
        if (byte == (unsigned char)'(' || byte == (unsigned char)'['
            || byte == (unsigned char)'{') {
            if (depth >= CM_EXPAND_DELIMITER_LIMIT) {
                *unsupported = 1;
                return 0;
            }
            closes[depth] = byte == (unsigned char)'('
                ? (unsigned char)')' : (byte == (unsigned char)'['
                    ? (unsigned char)']' : (unsigned char)'}');
            depth += 1u;
            position += 1u;
            continue;
        }
        if (byte == (unsigned char)')' || byte == (unsigned char)']'
            || byte == (unsigned char)'}') {
            if (depth == 0u || closes[depth - 1u] != byte) {
                return 0;
            }
            depth -= 1u;
            position += 1u;
            continue;
        }
        if (byte == (unsigned char)',' && depth == 0u) {
            *comma = position;
            *found = 1;
            return 1;
        }
        position += 1u;
    }
    return depth == 0u;
}

static int cm_expand_attribute_head(CmMetaSlice meta,
    size_t *name_length, size_t *arguments_start,
    CmMetaSlice *arguments, int *has_arguments)
{
    size_t position;
    size_t close;
    unsigned int depth;
    unsigned char byte;

    position = 0u;
    /* A path-headed attribute (`#[::core::prelude::v1::derive(..)]`, as
     * libc's `s!` emits): the head is the whole path. */
    if (position + 1u < meta.length && meta.bytes[position] == '\x3a'
        && meta.bytes[position + 1u] == '\x3a') {
        position += 2u;
        while (position < meta.length
            && cm_expand_is_space(meta.bytes[position])) position += 1u;
    }
    for (;;) {
        size_t probe;

        if (position >= meta.length
            || !cm_expand_is_ident_start(meta.bytes[position])) {
            return 0;
        }
        position += 1u;
        while (position < meta.length
            && cm_expand_is_ident_continue(meta.bytes[position])) {
            position += 1u;
        }
        /* Generated text is token-spaced: `:: core :: prelude :: v1`. */
        probe = position;
        while (probe < meta.length && cm_expand_is_space(meta.bytes[probe]))
            probe += 1u;
        if (probe + 1u < meta.length && meta.bytes[probe] == '\x3a'
            && meta.bytes[probe + 1u] == '\x3a') {
            position = probe + 2u;
            while (position < meta.length
                && cm_expand_is_space(meta.bytes[position])) position += 1u;
            continue;
        }
        break;
    }
    *name_length = position;
    while (position < meta.length && cm_expand_is_space(meta.bytes[position])) {
        position += 1u;
    }
    *has_arguments = 0;
    if (position == meta.length) {
        return 1;
    }
    if (meta.bytes[position] != (unsigned char)'(') {
        return 1;
    }
    *has_arguments = 1;
    *arguments_start = position + 1u;
    depth = 1u;
    close = position + 1u;
    while (close < meta.length && depth != 0u) {
        byte = meta.bytes[close];
        if ((byte == (unsigned char)'r' || byte == (unsigned char)'b'
            || byte == (unsigned char)'c')
            && cm_expand_skip_raw_string(meta, &close)) {
            continue;
        }
        if (byte == (unsigned char)'"') {
            if (!cm_expand_skip_quoted(meta, &close, byte)) {
                return 0;
            }
            continue;
        }
        if (byte == (unsigned char)'(') {
            depth += 1u;
        } else if (byte == (unsigned char)')') {
            depth -= 1u;
            if (depth == 0u) {
                break;
            }
        }
        close += 1u;
    }
    if (depth != 0u) {
        return 0;
    }
    arguments->bytes = meta.bytes + *arguments_start;
    arguments->length = close - *arguments_start;
    arguments->source_offset = meta.source_offset + *arguments_start;
    close += 1u;
    while (close < meta.length && cm_expand_is_space(meta.bytes[close])) {
        close += 1u;
    }
    return close == meta.length;
}

static int cm_expand_name_is(CmMetaSlice meta, size_t name_length,
    const char *name)
{
    size_t length;

    length = strlen(name);
    return name_length == length
        && memcmp(meta.bytes, name, length) == 0;
}

static int cm_expand_emit_attribute(CmVec *attributes,
    CmAstAttributeId attribute_id,
    const CmAstAttribute *attribute, CmMetaSlice meta,
    unsigned int expansion_depth)
{
    CmEffectiveAttribute effective;

    effective.source_id = attribute_id;
    effective.style = attribute->style;
    effective.span = cm_expand_slice_span(attribute, meta);
    effective.meta = meta.bytes;
    effective.meta_length = meta.length;
    effective.expansion_depth = expansion_depth;
    (void)cm_vec_push(attributes, &effective);
    return 1;
}

static int cm_expand_process_meta(CmExpandContext *context,
    CmVec *attributes, CmAstItemId item_id,
    CmAstAttributeId attribute_id, const CmAstAttribute *attribute,
    CmMetaSlice meta, unsigned int expansion_depth, int *active);

static int cm_expand_process_cfg_attr(CmExpandContext *context,
    CmVec *attributes, CmAstItemId item_id,
    CmAstAttributeId attribute_id, const CmAstAttribute *attribute,
    CmMetaSlice arguments, unsigned int expansion_depth, int *active)
{
    size_t comma;
    size_t position;
    int found;
    int unsupported;
    CmMetaSlice predicate;
    CmMetaSlice payload;
    CmCfgEvaluation evaluation;

    if (expansion_depth >= context->options->maximum_nesting) {
        cm_expand_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_EXPAND_DIAG_NESTING_LIMIT, item_id, attribute_id,
            cm_expand_slice_span(attribute, arguments),
            "cfg_attr nesting limit exceeded");
        return 0;
    }
    if (!cm_expand_find_comma(arguments, 0u, &comma, &found,
        &unsupported)) {
        cm_expand_error(context,
            unsupported ? CM_MACRO_LIMIT_EXCEEDED : CM_MACRO_SYNTAX_ERROR,
            unsupported ? CM_EXPAND_DIAG_NESTING_LIMIT
                : CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE,
            item_id, attribute_id, cm_expand_slice_span(attribute, arguments),
            unsupported ? "attribute delimiter nesting limit exceeded"
                : "malformed delimiters in cfg_attr");
        return 0;
    }
    if (!found) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            cm_expand_slice_span(attribute, arguments),
            "cfg_attr requires a predicate and attribute payload");
        return 0;
    }
    predicate.bytes = arguments.bytes;
    predicate.length = comma;
    predicate.source_offset = arguments.source_offset;
    predicate = cm_expand_trim(predicate);
    if (predicate.length == 0u) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            cm_expand_slice_span(attribute, predicate),
            "cfg_attr predicate is empty");
        return 0;
    }
    evaluation = cm_cfg_attr_decide(&context->options->cfg->environment,
        (const char *)predicate.bytes, predicate.length);
    if (evaluation.status != CM_MACRO_OK) {
        cm_expand_error(context, evaluation.status,
            CM_EXPAND_DIAG_CFG_PREDICATE, item_id, attribute_id,
            cm_expand_slice_span(attribute, predicate),
            "invalid cfg_attr predicate");
        context->result.diagnostic.cfg_diagnostic = evaluation.diagnostic;
        return 0;
    }
    position = comma + 1u;
    found = 0;
    while (position <= arguments.length) {
        size_t next;
        int has_comma;
        size_t remaining;

        remaining = position;
        while (remaining < arguments.length
            && cm_expand_is_space(arguments.bytes[remaining])) {
            remaining += 1u;
        }
        if (remaining == arguments.length) {
            if (found) {
                break;
            }
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
                cm_expand_slice_span(attribute, arguments),
                "cfg_attr has no attribute payload");
            return 0;
        }

        if (!cm_expand_find_comma(arguments, position, &next, &has_comma,
            &unsupported)) {
            cm_expand_error(context,
                unsupported ? CM_MACRO_LIMIT_EXCEEDED
                    : CM_MACRO_SYNTAX_ERROR,
                unsupported ? CM_EXPAND_DIAG_NESTING_LIMIT
                    : CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE,
                item_id, attribute_id,
                cm_expand_slice_span(attribute, arguments),
                unsupported ? "attribute delimiter nesting limit exceeded"
                    : "malformed cfg_attr payload delimiters");
            return 0;
        }
        if (!has_comma) {
            next = arguments.length;
        }
        payload.bytes = arguments.bytes + position;
        payload.length = next - position;
        payload.source_offset = arguments.source_offset + position;
        payload = cm_expand_trim(payload);
        if (payload.length == 0u) {
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
                cm_expand_slice_span(attribute, payload),
                "cfg_attr contains an empty attribute payload");
            return 0;
        }
        found = 1;
        if (evaluation.value
            && !cm_expand_process_meta(context, attributes, item_id,
                attribute_id, attribute, payload, expansion_depth + 1u,
                active)) {
            return 0;
        }
        if (!has_comma) {
            break;
        }
        position = next + 1u;
    }
    if (!found) {
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
            cm_expand_slice_span(attribute, arguments),
            "cfg_attr has no attribute payload");
        return 0;
    }
    return 1;
}

static int cm_expand_process_meta(CmExpandContext *context,
    CmVec *attributes, CmAstItemId item_id,
    CmAstAttributeId attribute_id, const CmAstAttribute *attribute,
    CmMetaSlice meta, unsigned int expansion_depth, int *active)
{
    size_t name_length;
    size_t arguments_start;
    CmMetaSlice arguments;
    int has_arguments;
    CmCfgEvaluation evaluation;

    meta = cm_expand_trim(meta);
    memset(&arguments, 0, sizeof(arguments));
    if (context->attribute_expansions
        >= context->options->maximum_attribute_expansions) {
        cm_expand_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_EXPAND_DIAG_ATTRIBUTE_LIMIT, item_id, attribute_id,
            cm_expand_slice_span(attribute, meta),
            "attribute expansion limit exceeded");
        return 0;
    }
    context->attribute_expansions += 1u;
    if (meta.length == 0u || !cm_expand_attribute_head(meta, &name_length,
        &arguments_start, &arguments, &has_arguments)) {
        if (getenv("CM_MACRO_DEBUG") != NULL) {
            fprintf(stderr, "MACRO attribute head rejected: [%.*s]\n",
                (int)(meta.length > 300u ? 300u : meta.length),
                (const char *)meta.bytes);
        }
        cm_expand_error(context, CM_MACRO_UNSUPPORTED,
            CM_EXPAND_DIAG_UNSUPPORTED_ATTRIBUTE, item_id, attribute_id,
            cm_expand_slice_span(attribute, meta),
            "unsupported or malformed attribute metadata");
        return 0;
    }
    if (cm_expand_name_is(meta, name_length, "cfg")) {
        if (!has_arguments) {
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
                cm_expand_slice_span(attribute, meta),
                "cfg attribute requires parenthesized predicate");
            return 0;
        }
        evaluation = cm_cfg_evaluate(&context->options->cfg->environment,
            (const char *)arguments.bytes, arguments.length);
        if (evaluation.status != CM_MACRO_OK) {
            cm_expand_error(context, evaluation.status,
                CM_EXPAND_DIAG_CFG_PREDICATE, item_id, attribute_id,
                cm_expand_slice_span(attribute, arguments),
                "invalid cfg predicate");
            context->result.diagnostic.cfg_diagnostic =
                evaluation.diagnostic;
            return 0;
        }
        if (!evaluation.value) {
            *active = 0;
        }
        return 1;
    }
    if (cm_expand_name_is(meta, name_length, "cfg_attr")) {
        if (!has_arguments) {
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE, item_id, attribute_id,
                cm_expand_slice_span(attribute, meta),
                "cfg_attr requires parenthesized arguments");
            return 0;
        }
        return cm_expand_process_cfg_attr(context, attributes, item_id,
            attribute_id, attribute, arguments, expansion_depth, active);
    }
    return cm_expand_emit_attribute(attributes, attribute_id, attribute,
        meta, expansion_depth);
}

static int cm_expand_attribute_list(CmExpandContext *context,
    CmAstItemId item_id, const CmAstAttributeId *ids, size_t count,
    CmEffectiveAttribute **attributes_out, size_t *count_out, int *active)
{
    CmVec attributes;
    size_t index;
    const CmAstAttribute *attribute;
    CmMetaSlice meta;

    cm_vec_init(&attributes, sizeof(CmEffectiveAttribute));
    *active = 1;
    if (count != 0u && ids == NULL) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_INVALID_ATTRIBUTE_ID, item_id, 0u, span,
            "nonempty attribute list has no AST IDs");
        cm_vec_destroy(&attributes);
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        attribute = cm_ast_get_attribute(context->ast, ids[index]);
        if (attribute == NULL) {
            CmAstSpan span;

            memset(&span, 0, sizeof(span));
            cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
                CM_EXPAND_DIAG_INVALID_ATTRIBUTE_ID, item_id, ids[index],
                span, "attribute list contains an invalid AST ID");
            cm_vec_destroy(&attributes);
            return 0;
        }
        if (!cm_expand_extract_meta(context, item_id, ids[index], attribute,
            &meta) || !cm_expand_process_meta(context, &attributes, item_id,
                ids[index], attribute, meta, 0u, active)) {
            cm_vec_destroy(&attributes);
            return 0;
        }
    }
    if (attributes.len != 0u) {
        *attributes_out = (CmEffectiveAttribute *)cm_alloc(
            attributes.len * sizeof(CmEffectiveAttribute));
        memcpy(*attributes_out, attributes.data,
            attributes.len * sizeof(CmEffectiveAttribute));
    }
    *count_out = attributes.len;
    cm_vec_destroy(&attributes);
    return 1;
}

static void cm_expand_item_children(const CmAstItem *item,
    const CmAstItemId **ids, size_t *count, CmExpandedChildKind *kind)
{
    *ids = NULL;
    *count = 0u;
    *kind = CM_EXPANDED_CHILD_NONE;
    switch (item->kind) {
    case CM_AST_ITEM_MODULE:
        *ids = item->data.module_item.items;
        *count = (size_t)item->data.module_item.item_count;
        *kind = CM_EXPANDED_CHILD_MODULE;
        break;
    case CM_AST_ITEM_EXTERN_BLOCK:
        *ids = item->data.extern_block_item.items;
        *count = (size_t)item->data.extern_block_item.item_count;
        *kind = CM_EXPANDED_CHILD_EXTERN_BLOCK;
        break;
    case CM_AST_ITEM_TRAIT:
        *ids = item->data.trait_item.items;
        *count = (size_t)item->data.trait_item.item_count;
        *kind = CM_EXPANDED_CHILD_TRAIT;
        break;
    case CM_AST_ITEM_IMPL:
        *ids = item->data.impl_item.items;
        *count = (size_t)item->data.impl_item.item_count;
        *kind = CM_EXPANDED_CHILD_IMPL;
        break;
    default:
        break;
    }
}

static int cm_expand_item(CmExpandContext *context, CmAstItemId item_id,
    unsigned int depth, CmExpandedItem *expanded, int *is_active);

static int cm_expand_items(CmExpandContext *context,
    const CmAstItemId *ids, size_t count, unsigned int depth,
    CmExpandedItem **items_out, size_t *count_out)
{
    CmExpandedItem *items;
    size_t index;
    size_t active_count;
    int active;

    *items_out = NULL;
    *count_out = 0u;
    if (count == 0u) {
        return 1;
    }
    if (ids == NULL) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_INVALID_ITEM_ID, 0u, 0u, span,
            "nonempty item list has no AST IDs");
        return 0;
    }
    if (context->items_seen > context->options->maximum_items
        || count > context->options->maximum_items - context->items_seen) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_EXPAND_DIAG_ITEM_LIMIT, ids[0], 0u, span,
            "item expansion limit exceeded");
        return 0;
    }
    items = (CmExpandedItem *)cm_alloc_zeroed(count,
        sizeof(CmExpandedItem));
    active_count = 0u;
    for (index = 0u; index < count; ++index) {
        if (!cm_expand_item(context, ids[index], depth,
            &items[active_count], &active)) {
            size_t destroy_index;

            for (destroy_index = 0u; destroy_index < active_count;
                ++destroy_index) {
                cm_expanded_item_destroy(&items[destroy_index]);
            }
            cm_free(items);
            return 0;
        }
        if (active) {
            active_count += 1u;
        }
    }
    if (active_count == 0u) {
        cm_free(items);
        items = NULL;
    }
    *items_out = items;
    *count_out = active_count;
    return 1;
}

static int cm_expand_item(CmExpandContext *context, CmAstItemId item_id,
    unsigned int depth, CmExpandedItem *expanded, int *is_active)
{
    const CmAstItem *item;
    const CmAstItemId *child_ids;
    size_t child_count;
    int inner_active;

    memset(expanded, 0, sizeof(*expanded));
    *is_active = 0;
    if (depth >= context->options->maximum_nesting) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_EXPAND_DIAG_NESTING_LIMIT, item_id, 0u, span,
            "item nesting limit exceeded");
        return 0;
    }
    if (context->items_seen >= context->options->maximum_items) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_LIMIT_EXCEEDED,
            CM_EXPAND_DIAG_ITEM_LIMIT, item_id, 0u, span,
            "item expansion limit exceeded");
        return 0;
    }
    context->items_seen += 1u;
    item = cm_ast_get_item(context->ast, item_id);
    if (item == NULL) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(context, CM_MACRO_SYNTAX_ERROR,
            CM_EXPAND_DIAG_INVALID_ITEM_ID, item_id, 0u, span,
            "item list contains an invalid AST ID");
        return 0;
    }
    expanded->source_id = item_id;
    expanded->span = item->span;
    if (!cm_expand_attribute_list(context, item_id, item->attributes,
        (size_t)item->attribute_count, &expanded->attributes,
        &expanded->attribute_count, is_active)) {
        return 0;
    }
    if (!*is_active) {
        cm_free(expanded->attributes);
        memset(expanded, 0, sizeof(*expanded));
        return 1;
    }
    if (item->kind == CM_AST_ITEM_MODULE
        && item->data.module_item.is_inline) {
        inner_active = 0;
        if (!cm_expand_attribute_list(context, item_id,
                item->data.module_item.inner_attributes,
                (size_t)item->data.module_item.inner_attribute_count,
                &expanded->inner_attributes,
                &expanded->inner_attribute_count, &inner_active)) {
            cm_expanded_item_destroy(expanded);
            return 0;
        }
        if (!inner_active) {
            cm_expanded_item_destroy(expanded);
            *is_active = 0;
            return 1;
        }
    }
    cm_expand_item_children(item, &child_ids, &child_count,
        &expanded->child_kind);
    if (!cm_expand_items(context, child_ids, child_count, depth + 1u,
        &expanded->children, &expanded->child_count)) {
        cm_expanded_item_destroy(expanded);
        return 0;
    }
    return 1;
}

CmExpandResult cm_expand_cfg_view(const CmAst *ast,
    const CmExpandOptions *options, CmExpandedAst *expanded)
{
    CmExpandContext context;
    int crate_active;

    if (expanded != NULL) {
        cm_expanded_ast_destroy(expanded);
        cm_expanded_ast_init(expanded);
    }
    memset(&context, 0, sizeof(context));
    context.result = cm_expand_ok();
    if (ast == NULL || options == NULL || expanded == NULL
        || options->cfg == NULL || options->maximum_nesting == 0u
        || options->maximum_items == 0u
        || options->maximum_attribute_expansions == 0u) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(&context, CM_MACRO_INVALID_ARGUMENT,
            CM_EXPAND_DIAG_INVALID_ARGUMENT, 0u, 0u, span,
            "invalid cfg expansion argument or zero limit");
        return context.result;
    }
    context.ast = ast;
    context.options = options;
    if (!cm_expand_attribute_list(&context, 0u,
        (const CmAstAttributeId *)ast->crate_attributes.data,
        ast->crate_attributes.len, &expanded->crate_attributes,
        &expanded->crate_attribute_count, &crate_active)) {
        cm_expanded_ast_destroy(expanded);
        return context.result;
    }
    expanded->crate_is_active = crate_active;
    if (crate_active && !cm_expand_items(&context,
        (const CmAstItemId *)ast->root_items.data, ast->root_items.len, 0u,
        &expanded->root_items, &expanded->root_item_count)) {
        cm_expanded_ast_destroy(expanded);
        return context.result;
    }
    return context.result;
}

CmExpandResult cm_expand_cfg_item_sequence(const CmAst *ast,
    const CmAstItemId *item_ids, size_t item_count,
    const CmExpandOptions *options, CmExpandedItemSequence *expanded)
{
    CmExpandContext context;

    if (expanded != NULL) {
        cm_expanded_item_sequence_destroy(expanded);
        cm_expanded_item_sequence_init(expanded);
    }
    memset(&context, 0, sizeof(context));
    context.result = cm_expand_ok();
    if (ast == NULL || options == NULL || expanded == NULL
        || options->cfg == NULL || options->maximum_nesting == 0u
        || options->maximum_items == 0u
        || options->maximum_attribute_expansions == 0u
        || (item_count != 0u && item_ids == NULL)) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(&context, CM_MACRO_INVALID_ARGUMENT,
            CM_EXPAND_DIAG_INVALID_ARGUMENT, 0u, 0u, span,
            "invalid cfg item-sequence argument or zero limit");
        return context.result;
    }
    context.ast = ast;
    context.options = options;
    if (!cm_expand_items(&context, item_ids, item_count, 0u,
        &expanded->items, &expanded->item_count)) {
        cm_expanded_item_sequence_destroy(expanded);
    }
    return context.result;
}

CmExpandResult cm_expand_cfg_attribute_list(const CmAst *ast,
    CmAstItemId diagnostic_owner, const CmAstAttributeId *attribute_ids,
    size_t attribute_count, const CmExpandOptions *options,
    CmExpandedAttributeList *expanded)
{
    CmExpandContext context;

    if (expanded != NULL) {
        cm_expanded_attribute_list_destroy(expanded);
        cm_expanded_attribute_list_init(expanded);
    }
    memset(&context, 0, sizeof(context));
    context.result = cm_expand_ok();
    if (ast == NULL || options == NULL || expanded == NULL
        || options->cfg == NULL || options->maximum_nesting == 0u
        || options->maximum_items == 0u
        || options->maximum_attribute_expansions == 0u
        || (attribute_count != 0u && attribute_ids == NULL)) {
        CmAstSpan span;

        memset(&span, 0, sizeof(span));
        cm_expand_error(&context, CM_MACRO_INVALID_ARGUMENT,
            CM_EXPAND_DIAG_INVALID_ARGUMENT, diagnostic_owner, 0u, span,
            "invalid cfg attribute-list argument or zero limit");
        return context.result;
    }
    context.ast = ast;
    context.options = options;
    if (!cm_expand_attribute_list(&context, diagnostic_owner,
            attribute_ids, attribute_count, &expanded->attributes,
            &expanded->attribute_count, &expanded->is_active)) {
        cm_expanded_attribute_list_destroy(expanded);
    }
    return context.result;
}

const char *cm_expand_diagnostic_code_name(CmExpandDiagnosticCode code)
{
    switch (code) {
    case CM_EXPAND_DIAG_NONE:
        return "none";
    case CM_EXPAND_DIAG_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_EXPAND_DIAG_INVALID_ITEM_ID:
        return "invalid-item-id";
    case CM_EXPAND_DIAG_INVALID_ATTRIBUTE_ID:
        return "invalid-attribute-id";
    case CM_EXPAND_DIAG_MALFORMED_ATTRIBUTE:
        return "malformed-attribute";
    case CM_EXPAND_DIAG_UNSUPPORTED_ATTRIBUTE:
        return "unsupported-attribute";
    case CM_EXPAND_DIAG_CFG_PREDICATE:
        return "cfg-predicate";
    case CM_EXPAND_DIAG_NESTING_LIMIT:
        return "nesting-limit";
    case CM_EXPAND_DIAG_ITEM_LIMIT:
        return "item-limit";
    case CM_EXPAND_DIAG_ATTRIBUTE_LIMIT:
        return "attribute-limit";
    }
    return "unknown";
}
