#include "cm/resolve/body_expand.h"
#include "cm/alloc.h"
#include "cm/buf.h"
#include "cm/macro/ast_builtin.h"
#include "cm/macro/syntax_adapter.h"
#include "cm/syntax/parser.h"
#include "cm/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lenient expression-position macro expansion (M9-01).  See body_expand.h.
 *
 * The walker visits every cfg-active item of every module, descends into
 * function bodies, const/static initializers, impl/trait children, and
 * items nested inside bodies, and expands each `CM_AST_EXPR_MACRO` node in
 * place.  Expansion appends new nodes to the same AST and overwrites the
 * invocation node with the parsed result, so every existing expression ID
 * (including the body roots recorded by HIR lowering) stays valid.
 */

#define CM_BODY_LOCAL_MACRO_LIMIT 256u
#define CM_BODY_NAME_LIMIT 128u

typedef struct CmBodyLocalMacro {
    char name[CM_BODY_NAME_LIMIT];
    CmAstItemId item;
} CmBodyLocalMacro;

typedef struct CmBodyExpandState {
    CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    const CmBodyExpandOptions *options;
    CmBodyExpandResult result;
    CmAst *ast;
    CmModuleId module;
    CmBodyLocalMacro locals[CM_BODY_LOCAL_MACRO_LIMIT];
    size_t local_count;
    CmStrBuf text;
    CmStrBuf scratch;
    CmStrBuf pieces;
} CmBodyExpandState;

typedef enum CmBodyBuiltin {
    CM_BODY_BUILTIN_NONE = 0,
    CM_BODY_BUILTIN_AST,          /* handled by cm_builtin_ast_expand_expression */
    CM_BODY_BUILTIN_ASSERT,
    CM_BODY_BUILTIN_PANIC,
    CM_BODY_BUILTIN_UNREACHABLE,
    CM_BODY_BUILTIN_FORMAT_ARGS,
    CM_BODY_BUILTIN_ASM,
    CM_BODY_BUILTIN_UNSUPPORTED
} CmBodyBuiltin;

void cm_body_expand_options_init(CmBodyExpandOptions *options)
{
    memset(options, 0, sizeof(*options));
    options->edition = CM_EDITION_2024;
    options->crate_identifier = "crate";
    options->maximum_depth = CM_BODY_EXPAND_DEFAULT_MAX_DEPTH;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */

static void cm_body_fail(CmBodyExpandState *state, const char *name,
    CmAstSpan span, const char *reason)
{
    size_t index;
    state->result.failed += 1u;
    for (index = 0u; index < state->result.failure_class_count; ++index) {
        if (strncmp(state->result.failure_classes[index].macro_name,
                name == NULL ? "?" : name, 63u) == 0
            && strncmp(state->result.failure_classes[index].reason, reason,
                127u) == 0) {
            state->result.failure_classes[index].count += 1u;
            break;
        }
    }
    if (index == state->result.failure_class_count
        && index < CM_BODY_EXPAND_FAILURE_CLASSES) {
        strncpy(state->result.failure_classes[index].macro_name,
            name == NULL ? "?" : name, 63u);
        strncpy(state->result.failure_classes[index].reason, reason, 127u);
        state->result.failure_classes[index].count = 1u;
        state->result.failure_class_count += 1u;
    }
    if (state->result.first_failure_reason[0] != '\0') return;
    strncpy(state->result.first_failure_macro, name == NULL ? "?" : name,
        sizeof(state->result.first_failure_macro) - 1u);
    strncpy(state->result.first_failure_reason, reason,
        sizeof(state->result.first_failure_reason) - 1u);
    state->result.first_failure_span.source = 0u;
    state->result.first_failure_span.start = span.start;
    state->result.first_failure_span.end = span.end;
}

/* ------------------------------------------------------------------ */
/* Small text helpers                                                   */

static int cm_body_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int cm_body_is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int cm_body_is_ident_continue(char c)
{
    return cm_body_is_ident_start(c) || (c >= '0' && c <= '9');
}

static void cm_body_trim(const char **text, size_t *length)
{
    while (*length != 0u && cm_body_is_space((*text)[0])) {
        *text += 1;
        *length -= 1u;
    }
    while (*length != 0u && cm_body_is_space((*text)[*length - 1u]))
        *length -= 1u;
}

/*
 * Advance over one string, raw string, byte string, or char literal
 * starting at `text[index]`; returns the index just past it, or `index`
 * when no literal starts there.
 */
static size_t cm_body_skip_literal(const char *text, size_t length,
    size_t index)
{
    size_t position = index;
    size_t hashes = 0u;
    if (position < length && (text[position] == 'b' || text[position] == 'c')
        && position + 1u < length
        && (text[position + 1u] == '"' || text[position + 1u] == '\''
            || text[position + 1u] == 'r'))
        position += 1u;
    if (position < length && text[position] == 'r') {
        size_t probe = position + 1u;
        while (probe < length && text[probe] == '#') {
            hashes += 1u;
            probe += 1u;
        }
        if (probe < length && text[probe] == '"') {
            position = probe + 1u;
            for (;;) {
                if (position >= length) return length;
                if (text[position] == '"') {
                    size_t seen = 0u;
                    size_t look = position + 1u;
                    while (seen < hashes && look < length
                        && text[look] == '#') {
                        seen += 1u;
                        look += 1u;
                    }
                    if (seen == hashes) return look;
                }
                position += 1u;
            }
        }
        return index;
    }
    if (position < length && text[position] == '"') {
        position += 1u;
        while (position < length && text[position] != '"') {
            if (text[position] == '\\') position += 1u;
            position += 1u;
        }
        return position < length ? position + 1u : length;
    }
    if (position < length && text[position] == '\'') {
        /* Char literal when a closing quote follows one (escaped) char;
         * otherwise a lifetime or label. */
        if (position + 2u < length && text[position + 1u] == '\\') {
            size_t probe = position + 2u;
            while (probe < length && text[probe] != '\'') probe += 1u;
            return probe < length ? probe + 1u : length;
        }
        if (position + 2u < length && text[position + 2u] == '\'')
            return position + 3u;
        if (position + 1u < length && ((unsigned char)text[position + 1u]
                & 0x80u) != 0u) {
            /* Multi-byte UTF-8 char literal. */
            size_t probe = position + 1u;
            while (probe < length && text[probe] != '\'' && probe
                < position + 6u) probe += 1u;
            if (probe < length && text[probe] == '\'') return probe + 1u;
        }
        return index + 1u;
    }
    return index;
}

/*
 * Split `text` at top-level commas.  `out_starts`/`out_lengths` receive up
 * to `limit` trimmed pieces; returns the piece count (which may exceed the
 * limit).  A trailing comma does not add an empty piece.
 */
static size_t cm_body_split_commas(const char *text, size_t length,
    size_t *out_starts, size_t *out_lengths, size_t limit)
{
    size_t depth = 0u;
    size_t index = 0u;
    size_t piece_start = 0u;
    size_t count = 0u;
    while (index < length) {
        size_t skipped = cm_body_skip_literal(text, length, index);
        char c;
        if (skipped != index) {
            index = skipped;
            continue;
        }
        c = text[index];
        if (c == '(' || c == '[' || c == '{') depth += 1u;
        else if ((c == ')' || c == ']' || c == '}') && depth != 0u)
            depth -= 1u;
        else if (c == ',' && depth == 0u) {
            const char *piece = text + piece_start;
            size_t piece_length = index - piece_start;
            cm_body_trim(&piece, &piece_length);
            if (count < limit) {
                out_starts[count] = (size_t)(piece - text);
                out_lengths[count] = piece_length;
            }
            count += 1u;
            piece_start = index + 1u;
        }
        index += 1u;
    }
    {
        const char *piece = text + piece_start;
        size_t piece_length = length - piece_start;
        cm_body_trim(&piece, &piece_length);
        if (piece_length != 0u) {
            if (count < limit) {
                out_starts[count] = (size_t)(piece - text);
                out_lengths[count] = piece_length;
            }
            count += 1u;
        }
    }
    return count;
}

static void cm_body_append_escaped(CmStrBuf *buffer, const char *text,
    size_t length)
{
    size_t index;
    for (index = 0u; index < length; ++index) {
        char c = text[index];
        if (c == '"' || c == '\\') {
            cm_str_buf_push(buffer, '\\');
            cm_str_buf_push(buffer, c);
        } else if (c == '\n') {
            cm_str_buf_append(buffer, "\\n");
        } else if (c == '\r') {
            cm_str_buf_append(buffer, "\\r");
        } else if (c == '\t') {
            cm_str_buf_append(buffer, "\\t");
        } else {
            cm_str_buf_push(buffer, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Literal text evaluation (`concat!`, `stringify!`)                    */

static int cm_body_text_starts_with_macro(const char *text, size_t length,
    const char *name, size_t *out_open)
{
    size_t name_length = strlen(name);
    size_t index = name_length;
    if (length < name_length + 2u || memcmp(text, name, name_length) != 0)
        return 0;
    while (index < length && cm_body_is_space(text[index])) index += 1u;
    if (index >= length || text[index] != '!') return 0;
    index += 1u;
    while (index < length && cm_body_is_space(text[index])) index += 1u;
    if (index >= length || (text[index] != '(' && text[index] != '['
            && text[index] != '{')) return 0;
    *out_open = index;
    return 1;
}

/*
 * Append the *contents* (between the quotes, escapes preserved) of one
 * literal-producing expression to `out`: a string literal, a raw string,
 * another literal token (numbers, chars, bools), `stringify!(...)`, or a
 * nested `concat!(...)`.  Returns 0 for anything else.
 */
static int cm_body_literal_contents(const char *text, size_t length,
    CmStrBuf *out, unsigned int depth)
{
    size_t open;
    cm_body_trim(&text, &length);
    if (length == 0u || depth > 16u) return 0;
    if (text[0] == '"') {
        if (length < 2u || text[length - 1u] != '"') return 0;
        cm_str_buf_append_n(out, text + 1, length - 2u);
        return 1;
    }
    if (text[0] == 'r' && length >= 2u && (text[1] == '"' || text[1] == '#')) {
        size_t hashes = 0u;
        size_t start;
        while (1u + hashes < length && text[1u + hashes] == '#') hashes += 1u;
        start = 1u + hashes;
        if (start >= length || text[start] != '"'
            || length < start + 2u + hashes
            || text[length - 1u - hashes] != '"') return 0;
        /* Raw contents contain no escapes; re-escape for a normal literal. */
        cm_body_append_escaped(out, text + start + 1u,
            length - start - 2u - hashes);
        return 1;
    }
    if (cm_body_text_starts_with_macro(text, length, "stringify", &open)) {
        const char *inner = text + open + 1u;
        size_t inner_length = length - open - 2u;
        if (length < open + 2u) return 0;
        cm_body_trim(&inner, &inner_length);
        cm_body_append_escaped(out, inner, inner_length);
        return 1;
    }
    if (cm_body_text_starts_with_macro(text, length, "concat", &open)) {
        const char *inner = text + open + 1u;
        size_t inner_length = length - open - 2u;
        size_t starts[64];
        size_t lengths[64];
        size_t count;
        size_t index;
        if (length < open + 2u) return 0;
        count = cm_body_split_commas(inner, inner_length, starts, lengths,
            64u);
        if (count > 64u) return 0;
        for (index = 0u; index < count; ++index)
            if (!cm_body_literal_contents(inner + starts[index],
                    lengths[index], out, depth + 1u)) return 0;
        return 1;
    }
    if (text[0] == '\'' ) {
        /* Char literal: contents without quotes. */
        if (length < 3u || text[length - 1u] != '\'') return 0;
        cm_str_buf_append_n(out, text + 1, length - 2u);
        return 1;
    }
    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-'
        || (length == 4u && memcmp(text, "true", 4u) == 0)
        || (length == 5u && memcmp(text, "false", 5u) == 0)) {
        /* Numeric literal: drop an integer/float suffix. */
        size_t end = length;
        if (text[0] >= '0' && text[0] <= '9') {
            size_t probe = 0u;
            while (probe < length && ((text[probe] >= '0' && text[probe] <= '9')
                || text[probe] == '.' || text[probe] == '_'
                || text[probe] == 'x' || text[probe] == 'o'
                || text[probe] == 'b' || (text[probe] >= 'a'
                    && text[probe] <= 'f' && text[1] == 'x')
                || (text[probe] >= 'A' && text[probe] <= 'F'
                    && text[1] == 'x'))) probe += 1u;
            end = probe;
            if (end != 0u && text[end - 1u] == '_') end -= 1u;
        }
        cm_str_buf_append_n(out, text, end);
        return 1;
    }
    return 0;
}

/* Produce a `"..."` literal in `out` from a literal-producing expression. */
static int cm_body_literal_text(const char *text, size_t length,
    CmStrBuf *out)
{
    cm_str_buf_clear(out);
    cm_str_buf_push(out, '"');
    if (!cm_body_literal_contents(text, length, out, 0u)) return 0;
    cm_str_buf_push(out, '"');
    return 1;
}

/* ------------------------------------------------------------------ */
/* Names and lookup                                                     */

static int cm_body_copy_name(const CmBodyExpandState *state,
    CmResolveStringId id, char *buffer, size_t size)
{
    return cm_module_graph_copy_string(state->graph, id, buffer, size);
}

static int cm_body_intern_text(const CmAst *ast, CmInternId id,
    const char **out_text, size_t *out_length)
{
    const CmInternedString *string = cm_ast_get_string(ast, id);
    if (string == NULL) return 0;
    *out_text = (const char *)string->bytes;
    *out_length = string->len;
    return 1;
}

static int cm_body_segment_name(const CmAst *ast, const CmAstPath *path,
    uint32_t segment, char *buffer, size_t size)
{
    const char *text;
    size_t length;
    if (path == NULL || segment >= path->segment_count
        || path->segments == NULL
        || !cm_body_intern_text(ast, path->segments[segment].name, &text,
            &length) || length >= size) return 0;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    return 1;
}

static CmBodyBuiltin cm_body_builtin_by_name(const char *name)
{
    static const char *const ast_builtins[] = {
        "cfg", "stringify", "concat", "line", "column", "file",
        "module_path", "env", "option_env"
    };
    static const char *const unsupported[] = {
        "cfg_select", "offset_of", "include_str", "include",
        "include_bytes", "compile_error", "concat_idents", "trace_macros",
        "log_syntax", "global_asm", "naked_asm", "const_format_args_nl",
        "format_args_nl", "type_ascribe"
    };
    size_t index;
    for (index = 0u; index < sizeof(ast_builtins) / sizeof(ast_builtins[0]);
            ++index)
        if (strcmp(name, ast_builtins[index]) == 0) return CM_BODY_BUILTIN_AST;
    if (strcmp(name, "assert") == 0) return CM_BODY_BUILTIN_ASSERT;
    if (strcmp(name, "panic") == 0) return CM_BODY_BUILTIN_PANIC;
    if (strcmp(name, "unreachable") == 0) return CM_BODY_BUILTIN_UNREACHABLE;
    if (strcmp(name, "format_args") == 0
        || strcmp(name, "const_format_args") == 0)
        return CM_BODY_BUILTIN_FORMAT_ARGS;
    if (strcmp(name, "asm") == 0) return CM_BODY_BUILTIN_ASM;
    for (index = 0u; index < sizeof(unsupported) / sizeof(unsupported[0]);
            ++index)
        if (strcmp(name, unsupported[index]) == 0)
            return CM_BODY_BUILTIN_UNSUPPORTED;
    return CM_BODY_BUILTIN_NONE;
}

/* Does the resolved declaration carry `#[rustc_builtin_macro...]`? */
static int cm_body_declaration_is_builtin(const CmBodyExpandState *state,
    const CmResolveMacroDeclaration *declaration)
{
    uint32_t index;
    for (index = 0u; index < declaration->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        char text[64];
        if (cm_module_graph_get_macro_declaration_attribute(state->graph,
                state->revision, declaration->declaration, index,
                &attribute) != CM_RESOLVE_VIEW_OK) continue;
        if (!cm_body_copy_name(state, attribute.metadata, text,
                sizeof(text))) continue;
        if (strncmp(text, "rustc_builtin_macro", 19u) == 0) return 1;
    }
    return 0;
}

typedef struct CmBodyMacroTarget {
    const CmAst *definition_ast;
    CmAstItemId definition_item;
    int is_builtin;
} CmBodyMacroTarget;

static int cm_body_target_from_declaration(CmBodyExpandState *state,
    CmResolveItemRef declaration, CmBodyMacroTarget *out_target)
{
    CmResolveMacroDeclaration record;
    memset(out_target, 0, sizeof(*out_target));
    if (cm_module_graph_get_macro_declaration(state->graph, state->revision,
            declaration, &record) != CM_RESOLVE_VIEW_OK) return 0;
    if (cm_body_declaration_is_builtin(state, &record)) {
        out_target->is_builtin = 1;
        return 1;
    }
    if (!cm_module_graph_borrow_item_ast(state->graph, record.owner_module,
            record.declaration, &out_target->definition_ast)) return 0;
    out_target->definition_item = record.declaration.item;
    return 1;
}

/* Last textual-scope entry of `module` with the given name. */
static int cm_body_lookup_scope(CmBodyExpandState *state, CmModuleId module,
    const char *name, CmBodyMacroTarget *out_target)
{
    uint32_t index;
    int found = 0;
    CmResolveItemRef declaration;
    memset(&declaration, 0, sizeof(declaration));
    for (index = 0u;; ++index) {
        CmResolveMacroScopeEntry entry;
        char text[CM_BODY_NAME_LIMIT];
        if (!cm_module_graph_get_macro_scope_entry(state->graph, module,
                index, &entry)) break;
        if (!cm_body_copy_name(state, entry.name, text, sizeof(text)))
            continue;
        if (strcmp(text, name) == 0) {
            declaration = entry.declaration;
            found = 1;
        }
    }
    return found && cm_body_target_from_declaration(state, declaration,
        out_target);
}

/* Macro-namespace (path-visible) entry of `module` with the given name. */
static int cm_body_lookup_namespace(CmBodyExpandState *state,
    CmModuleId module, const char *name, CmBodyMacroTarget *out_target)
{
    uint32_t index;
    for (index = 0u;; ++index) {
        CmResolveNamespaceEntry entry;
        char text[CM_BODY_NAME_LIMIT];
        if (!cm_module_graph_get_namespace_entry(state->graph, module,
                CM_RESOLVE_NAMESPACE_MACRO, index, &entry)) break;
        if (!cm_body_copy_name(state, entry.name, text, sizeof(text)))
            continue;
        if (strcmp(text, name) == 0)
            return cm_body_target_from_declaration(state, entry.declaration,
                out_target);
    }
    return 0;
}

static int cm_body_child_module(CmBodyExpandState *state, CmModuleId parent,
    const char *name, CmModuleId *out_child)
{
    CmResolveModuleInfo parent_info;
    uint32_t index;
    if (!cm_module_graph_get_module(state->graph, parent, &parent_info))
        return 0;
    for (index = 0u; index < parent_info.child_count; ++index) {
        CmModuleId child;
        CmResolveModuleInfo info;
        char text[CM_BODY_NAME_LIMIT];
        if (!cm_module_graph_get_child(state->graph, parent, index, &child)
            || !cm_module_graph_get_module(state->graph, child, &info)
            || !cm_body_copy_name(state, info.name, text, sizeof(text)))
            continue;
        if (strcmp(text, name) == 0) {
            *out_child = child;
            return 1;
        }
    }
    return 0;
}

/* `use` bindings of `module` in the macro namespace. */
static int cm_body_lookup_imports(CmBodyExpandState *state, CmModuleId module,
    const char *name, CmBodyMacroTarget *out_target)
{
    size_t count;
    uint32_t index;
    if (state->options->imports == NULL) return 0;
    count = cm_import_binding_count(state->options->imports, module,
        CM_RESOLVE_NAMESPACE_MACRO);
    for (index = 0u; (size_t)index < count; ++index) {
        CmResolvedBinding binding;
        char text[CM_BODY_NAME_LIMIT];
        if (!cm_import_get_binding(state->options->imports, module,
                CM_RESOLVE_NAMESPACE_MACRO, index, &binding)) continue;
        if (!cm_import_copy_string(state->options->imports, binding.name,
                text, sizeof(text))) continue;
        if (strcmp(text, name) == 0 && binding.declaration.item
                != CM_AST_ITEM_NONE)
            return cm_body_target_from_declaration(state,
                binding.declaration, out_target);
    }
    return 0;
}

/*
 * Lenient unqualified lookup: the module's textual scope and every ancestor
 * module's textual scope (a child sees the `macro_rules!` its parents saw),
 * then the module's `use`-imported macros, then the crate root's macro
 * namespace (`#[macro_export]`).  Textual-order precision is not modelled;
 * the input is assumed to be valid Rust.
 */
static int cm_body_lookup_unqualified(CmBodyExpandState *state,
    CmModuleId module, const char *name, CmBodyMacroTarget *out_target)
{
    CmModuleId current = module;
    CmModuleId root;
    unsigned int steps = 0u;
    while (current != CM_MODULE_NONE && steps < 64u) {
        CmResolveModuleInfo info;
        if (cm_body_lookup_scope(state, current, name, out_target)) return 1;
        if (cm_body_lookup_imports(state, current, name, out_target)) return 1;
        if (cm_body_lookup_namespace(state, current, name, out_target))
            return 1;
        if (!cm_module_graph_get_module(state->graph, current, &info)) break;
        if (info.parent == current) break;
        current = info.parent;
        steps += 1u;
    }
    if (cm_module_graph_get_root(state->graph, &root)
        && cm_body_lookup_namespace(state, root, name, out_target)) return 1;
    return 0;
}

/*
 * Resolve a (possibly qualified) macro path.  Unqualified names use the
 * body-local definitions, then the module's textual scope.  Qualified paths
 * walk `crate`/`self`/`super`/module names and end in the macro namespace.
 */
static int cm_body_resolve_path(CmBodyExpandState *state, const CmAst *ast,
    const CmAstPath *path, char *out_name, size_t name_size,
    CmBodyMacroTarget *out_target)
{
    CmModuleId module = state->module;
    uint32_t segment = 0u;
    if (path == NULL || path->segment_count == 0u
        || !cm_body_segment_name(ast, path, path->segment_count - 1u,
            out_name, name_size)) return 0;
    if (path->segment_count == 1u && !path->absolute) {
        size_t index = state->local_count;
        while (index != 0u) {
            index -= 1u;
            if (strcmp(state->locals[index].name, out_name) == 0) {
                memset(out_target, 0, sizeof(*out_target));
                out_target->definition_ast = state->ast;
                out_target->definition_item = state->locals[index].item;
                return 1;
            }
        }
        return cm_body_lookup_unqualified(state, module, out_name,
            out_target);
    }
    if (path->absolute) {
        /* `::name!` (edition 2015 extern) or `::crate_name::...`: only the
         * local crate root is modelled here. */
        if (!cm_module_graph_get_root(state->graph, &module)) return 0;
        segment = 0u;
        if (path->segment_count >= 2u) {
            char head[CM_BODY_NAME_LIMIT];
            if (cm_body_segment_name(ast, path, 0u, head, sizeof(head))
                && (strcmp(head, "core") == 0 || strcmp(head, "crate") == 0))
                segment = 1u;
        }
    }
    for (; segment + 1u < path->segment_count; ++segment) {
        char name[CM_BODY_NAME_LIMIT];
        if (!cm_body_segment_name(ast, path, segment, name, sizeof(name)))
            return 0;
        if (segment == 0u && !path->absolute
            && (strcmp(name, "crate") == 0 || strcmp(name, "$crate") == 0)) {
            if (!cm_module_graph_get_root(state->graph, &module)) return 0;
        } else if (strcmp(name, "self") == 0) {
            continue;
        } else if (strcmp(name, "super") == 0) {
            CmResolveModuleInfo info;
            if (!cm_module_graph_get_module(state->graph, module, &info))
                return 0;
            module = info.parent;
        } else if (segment == 0u && !path->absolute
            && strcmp(name, "core") == 0
            && strcmp(state->options->crate_identifier, "crate") == 0) {
            if (!cm_module_graph_get_root(state->graph, &module)) return 0;
        } else if (!cm_body_child_module(state, module, name, &module)) {
            return 0;
        }
    }
    if (cm_body_lookup_namespace(state, module, out_name, out_target))
        return 1;
    return cm_body_lookup_unqualified(state, module, out_name, out_target);
}

/* ------------------------------------------------------------------ */
/* Splicing                                                             */

static int cm_body_splice(CmBodyExpandState *state, CmAstExprId target,
    CmAstExprId replacement)
{
    const CmAstExpr *source = cm_ast_get_expr(state->ast, replacement);
    CmAstExpr *destination;
    CmAstSpan span;
    if (source == NULL || target == CM_AST_EXPR_NONE
        || (size_t)target > state->ast->expressions.len) return 0;
    destination = (CmAstExpr *)cm_vec_at(&state->ast->expressions,
        (size_t)target - 1u);
    if (destination == NULL) return 0;
    span = destination->span;
    *destination = *source;
    /* Generated nodes carry offsets into generated text; keep the
     * invocation's source span as the coarse diagnostic anchor. */
    destination->span = span;
    return 1;
}

static CmAstExprId cm_body_parse_text(CmBodyExpandState *state,
    const char *text, size_t length, const char **out_error)
{
    CmExpressionFragment fragment = cm_parse_expression_fragment(state->ast,
        text, length, state->options->edition);
    if (fragment.parse.error_count != 0u) {
        *out_error = fragment.parse.first_error.message;
        return CM_AST_EXPR_NONE;
    }
    *out_error = NULL;
    return fragment.expression;
}

/*
 * Parse generated text as an expression; statement-shaped output (several
 * statements, trailing `;`) is retried inside a block.
 */
static CmAstExprId cm_body_parse_expansion(CmBodyExpandState *state,
    const char *text, size_t length, const char **out_error)
{
    CmAstExprId expression = cm_body_parse_text(state, text, length,
        out_error);
    if (expression != CM_AST_EXPR_NONE) return expression;
    cm_str_buf_clear(&state->scratch);
    cm_str_buf_append(&state->scratch, "{ ");
    cm_str_buf_append_n(&state->scratch, text, length);
    cm_str_buf_append(&state->scratch, " }");
    return cm_body_parse_text(state, state->scratch.data,
        state->scratch.len, out_error);
}

/* ------------------------------------------------------------------ */
/* macro_rules expansion                                                */

static int cm_body_expand_rules(CmBodyExpandState *state, CmAstExprId id,
    const char *name, const CmBodyMacroTarget *target)
{
    const CmAstExpr *expr = cm_ast_get_expr(state->ast, id);
    CmMacroSyntaxOptions options;
    CmMacroSyntaxResult expansion;
    CmAstExprId replacement;
    const char *error = NULL;
    CmAstSpan span;
    if (expr == NULL) return 0;
    span = expr->span;
    cm_macro_syntax_options_init(&options);
    options.edition = state->options->edition;
    options.crate_identifier = state->options->crate_identifier;
    cm_str_buf_clear(&state->text);
    expansion = cm_macro_syntax_expand(target->definition_ast,
        target->definition_item, state->ast, &expr->data.macro_expr,
        &options, &state->text);
    if (expansion.status != CM_MACRO_OK) {
        cm_body_fail(state, name, span, expansion.diagnostic.message == NULL
            ? "macro_rules expansion failed" : expansion.diagnostic.message);
        return 0;
    }
    replacement = cm_body_parse_expansion(state, state->text.data,
        state->text.len, &error);
    if (replacement == CM_AST_EXPR_NONE) {
        if (getenv("CM_BODY_EXPAND_DEBUG") != NULL)
            fprintf(stderr, "body-expand: %s! expansion failed (%s):\n%.*s\n",
                name, error == NULL ? "?" : error, (int)state->text.len,
                state->text.data);
        cm_body_fail(state, name, span,
            error == NULL ? "expansion did not parse" : error);
        return 0;
    }
    if (!cm_body_splice(state, id, replacement)) {
        cm_body_fail(state, name, span, "could not splice expansion");
        return 0;
    }
    state->result.expanded_rules += 1u;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Builtins                                                             */

static int cm_body_finish_generated(CmBodyExpandState *state, CmAstExprId id,
    const char *name, CmAstSpan span)
{
    const char *error = NULL;
    CmAstExprId replacement = cm_body_parse_expansion(state,
        state->text.data, state->text.len, &error);
    if (replacement == CM_AST_EXPR_NONE) {
        if (getenv("CM_BODY_EXPAND_DEBUG") != NULL)
            fprintf(stderr, "body-expand: %s! generated text failed (%s):\n%.*s\n",
                name, error == NULL ? "?" : error, (int)state->text.len,
                state->text.data);
        cm_body_fail(state, name, span,
            error == NULL ? "generated builtin text did not parse" : error);
        return 0;
    }
    if (!cm_body_splice(state, id, replacement)) {
        cm_body_fail(state, name, span, "could not splice builtin");
        return 0;
    }
    state->result.expanded_builtin += 1u;
    return 1;
}

static int cm_body_builtin_ast(CmBodyExpandState *state, CmAstExprId id,
    const char *name, CmAstSpan span)
{
    CmBuiltinContext context;
    CmBuiltinAstResult result;
    memset(&context, 0, sizeof(context));
    context.file = "";
    context.module_path = "";
    context.cfg = state->options->cfg == NULL ? NULL
        : &state->options->cfg->environment;
    result = cm_builtin_ast_expand_expression(state->ast, id, &context,
        state->options->edition);
    if (result.expanded_expression == CM_AST_EXPR_NONE) {
        cm_body_fail(state, name, span, result.macro_diagnostic.message
            == NULL ? "builtin expansion failed"
            : result.macro_diagnostic.message);
        return 0;
    }
    if (!cm_body_splice(state, id, result.expanded_expression)) {
        cm_body_fail(state, name, span, "could not splice builtin");
        return 0;
    }
    state->result.expanded_builtin += 1u;
    return 1;
}

/* `assert!(cond)` / `assert!(cond, args...)`. */
static int cm_body_builtin_assert(CmBodyExpandState *state, CmAstExprId id,
    const char *name, const char *arguments, size_t length, CmAstSpan span)
{
    size_t starts[2];
    size_t lengths[2];
    size_t count = cm_body_split_commas(arguments, length, starts, lengths, 2u);
    if (count == 0u) {
        cm_body_fail(state, name, span, "assert! needs a condition");
        return 0;
    }
    cm_str_buf_clear(&state->text);
    cm_str_buf_append(&state->text, "if !(");
    cm_str_buf_append_n(&state->text, arguments + starts[0], lengths[0]);
    cm_str_buf_append(&state->text, ") { ");
    if (count == 1u) {
        cm_str_buf_append(&state->text, state->options->crate_identifier);
        cm_str_buf_append(&state->text,
            "::panicking::panic(\"assertion failed: ");
        cm_body_append_escaped(&state->text, arguments + starts[0],
            lengths[0]);
        cm_str_buf_append(&state->text, "\")");
    } else {
        /* Everything after the first top-level comma is the panic input. */
        const char *rest = arguments + starts[1];
        size_t rest_length = length - starts[1];
        cm_body_trim(&rest, &rest_length);
        cm_str_buf_append(&state->text, "panic!(");
        cm_str_buf_append_n(&state->text, rest, rest_length);
        cm_str_buf_append(&state->text, ")");
    }
    cm_str_buf_append(&state->text, " }");
    return cm_body_finish_generated(state, id, name, span);
}

/* `panic!`/`unreachable!` forward to the edition-specific core macro. */
static int cm_body_builtin_forward(CmBodyExpandState *state, CmAstExprId id,
    const char *name, const char *arguments, size_t length, CmAstSpan span,
    const char *target)
{
    cm_str_buf_clear(&state->text);
    cm_str_buf_append(&state->text, state->options->crate_identifier);
    cm_str_buf_append(&state->text, "::panic::");
    cm_str_buf_append(&state->text, target);
    cm_str_buf_append(&state->text, state->options->edition
            >= CM_EDITION_2021 ? "_2021!(" : "_2015!(");
    cm_str_buf_append_n(&state->text, arguments, length);
    cm_str_buf_append(&state->text, ")");
    return cm_body_finish_generated(state, id, name, span);
}

typedef struct CmBodyFormatArg {
    const char *text;
    size_t length;
    const char *name;
    size_t name_length;
} CmBodyFormatArg;

#define CM_BODY_FORMAT_ARG_LIMIT 64u

static const char *cm_body_format_trait(const char *spec, size_t length)
{
    if (length == 0u) return "new_display";
    if (length == 1u) {
        switch (spec[0]) {
        case '?': return "new_debug";
        case 'x': return "new_lower_hex";
        case 'X': return "new_upper_hex";
        case 'o': return "new_octal";
        case 'b': return "new_binary";
        case 'e': return "new_lower_exp";
        case 'E': return "new_upper_exp";
        case 'p': return "new_pointer";
        default: return NULL;
        }
    }
    return NULL;
}

/*
 * `format_args!("...", args...)`.  Supports positional, named, implicit
 * captured arguments and the trait selectors; width/precision/fill/flags
 * (which need `rt::Placeholder`) are counted as unsupported for now.
 */
static int cm_body_builtin_format_args(CmBodyExpandState *state,
    CmAstExprId id, const char *name, const char *arguments, size_t length,
    CmAstSpan span)
{
    size_t starts[CM_BODY_FORMAT_ARG_LIMIT + 1u];
    size_t lengths[CM_BODY_FORMAT_ARG_LIMIT + 1u];
    size_t count;
    CmBodyFormatArg args[CM_BODY_FORMAT_ARG_LIMIT];
    size_t arg_count = 0u;
    size_t positional_count = 0u;
    const char *format;
    size_t format_length;
    size_t index;
    size_t next_positional = 0u;
    size_t placeholder_count = 0u;
    /* argument index per placeholder and trait constructor per placeholder */
    size_t placeholder_arg[CM_BODY_FORMAT_ARG_LIMIT];
    const char *placeholder_trait[CM_BODY_FORMAT_ARG_LIMIT];
    int trailing_piece = 0;

    count = cm_body_split_commas(arguments, length, starts, lengths,
        CM_BODY_FORMAT_ARG_LIMIT + 1u);
    if (count == 0u || count > CM_BODY_FORMAT_ARG_LIMIT) {
        cm_body_fail(state, name, span, "format_args! argument count");
        return 0;
    }
    if (!cm_body_literal_text(arguments + starts[0], lengths[0],
            &state->scratch)) {
        cm_body_fail(state, name, span,
            "format string is not a literal expression");
        return 0;
    }
    /* scratch holds `"..."`; keep it alive while pieces are built. */
    format = state->scratch.data + 1;
    format_length = state->scratch.len - 2u;
    for (index = 1u; index < count; ++index) {
        const char *text = arguments + starts[index];
        size_t text_length = lengths[index];
        size_t probe = 0u;
        args[arg_count].text = text;
        args[arg_count].length = text_length;
        args[arg_count].name = NULL;
        args[arg_count].name_length = 0u;
        /* `name = expr` (but not `==`). */
        if (text_length != 0u && cm_body_is_ident_start(text[0])) {
            while (probe < text_length && cm_body_is_ident_continue(text[probe]))
                probe += 1u;
            {
                size_t after = probe;
                while (after < text_length && cm_body_is_space(text[after]))
                    after += 1u;
                if (after < text_length && text[after] == '='
                    && !(after + 1u < text_length && text[after + 1u] == '=')) {
                    const char *value = text + after + 1u;
                    size_t value_length = text_length - after - 1u;
                    cm_body_trim(&value, &value_length);
                    args[arg_count].name = text;
                    args[arg_count].name_length = probe;
                    args[arg_count].text = value;
                    args[arg_count].length = value_length;
                }
            }
        }
        if (args[arg_count].name == NULL) positional_count += 1u;
        arg_count += 1u;
    }

    /* Parse the format string into pieces and placeholders. */
    cm_str_buf_clear(&state->pieces);
    cm_str_buf_append(&state->pieces, "&[\"");
    for (index = 0u; index < format_length; ++index) {
        char c = format[index];
        if (c == '{' && index + 1u < format_length && format[index + 1u] == '{') {
            cm_str_buf_push(&state->pieces, '{');
            index += 1u;
            continue;
        }
        if (c == '}' && index + 1u < format_length && format[index + 1u] == '}') {
            cm_str_buf_push(&state->pieces, '}');
            index += 1u;
            continue;
        }
        if (c == '\\' && index + 1u < format_length) {
            cm_str_buf_push(&state->pieces, c);
            cm_str_buf_push(&state->pieces, format[index + 1u]);
            index += 1u;
            continue;
        }
        if (c != '{') {
            cm_str_buf_push(&state->pieces, c);
            continue;
        }
        {
            size_t close = index + 1u;
            size_t colon;
            const char *argument;
            size_t argument_length;
            const char *spec = NULL;
            size_t spec_length = 0u;
            const char *trait;
            size_t arg_index;
            while (close < format_length && format[close] != '}') close += 1u;
            if (close >= format_length) {
                cm_body_fail(state, name, span, "unterminated format placeholder");
                return 0;
            }
            argument = format + index + 1u;
            argument_length = close - index - 1u;
            for (colon = 0u; colon < argument_length; ++colon)
                if (argument[colon] == ':') break;
            if (colon < argument_length) {
                spec = argument + colon + 1u;
                spec_length = argument_length - colon - 1u;
                argument_length = colon;
            }
            cm_body_trim(&argument, &argument_length);
            trait = cm_body_format_trait(spec, spec_length);
            if (trait == NULL) {
                /* Width, precision, fill, alignment, sign, `#`, `0`. */
                state->result.remaining_builtin += 1u;
                return 0;
            }
            if (placeholder_count == CM_BODY_FORMAT_ARG_LIMIT) {
                cm_body_fail(state, name, span, "too many placeholders");
                return 0;
            }
            if (argument_length == 0u) {
                arg_index = next_positional;
                next_positional += 1u;
                if (arg_index >= positional_count) {
                    cm_body_fail(state, name, span,
                        "format placeholder has no positional argument");
                    return 0;
                }
            } else if (argument[0] >= '0' && argument[0] <= '9') {
                size_t digit;
                arg_index = 0u;
                for (digit = 0u; digit < argument_length; ++digit)
                    arg_index = arg_index * 10u
                        + (size_t)(argument[digit] - '0');
                if (arg_index >= positional_count) {
                    cm_body_fail(state, name, span,
                        "format placeholder index out of range");
                    return 0;
                }
            } else {
                size_t search;
                arg_index = arg_count;
                for (search = 0u; search < arg_count; ++search)
                    if (args[search].name != NULL
                        && args[search].name_length == argument_length
                        && memcmp(args[search].name, argument,
                            argument_length) == 0) {
                        arg_index = search;
                        break;
                    }
                if (arg_index == arg_count) {
                    /* Implicit capture of a local. */
                    if (arg_count == CM_BODY_FORMAT_ARG_LIMIT) {
                        cm_body_fail(state, name, span, "too many arguments");
                        return 0;
                    }
                    args[arg_count].text = argument;
                    args[arg_count].length = argument_length;
                    args[arg_count].name = argument;
                    args[arg_count].name_length = argument_length;
                    arg_count += 1u;
                }
            }
            placeholder_arg[placeholder_count] = arg_index;
            placeholder_trait[placeholder_count] = trait;
            placeholder_count += 1u;
            cm_str_buf_append(&state->pieces, "\", \"");
            index = close;
        }
    }
    /* Close the last piece; drop it when empty and placeholders exist. */
    {
        /* An empty trailing piece is dropped: `"a ", "` becomes `"a "`. */
        size_t tail = state->pieces.len;
        trailing_piece = 1;
        if (placeholder_count != 0u && tail >= 4u
            && memcmp(state->pieces.data + tail - 4u, "\", \"", 4u) == 0) {
            state->pieces.len = tail - 3u;
            trailing_piece = 0;
        }
        cm_str_buf_append(&state->pieces, trailing_piece ? "\"]" : "]");
    }

    cm_str_buf_clear(&state->text);
    if (placeholder_count == 0u) {
        if (arg_count != 0u) {
            /* Arguments without placeholders: evaluate them for effect. */
            cm_str_buf_append(&state->text, "{ ");
            for (index = 0u; index < arg_count; ++index) {
                cm_str_buf_append(&state->text, "let _ = &(");
                cm_str_buf_append_n(&state->text, args[index].text,
                    args[index].length);
                cm_str_buf_append(&state->text, "); ");
            }
        }
        cm_str_buf_append(&state->text, state->options->crate_identifier);
        cm_str_buf_append(&state->text, "::fmt::Arguments::new_const(");
        cm_str_buf_append_n(&state->text, state->pieces.data,
            state->pieces.len);
        cm_str_buf_append(&state->text, ")");
        if (arg_count != 0u) cm_str_buf_append(&state->text, " }");
        return cm_body_finish_generated(state, id, name, span);
    }
    /* match (&a, &b, ...) { args => Arguments::new_v1(pieces, &[...]) } */
    cm_str_buf_append(&state->text, "match (");
    for (index = 0u; index < arg_count; ++index) {
        cm_str_buf_append(&state->text, "&(");
        cm_str_buf_append_n(&state->text, args[index].text,
            args[index].length);
        cm_str_buf_append(&state->text, "), ");
    }
    cm_str_buf_append(&state->text, ") { args => ");
    cm_str_buf_append(&state->text, state->options->crate_identifier);
    cm_str_buf_append(&state->text, "::fmt::Arguments::new_v1(");
    cm_str_buf_append_n(&state->text, state->pieces.data, state->pieces.len);
    cm_str_buf_append(&state->text, ", &[");
    for (index = 0u; index < placeholder_count; ++index) {
        char number[32];
        int written;
        cm_str_buf_append(&state->text, state->options->crate_identifier);
        cm_str_buf_append(&state->text, "::fmt::rt::Argument::");
        cm_str_buf_append(&state->text, placeholder_trait[index]);
        written = snprintf(number, sizeof(number), "(args.%lu)",
            (unsigned long)placeholder_arg[index]);
        if (written <= 0 || (size_t)written >= sizeof(number)) return 0;
        cm_str_buf_append(&state->text, number);
        if (index + 1u < placeholder_count)
            cm_str_buf_append(&state->text, ", ");
    }
    cm_str_buf_append(&state->text, "]) }");
    return cm_body_finish_generated(state, id, name, span);
}

/* ------------------------------------------------------------------ */
/* Expansion dispatch                                                   */

static void cm_body_walk_expr(CmBodyExpandState *state, CmAstExprId id,
    unsigned int depth);

static int cm_body_expand_macro(CmBodyExpandState *state, CmAstExprId id,
    unsigned int depth)
{
    const CmAstExpr *expr = cm_ast_get_expr(state->ast, id);
    const CmAstPath *path;
    CmAstSpan span;
    char name[CM_BODY_NAME_LIMIT];
    CmBodyMacroTarget target;
    CmBodyBuiltin builtin;
    const char *arguments = "";
    size_t argument_length = 0u;
    int resolved;
    if (expr == NULL || expr->kind != CM_AST_EXPR_MACRO) return 0;
    span = expr->span;
    state->result.invocations += 1u;
    path = cm_ast_get_path(state->ast, expr->data.macro_expr.path);
    if (path == NULL || path->segment_count == 0u
        || !cm_body_segment_name(state->ast, path, path->segment_count - 1u,
            name, sizeof(name))) {
        cm_body_fail(state, "?", span, "macro path is not a name");
        return 0;
    }
    if (depth > state->options->maximum_depth) {
        cm_body_fail(state, name, span, "macro nesting limit");
        return 0;
    }
    if (expr->data.macro_expr.arguments != CM_INTERN_ID_NONE
        && !cm_body_intern_text(state->ast, expr->data.macro_expr.arguments,
            &arguments, &argument_length)) {
        cm_body_fail(state, name, span, "macro arguments unavailable");
        return 0;
    }
    memset(&target, 0, sizeof(target));
    resolved = cm_body_resolve_path(state, state->ast, path, name,
        sizeof(name), &target);
    if (resolved && !target.is_builtin)
        return cm_body_expand_rules(state, id, name, &target);
    builtin = cm_body_builtin_by_name(name);
    switch (builtin) {
    case CM_BODY_BUILTIN_AST:
        if (strcmp(name, "concat") == 0 || strcmp(name, "stringify") == 0) {
            /* Nested `concat!`/`stringify!` arguments are folded textually. */
            cm_str_buf_clear(&state->scratch);
            cm_str_buf_append(&state->scratch, name);
            cm_str_buf_append(&state->scratch, "!(");
            cm_str_buf_append_n(&state->scratch, arguments, argument_length);
            cm_str_buf_append(&state->scratch, ")");
            if (cm_body_literal_text(state->scratch.data, state->scratch.len,
                    &state->text))
                return cm_body_finish_generated(state, id, name, span);
        }
        return cm_body_builtin_ast(state, id, name, span);
    case CM_BODY_BUILTIN_ASSERT:
        return cm_body_builtin_assert(state, id, name, arguments,
            argument_length, span);
    case CM_BODY_BUILTIN_PANIC:
        return cm_body_builtin_forward(state, id, name, arguments,
            argument_length, span, "panic");
    case CM_BODY_BUILTIN_UNREACHABLE:
        return cm_body_builtin_forward(state, id, name, arguments,
            argument_length, span, "unreachable");
    case CM_BODY_BUILTIN_FORMAT_ARGS:
        return cm_body_builtin_format_args(state, id, name, arguments,
            argument_length, span);
    case CM_BODY_BUILTIN_ASM:
        state->result.remaining_asm += 1u;
        return 0;
    case CM_BODY_BUILTIN_UNSUPPORTED:
        state->result.remaining_builtin += 1u;
        return 0;
    case CM_BODY_BUILTIN_NONE:
    default:
        break;
    }
    cm_body_fail(state, name, span, resolved
        ? "builtin macro is not implemented"
        : "macro is not in scope");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Walkers                                                              */

static void cm_body_walk_item(CmBodyExpandState *state, CmAstItemId id,
    unsigned int depth);

static void cm_body_walk_stmt(CmBodyExpandState *state, CmAstStmtId id,
    unsigned int depth)
{
    const CmAstStmt *stmt = cm_ast_get_stmt(state->ast, id);
    if (stmt == NULL) return;
    switch (stmt->kind) {
    case CM_AST_STMT_LET:
        cm_body_walk_expr(state, stmt->data.let_stmt.initializer, depth);
        cm_body_walk_expr(state, stmt->data.let_stmt.else_block, depth);
        break;
    case CM_AST_STMT_EXPR:
        cm_body_walk_expr(state, stmt->data.expr_stmt.expression, depth);
        break;
    case CM_AST_STMT_ITEM:
        cm_body_walk_item(state, stmt->data.item_stmt.item, depth);
        break;
    }
}

static void cm_body_walk_expr(CmBodyExpandState *state, CmAstExprId id,
    unsigned int depth)
{
    const CmAstExpr *expr = cm_ast_get_expr(state->ast, id);
    uint32_t index;
    if (expr == NULL) return;
    /* Copy the node: expansion may reallocate the expression vector. */
    switch (expr->kind) {
    case CM_AST_EXPR_MACRO:
        if (cm_body_expand_macro(state, id, depth))
            cm_body_walk_expr(state, id, depth + 1u);
        return;
    case CM_AST_EXPR_BLOCK:
    case CM_AST_EXPR_TRY_BLOCK: {
        size_t saved_locals = state->local_count;
        uint32_t statement_count = expr->data.block.statement_count;
        CmAstExprId tail = expr->data.block.tail;
        CmAstStmtId *statements = expr->data.block.statements;
        for (index = 0u; index < statement_count; ++index)
            cm_body_walk_stmt(state, statements[index], depth);
        cm_body_walk_expr(state, tail, depth);
        state->local_count = saved_locals;
        return;
    }
    case CM_AST_EXPR_CALL: {
        CmAstExprId callee = expr->data.call.callee;
        uint32_t argument_count = expr->data.call.argument_count;
        CmAstExprId *arguments = expr->data.call.arguments;
        cm_body_walk_expr(state, callee, depth);
        for (index = 0u; index < argument_count; ++index)
            cm_body_walk_expr(state, arguments[index], depth);
        return;
    }
    case CM_AST_EXPR_METHOD_CALL: {
        CmAstExprId receiver = expr->data.method_call.receiver;
        uint32_t argument_count = expr->data.method_call.argument_count;
        CmAstExprId *arguments = expr->data.method_call.arguments;
        cm_body_walk_expr(state, receiver, depth);
        for (index = 0u; index < argument_count; ++index)
            cm_body_walk_expr(state, arguments[index], depth);
        return;
    }
    case CM_AST_EXPR_FIELD:
        cm_body_walk_expr(state, expr->data.field.base, depth);
        return;
    case CM_AST_EXPR_TUPLE_FIELD:
        cm_body_walk_expr(state, expr->data.tuple_field.base, depth);
        return;
    case CM_AST_EXPR_INDEX: {
        CmAstExprId base = expr->data.index.base;
        CmAstExprId subscript = expr->data.index.index;
        cm_body_walk_expr(state, base, depth);
        cm_body_walk_expr(state, subscript, depth);
        return;
    }
    case CM_AST_EXPR_UNARY:
        cm_body_walk_expr(state, expr->data.unary.operand, depth);
        return;
    case CM_AST_EXPR_RAW_REFERENCE:
        cm_body_walk_expr(state, expr->data.raw_reference.operand, depth);
        return;
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN: {
        CmAstExprId left = expr->data.binary.left;
        CmAstExprId right = expr->data.binary.right;
        cm_body_walk_expr(state, left, depth);
        cm_body_walk_expr(state, right, depth);
        return;
    }
    case CM_AST_EXPR_CAST:
        cm_body_walk_expr(state, expr->data.cast.value, depth);
        return;
    case CM_AST_EXPR_TRY:
        cm_body_walk_expr(state, expr->data.try_expr.operand, depth);
        return;
    case CM_AST_EXPR_RANGE: {
        CmAstExprId start = expr->data.range.start;
        CmAstExprId end = expr->data.range.end;
        cm_body_walk_expr(state, start, depth);
        cm_body_walk_expr(state, end, depth);
        return;
    }
    case CM_AST_EXPR_LET:
        cm_body_walk_expr(state, expr->data.let_expr.initializer, depth);
        return;
    case CM_AST_EXPR_RETURN:
    case CM_AST_EXPR_BREAK:
    case CM_AST_EXPR_CONTINUE:
        cm_body_walk_expr(state, expr->data.flow.value, depth);
        return;
    case CM_AST_EXPR_IF: {
        CmAstExprId condition = expr->data.if_expr.condition;
        CmAstExprId then_expr = expr->data.if_expr.then_expr;
        CmAstExprId else_expr = expr->data.if_expr.else_expr;
        cm_body_walk_expr(state, condition, depth);
        cm_body_walk_expr(state, then_expr, depth);
        cm_body_walk_expr(state, else_expr, depth);
        return;
    }
    case CM_AST_EXPR_MATCH: {
        CmAstExprId scrutinee = expr->data.match_expr.scrutinee;
        uint32_t arm_count = expr->data.match_expr.arm_count;
        CmAstMatchArm *arms = expr->data.match_expr.arms;
        cm_body_walk_expr(state, scrutinee, depth);
        for (index = 0u; index < arm_count; ++index) {
            CmAstExprId guard = arms[index].guard;
            CmAstExprId guard_initializer = arms[index].guard_initializer;
            CmAstExprId body = arms[index].body;
            cm_body_walk_expr(state, guard, depth);
            cm_body_walk_expr(state, guard_initializer, depth);
            cm_body_walk_expr(state, body, depth);
        }
        return;
    }
    case CM_AST_EXPR_LOOP:
        cm_body_walk_expr(state, expr->data.loop_expr.body, depth);
        return;
    case CM_AST_EXPR_WHILE: {
        CmAstExprId condition = expr->data.while_expr.condition;
        CmAstExprId body = expr->data.while_expr.body;
        cm_body_walk_expr(state, condition, depth);
        cm_body_walk_expr(state, body, depth);
        return;
    }
    case CM_AST_EXPR_FOR: {
        CmAstExprId iterable = expr->data.for_expr.iterable;
        CmAstExprId body = expr->data.for_expr.body;
        cm_body_walk_expr(state, iterable, depth);
        cm_body_walk_expr(state, body, depth);
        return;
    }
    case CM_AST_EXPR_CLOSURE:
        cm_body_walk_expr(state, expr->data.closure.body, depth);
        return;
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY: {
        uint32_t element_count = expr->data.list.element_count;
        CmAstExprId *elements = expr->data.list.elements;
        CmAstExprId repeat_value = expr->data.list.repeat_value;
        CmAstExprId repeat_length = expr->data.list.repeat_length;
        for (index = 0u; index < element_count; ++index)
            cm_body_walk_expr(state, elements[index], depth);
        cm_body_walk_expr(state, repeat_value, depth);
        cm_body_walk_expr(state, repeat_length, depth);
        return;
    }
    case CM_AST_EXPR_STRUCT: {
        uint32_t field_count = expr->data.struct_expr.field_count;
        CmAstExprField *fields = expr->data.struct_expr.fields;
        CmAstExprId base = expr->data.struct_expr.base;
        for (index = 0u; index < field_count; ++index)
            cm_body_walk_expr(state, fields[index].value, depth);
        cm_body_walk_expr(state, base, depth);
        return;
    }
    default:
        return;
    }
}

static void cm_body_register_local_macro(CmBodyExpandState *state,
    const CmAstItem *item, CmAstItemId id)
{
    const char *text;
    size_t length;
    if (state->local_count == CM_BODY_LOCAL_MACRO_LIMIT
        || !cm_body_intern_text(state->ast, item->name, &text, &length)
        || length == 0u || length >= CM_BODY_NAME_LIMIT) return;
    memcpy(state->locals[state->local_count].name, text, length);
    state->locals[state->local_count].name[length] = '\0';
    state->locals[state->local_count].item = id;
    state->local_count += 1u;
}

static void cm_body_walk_item(CmBodyExpandState *state, CmAstItemId id,
    unsigned int depth)
{
    const CmAstItem *item = cm_ast_get_item(state->ast, id);
    uint32_t index;
    if (item == NULL) return;
    switch (item->kind) {
    case CM_AST_ITEM_FUNCTION:
        state->result.bodies += 1u;
        cm_body_walk_expr(state, item->data.function_item.body, depth);
        break;
    case CM_AST_ITEM_CONST:
    case CM_AST_ITEM_STATIC:
        if (item->data.value_item.initializer != CM_AST_EXPR_NONE)
            state->result.bodies += 1u;
        cm_body_walk_expr(state, item->data.value_item.initializer, depth);
        break;
    case CM_AST_ITEM_IMPL:
        for (index = 0u; index < item->data.impl_item.item_count; ++index)
            cm_body_walk_item(state, item->data.impl_item.items[index],
                depth);
        break;
    case CM_AST_ITEM_TRAIT:
        for (index = 0u; index < item->data.trait_item.item_count; ++index)
            cm_body_walk_item(state, item->data.trait_item.items[index],
                depth);
        break;
    case CM_AST_ITEM_MACRO:
        if (item->data.macro_item.form != CM_AST_MACRO_INVOCATION)
            cm_body_register_local_macro(state, item, id);
        break;
    default:
        break;
    }
}

/* Cfg-active items of one module, including impl/trait children. */
static void cm_body_walk_module(CmBodyExpandState *state, CmModuleId module)
{
    uint32_t index;
    state->module = module;
    state->local_count = 0u;
    state->ast = cm_module_graph_borrow_ast_mut(state->graph, module);
    if (state->ast == NULL) return;
    for (index = 0u;; ++index) {
        CmResolveEffectiveItem item;
        const CmAstItem *ast_item;
        if (cm_module_graph_get_effective_item(state->graph, state->revision,
                module, index, &item) != CM_RESOLVE_VIEW_OK) break;
        ast_item = cm_ast_get_item(state->ast, item.declaration.item);
        if (ast_item == NULL) continue;
        if (ast_item->kind == CM_AST_ITEM_IMPL
            || ast_item->kind == CM_AST_ITEM_TRAIT) {
            uint32_t child;
            for (child = 0u; child < item.child_count; ++child) {
                CmResolveEffectiveItem view;
                if (cm_module_graph_get_effective_child(state->graph,
                        state->revision, module, item.id, child, &view)
                        != CM_RESOLVE_VIEW_OK) continue;
                cm_body_walk_item(state, view.declaration.item, 0u);
            }
        } else if (ast_item->kind == CM_AST_ITEM_MODULE) {
            /* Inline module contents are roots of their own CmModuleId. */
            continue;
        } else {
            cm_body_walk_item(state, item.declaration.item, 0u);
        }
    }
}

CmBodyExpandResult cm_body_expand_graph(CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmBodyExpandOptions *options)
{
    CmBodyExpandState *state;
    CmBodyExpandResult result;
    size_t module_count;
    size_t index;
    memset(&result, 0, sizeof(result));
    if (graph == NULL || options == NULL) return result;
    state = (CmBodyExpandState *)cm_alloc_zeroed(1u, sizeof(*state));
    state->graph = graph;
    state->revision = revision;
    state->options = options;
    cm_str_buf_init(&state->text);
    cm_str_buf_init(&state->scratch);
    cm_str_buf_init(&state->pieces);
    module_count = cm_module_graph_module_count(graph);
    for (index = 0u; index < module_count; ++index) {
        CmResolveModuleInfo info;
        if (!cm_module_graph_get_module_at(graph, index, &info)) continue;
        cm_body_walk_module(state, info.id);
    }
    result = state->result;
    cm_str_buf_destroy(&state->pieces);
    cm_str_buf_destroy(&state->scratch);
    cm_str_buf_destroy(&state->text);
    cm_free(state);
    return result;
}
