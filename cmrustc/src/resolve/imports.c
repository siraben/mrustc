#include "cm/resolve/imports.h"

#include "cm/alloc.h"
#include "cm/interner.h"
#include "cm/vec.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmImportBinding {
    CmResolvedBinding value;
    int priority;
} CmImportBinding;

typedef struct CmImportModuleState {
    CmModuleId id;
    CmModuleId parent;
    CmResolveStringId name;
    CmVec children;
    CmVec namespaces[3];
} CmImportModuleState;

typedef struct CmImportEnumVariant {
    CmResolveStringId name;
    CmResolveVariantRef declaration;
    CmAstFieldForm form;
} CmImportEnumVariant;

typedef struct CmImportEnumState {
    CmModuleId module;
    CmResolveItemRef declaration;
    CmVec variants;
} CmImportEnumState;

typedef struct CmImportLeaf {
    CmModuleId module;
    CmResolveItemRef declaration;
    CmVec segments;
    CmVec bindings;
    CmResolveStringId alias;
    CmResolveStringId import_name;
    int absolute;
    int is_glob;
    int is_anonymous;
    int is_public;
    int is_crate_visible;
    int ever_resolved;
    int saw_ambiguous;
    CmModuleId blocked_module;
    CmResolveStringId blocked_name;
} CmImportLeaf;

typedef struct CmImportDependency {
    char name[64];
    const CmImportResolver *resolver;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    CmModuleId root;
} CmImportDependency;

typedef struct CmImportResolverState {
    uint64_t lifetime_id;
    uint64_t generation;
    uint64_t graph_lifetime_id;
    CmInterner strings;
    CmVec modules;
    CmVec enumerations;
    CmVec leaves;
    CmVec prelude_bindings;
    CmVec errors;
    CmResolveItemRef prelude_declaration;
    int prelude_invalid;
    CmModuleId root;
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    /* Registered external crates; survives clear/resolve cycles. */
    CmVec dependencies; /* CmImportDependency */
} CmImportResolverState;

static uint64_t cm_import_resolver_lifetime_counter;

static uint64_t cm_import_resolver_new_lifetime_id(void)
{
    if (cm_import_resolver_lifetime_counter == UINT64_MAX) abort();
    cm_import_resolver_lifetime_counter += UINT64_C(1);
    return cm_import_resolver_lifetime_counter;
}

typedef enum CmImportTokenKind {
    CM_IMPORT_TOKEN_END = 0,
    CM_IMPORT_TOKEN_NAME,
    CM_IMPORT_TOKEN_COLON2,
    CM_IMPORT_TOKEN_LBRACE,
    CM_IMPORT_TOKEN_RBRACE,
    CM_IMPORT_TOKEN_COMMA,
    CM_IMPORT_TOKEN_STAR,
    CM_IMPORT_TOKEN_AS,
    CM_IMPORT_TOKEN_INVALID
} CmImportTokenKind;

typedef struct CmImportToken {
    CmImportTokenKind kind;
    const unsigned char *bytes;
    size_t length;
} CmImportToken;

typedef struct CmImportParser {
    CmImportResolverState *state;
    const unsigned char *bytes;
    size_t length;
    size_t position;
    CmImportToken lookahead;
    int has_lookahead;
    int failed;
    CmModuleId module;
    CmResolveItemRef declaration;
    int is_public;
    int is_crate_visible;
} CmImportParser;

static CmImportResolverState *cm_import_state(CmImportResolver *resolver)
{
    return resolver == NULL ? NULL :
        (CmImportResolverState *)resolver->state;
}

static const CmImportResolverState *cm_import_state_const(
    const CmImportResolver *resolver)
{
    return resolver == NULL ? NULL :
        (const CmImportResolverState *)resolver->state;
}

static void cm_import_state_init(CmImportResolverState *state)
{
    memset(state, 0, sizeof(*state));
    cm_interner_init(&state->strings, 4096u);
    cm_vec_init(&state->modules, sizeof(CmImportModuleState));
    cm_vec_init(&state->enumerations, sizeof(CmImportEnumState));
    cm_vec_init(&state->leaves, sizeof(CmImportLeaf));
    cm_vec_init(&state->prelude_bindings, sizeof(CmResolvedBinding));
    cm_vec_init(&state->errors, sizeof(CmImportError));
    cm_vec_init(&state->dependencies, sizeof(CmImportDependency));
}

static void cm_import_state_clear(CmImportResolverState *state)
{
    size_t index;
    uint64_t lifetime_id;
    uint64_t generation;
    CmVec dependencies;

    lifetime_id = state->lifetime_id;
    generation = state->generation;
    dependencies = state->dependencies;

    for (index = 0u; index < state->modules.len; ++index) {
        CmImportModuleState *module;
        int namespace_index;

        module = (CmImportModuleState *)cm_vec_at(&state->modules, index);
        if (module == NULL) continue;
        cm_vec_destroy(&module->children);
        for (namespace_index = 0; namespace_index < 3; ++namespace_index)
            cm_vec_destroy(&module->namespaces[namespace_index]);
    }
    for (index = 0u; index < state->leaves.len; ++index) {
        CmImportLeaf *leaf;

        leaf = (CmImportLeaf *)cm_vec_at(&state->leaves, index);
        if (leaf != NULL) {
            cm_vec_destroy(&leaf->bindings);
            cm_vec_destroy(&leaf->segments);
        }
    }
    for (index = 0u; index < state->enumerations.len; ++index) {
        CmImportEnumState *enumeration;

        enumeration = (CmImportEnumState *)cm_vec_at(
            &state->enumerations, index);
        if (enumeration != NULL) cm_vec_destroy(&enumeration->variants);
    }
    cm_vec_destroy(&state->errors);
    cm_vec_destroy(&state->prelude_bindings);
    cm_vec_destroy(&state->leaves);
    cm_vec_destroy(&state->modules);
    cm_vec_destroy(&state->enumerations);
    cm_interner_destroy(&state->strings);
    cm_import_state_init(state);
    state->lifetime_id = lifetime_id;
    state->generation = generation;
    state->dependencies = dependencies;
}

static void cm_import_state_destroy(CmImportResolverState *state)
{
    if (state == NULL) return;
    cm_import_state_clear(state);
    cm_vec_destroy(&state->errors);
    cm_vec_destroy(&state->prelude_bindings);
    cm_vec_destroy(&state->leaves);
    cm_vec_destroy(&state->modules);
    cm_vec_destroy(&state->enumerations);
    cm_vec_destroy(&state->dependencies);
    cm_interner_destroy(&state->strings);
    memset(state, 0, sizeof(*state));
}

void cm_import_resolver_init(CmImportResolver *resolver)
{
    CmImportResolverState *state;

    if (resolver == NULL) return;
    state = (CmImportResolverState *)cm_alloc_zeroed(1u, sizeof(*state));
    cm_import_state_init(state);
    state->lifetime_id = cm_import_resolver_new_lifetime_id();
    resolver->state = state;
}

void cm_import_resolver_destroy(CmImportResolver *resolver)
{
    CmImportResolverState *state;

    state = cm_import_state(resolver);
    if (state == NULL) return;
    cm_import_state_destroy(state);
    cm_free(state);
    resolver->state = NULL;
}

static const CmInternedString *cm_import_string(
    const CmImportResolverState *state, CmResolveStringId id)
{
    return cm_interner_get(&state->strings, (CmInternId)id);
}

static int cm_import_string_is(const CmImportResolverState *state,
    CmResolveStringId id, const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_import_string(state, id);
    length = strlen(text);
    return string != NULL && string->len == length &&
        memcmp(string->bytes, text, length) == 0;
}

static CmResolveStringId cm_import_intern_graph_string(
    CmImportResolverState *state, const CmModuleGraph *graph,
    CmResolveStringId id)
{
    size_t length;
    char *buffer;
    CmResolveStringId result;

    if (id == CM_RESOLVE_STRING_NONE) return CM_RESOLVE_STRING_NONE;
    length = cm_module_graph_string_length(graph, id);
    buffer = (char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, id, buffer, length + 1u)) {
        cm_free(buffer);
        return CM_RESOLVE_STRING_NONE;
    }
    result = (CmResolveStringId)cm_interner_intern(&state->strings,
        buffer, length);
    cm_free(buffer);
    return result;
}

static int cm_import_effective_attribute_is(
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmModuleId module, uint32_t item_index, uint32_t attribute_index,
    const char *expected)
{
    CmResolveEffectiveAttribute attribute;
    size_t expected_length;
    size_t actual_length;
    char buffer[64];

    expected_length = strlen(expected);
    if (expected_length + 1u > sizeof(buffer)
        || cm_module_graph_get_effective_attribute(graph, revision,
            module, item_index, attribute_index, &attribute)
            != CM_RESOLVE_VIEW_OK) {
        return -1;
    }
    actual_length = cm_module_graph_string_length(graph,
        attribute.metadata);
    if (actual_length != expected_length) return 0;
    if (!cm_module_graph_copy_string(graph, attribute.metadata, buffer,
            sizeof(buffer))) {
        return -1;
    }
    return memcmp(buffer, expected, expected_length) == 0;
}

static CmImportModuleState *cm_import_module(CmImportResolverState *state,
    CmModuleId id)
{
    if (id == CM_MODULE_NONE) return NULL;
    return (CmImportModuleState *)cm_vec_at(&state->modules,
        (size_t)id - 1u);
}

static const CmImportModuleState *cm_import_module_const(
    const CmImportResolverState *state, CmModuleId id)
{
    if (state == NULL || id == CM_MODULE_NONE) return NULL;
    return (const CmImportModuleState *)cm_vec_at_const(&state->modules,
        (size_t)id - 1u);
}

static int cm_item_ref_equal(CmResolveItemRef left, CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static const CmImportEnumState *cm_import_enumeration_const(
    const CmImportResolverState *state, CmResolveItemRef declaration)
{
    size_t index;

    if (state == NULL || declaration.source == 0u
        || declaration.item == CM_AST_ITEM_NONE) return NULL;
    for (index = 0u; index < state->enumerations.len; ++index) {
        const CmImportEnumState *enumeration;

        enumeration = (const CmImportEnumState *)cm_vec_at_const(
            &state->enumerations, index);
        if (enumeration != NULL
            && cm_item_ref_equal(enumeration->declaration, declaration)) {
            return enumeration;
        }
    }
    return NULL;
}

static int cm_variant_ref_equal(CmResolveVariantRef left,
    CmResolveVariantRef right)
{
    return cm_item_ref_equal(left.enumeration, right.enumeration)
        && (left.enumeration.source == 0u || left.index == right.index);
}

static int cm_binding_target_equal(const CmResolvedBinding *left,
    const CmResolvedBinding *right)
{
    return left->target_module == right->target_module &&
        left->primitive_kind == right->primitive_kind &&
        left->item_kind == right->item_kind &&
        cm_item_ref_equal(left->declaration, right->declaration) &&
        cm_variant_ref_equal(left->variant, right->variant);
}

static int cm_leaf_binding_equal(const CmResolvedBinding *left,
    const CmResolvedBinding *right)
{
    return left->module == right->module && left->name == right->name &&
        left->namespace_kind == right->namespace_kind &&
        left->item_kind == right->item_kind &&
        left->primitive_kind == right->primitive_kind &&
        left->target_module == right->target_module &&
        cm_item_ref_equal(left->declaration, right->declaration) &&
        cm_variant_ref_equal(left->variant, right->variant) &&
        cm_item_ref_equal(left->import_declaration,
            right->import_declaration) &&
        left->is_public == right->is_public &&
        left->is_crate_visible == right->is_crate_visible &&
        left->is_import == right->is_import &&
        left->is_reexport == right->is_reexport &&
        left->is_ambiguous == right->is_ambiguous &&
        left->is_anonymous == right->is_anonymous;
}

static void cm_record_leaf_binding(CmImportLeaf *leaf,
    const CmResolvedBinding *binding)
{
    size_t index;

    if (leaf == NULL || binding == NULL) return;
    for (index = 0u; index < leaf->bindings.len; ++index) {
        const CmResolvedBinding *existing;

        existing = (const CmResolvedBinding *)cm_vec_at_const(
            &leaf->bindings, index);
        if (existing != NULL && cm_leaf_binding_equal(existing, binding))
            return;
    }
    (void)cm_vec_push(&leaf->bindings, binding);
}

static CmImportBinding *cm_find_binding(CmImportModuleState *module,
    CmResolveNamespace namespace_kind, CmResolveStringId name)
{
    CmVec *bindings;
    size_t index;

    if (module == NULL || namespace_kind < CM_RESOLVE_NAMESPACE_TYPE ||
        namespace_kind > CM_RESOLVE_NAMESPACE_MACRO) return NULL;
    bindings = &module->namespaces[(int)namespace_kind];
    for (index = 0u; index < bindings->len; ++index) {
        CmImportBinding *binding;

        binding = (CmImportBinding *)cm_vec_at(bindings, index);
        if (binding != NULL && binding->value.name == name) return binding;
    }
    return NULL;
}

static const CmImportBinding *cm_find_binding_const(
    const CmImportModuleState *module, CmResolveNamespace namespace_kind,
    CmResolveStringId name)
{
    const CmVec *bindings;
    size_t index;

    if (module == NULL || namespace_kind < CM_RESOLVE_NAMESPACE_TYPE ||
        namespace_kind > CM_RESOLVE_NAMESPACE_MACRO) return NULL;
    bindings = &module->namespaces[(int)namespace_kind];
    for (index = 0u; index < bindings->len; ++index) {
        const CmImportBinding *binding;

        binding = (const CmImportBinding *)cm_vec_at_const(bindings, index);
        if (binding != NULL && binding->value.name == name) return binding;
    }
    return NULL;
}

static int cm_module_is_ancestor(const CmImportResolverState *state,
    CmModuleId ancestor, CmModuleId module)
{
    while (module != CM_MODULE_NONE) {
        const CmImportModuleState *entry;

        if (module == ancestor) return 1;
        entry = cm_import_module_const(state, module);
        if (entry == NULL) break;
        module = entry->parent;
    }
    return 0;
}

static int cm_glob_binding_is_visible(const CmImportResolverState *state,
    const CmImportLeaf *leaf, CmModuleId source_module,
    const CmResolvedBinding *binding)
{
    if (state == NULL || leaf == NULL || binding == NULL) return 0;
    if (leaf->is_public) return binding->is_public;
    if (leaf->is_crate_visible) {
        return binding->is_public || binding->is_crate_visible;
    }
    return binding->is_public || binding->is_crate_visible
        || cm_module_is_ancestor(state, source_module, leaf->module);
}

static void cm_import_apply_leaf_visibility(CmResolvedBinding *binding,
    const CmImportLeaf *leaf)
{
    int source_is_public;
    int source_is_crate_visible;

    if (binding == NULL || leaf == NULL) return;
    if (binding->namespace_kind == CM_RESOLVE_NAMESPACE_MACRO) {
        binding->is_public = leaf->is_public;
        binding->is_crate_visible = leaf->is_crate_visible;
        binding->is_reexport = leaf->is_public;
        return;
    }
    source_is_public = binding->is_public;
    source_is_crate_visible = binding->is_crate_visible;
    binding->is_public = leaf->is_public && source_is_public;
    binding->is_crate_visible = leaf->is_crate_visible
        && source_is_crate_visible;
    binding->is_reexport = binding->is_public;
}

static int cm_add_binding(CmImportModuleState *module,
    const CmResolvedBinding *value, int priority)
{
    CmImportBinding *existing;
    CmImportBinding added;

    existing = cm_find_binding(module, value->namespace_kind, value->name);
    if (existing == NULL) {
        memset(&added, 0, sizeof(added));
        added.value = *value;
        added.priority = priority;
        (void)cm_vec_push(&module->namespaces[(int)value->namespace_kind],
            &added);
        return 1;
    }
    if (cm_binding_target_equal(&existing->value, value)) {
        int changed;

        changed = 0;
        if (priority > existing->priority) {
            existing->priority = priority;
            changed = 1;
        }
        if (value->is_public && !existing->value.is_public) {
            existing->value.is_public = 1;
            changed = 1;
        }
        if (value->is_crate_visible
            && !existing->value.is_crate_visible) {
            existing->value.is_crate_visible = 1;
            changed = 1;
        }
        if (value->is_reexport && !existing->value.is_reexport) {
            existing->value.is_reexport = 1;
            changed = 1;
        }
        return changed;
    }
    if (priority < existing->priority) {
        /* Glob imports are shadowable; explicit imports conflict with items. */
        if (priority == 2 && existing->priority == 3 &&
            !existing->value.is_ambiguous) {
            existing->value.is_ambiguous = 1;
            existing->value.import_declaration = value->import_declaration;
            return 1;
        }
        return 0;
    }
    if (priority > existing->priority) {
        existing->value = *value;
        existing->priority = priority;
        return 1;
    }
    if (!existing->value.is_ambiguous) {
        existing->value.is_ambiguous = 1;
        return 1;
    }
    return 0;
}

static void cm_push_error(CmImportResolverState *state,
    CmImportErrorKind kind, const CmImportLeaf *leaf,
    CmResolveStringId name, CmResolveStringId detail)
{
    CmImportError error;

    memset(&error, 0, sizeof(error));
    error.kind = kind;
    if (leaf != NULL) {
        error.module = leaf->module;
        error.import_declaration = leaf->declaration;
    }
    error.name = name;
    error.detail = detail;
    (void)cm_vec_push(&state->errors, &error);
}

static int cm_import_name_start(unsigned char byte)
{
    return (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
        (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
        byte == (unsigned char)'_';
}

static int cm_import_name_continue(unsigned char byte)
{
    return cm_import_name_start(byte) ||
        (byte >= (unsigned char)'0' && byte <= (unsigned char)'9');
}

static CmImportToken cm_import_scan(CmImportParser *parser)
{
    CmImportToken token;
    size_t start;

    memset(&token, 0, sizeof(token));
    while (parser->position < parser->length &&
        (parser->bytes[parser->position] == (unsigned char)' ' ||
         parser->bytes[parser->position] == (unsigned char)'\t' ||
         parser->bytes[parser->position] == (unsigned char)'\r' ||
         parser->bytes[parser->position] == (unsigned char)'\n')) {
        ++parser->position;
    }
    if (parser->position == parser->length) {
        token.kind = CM_IMPORT_TOKEN_END;
        return token;
    }
    start = parser->position;
    if (parser->bytes[start] == (unsigned char)':' &&
        start + 1u < parser->length &&
        parser->bytes[start + 1u] == (unsigned char)':') {
        parser->position += 2u;
        token.kind = CM_IMPORT_TOKEN_COLON2;
    } else if (parser->bytes[start] == (unsigned char)'{') {
        ++parser->position;
        token.kind = CM_IMPORT_TOKEN_LBRACE;
    } else if (parser->bytes[start] == (unsigned char)'}') {
        ++parser->position;
        token.kind = CM_IMPORT_TOKEN_RBRACE;
    } else if (parser->bytes[start] == (unsigned char)',') {
        ++parser->position;
        token.kind = CM_IMPORT_TOKEN_COMMA;
    } else if (parser->bytes[start] == (unsigned char)'*') {
        ++parser->position;
        token.kind = CM_IMPORT_TOKEN_STAR;
    } else if (cm_import_name_start(parser->bytes[start])) {
        ++parser->position;
        while (parser->position < parser->length &&
            cm_import_name_continue(parser->bytes[parser->position])) {
            ++parser->position;
        }
        token.bytes = parser->bytes + start;
        token.length = parser->position - start;
        token.kind = token.length == 2u &&
            memcmp(token.bytes, "as", 2u) == 0 ?
            CM_IMPORT_TOKEN_AS : CM_IMPORT_TOKEN_NAME;
    } else {
        ++parser->position;
        token.kind = CM_IMPORT_TOKEN_INVALID;
    }
    return token;
}

static CmImportToken cm_import_peek(CmImportParser *parser)
{
    if (!parser->has_lookahead) {
        parser->lookahead = cm_import_scan(parser);
        parser->has_lookahead = 1;
    }
    return parser->lookahead;
}

static CmImportToken cm_import_take(CmImportParser *parser)
{
    CmImportToken token;

    token = cm_import_peek(parser);
    parser->has_lookahead = 0;
    return token;
}

static int cm_import_accept(CmImportParser *parser,
    CmImportTokenKind kind)
{
    if (cm_import_peek(parser).kind != kind) return 0;
    (void)cm_import_take(parser);
    return 1;
}

static CmResolveStringId cm_import_token_id(CmImportParser *parser,
    CmImportToken token)
{
    return (CmResolveStringId)cm_interner_intern(&parser->state->strings,
        token.bytes, token.length);
}

static void cm_import_emit_leaf(CmImportParser *parser, const CmVec *prefix,
    int absolute, int is_glob, CmResolveStringId alias)
{
    CmImportLeaf leaf;
    const CmResolveStringId *first;
    const CmResolveStringId *last;
    size_t index;

    if (prefix->len == 0u && !is_glob) {
        parser->failed = 1;
        return;
    }
    first = prefix->len == 0u ? NULL
        : (const CmResolveStringId *)cm_vec_at_const(prefix, 0u);
    last = prefix->len == 0u ? NULL
        : (const CmResolveStringId *)cm_vec_at_const(prefix,
            prefix->len - 1u);
    if (!is_glob && alias == CM_RESOLVE_STRING_NONE
        && prefix->len == 2u && first != NULL && last != NULL
        && cm_import_string_is(parser->state, *first, "crate")
        && cm_import_string_is(parser->state, *last, "self")) {
        /* The crate root has no bindable local name; Rust requires `as`. */
        parser->failed = 1;
        return;
    }
    memset(&leaf, 0, sizeof(leaf));
    leaf.module = parser->module;
    leaf.declaration = parser->declaration;
    cm_vec_init(&leaf.segments, sizeof(CmResolveStringId));
    cm_vec_init(&leaf.bindings, sizeof(CmResolvedBinding));
    cm_vec_append(&leaf.segments, prefix->data, prefix->len);
    leaf.alias = alias;
    leaf.absolute = absolute;
    leaf.is_glob = is_glob;
    leaf.is_anonymous = alias != CM_RESOLVE_STRING_NONE
        && cm_import_string_is(parser->state, alias, "_");
    leaf.is_public = parser->is_public;
    leaf.is_crate_visible = parser->is_crate_visible;
    if (alias != CM_RESOLVE_STRING_NONE) {
        leaf.import_name = alias;
    } else if (!is_glob && prefix->len != 0u) {
        last = (const CmResolveStringId *)cm_vec_at_const(prefix,
            prefix->len - 1u);
        if (last != NULL && cm_import_string_is(parser->state, *last,
            "self") && prefix->len >= 2u) {
            last = (const CmResolveStringId *)cm_vec_at_const(prefix,
                prefix->len - 2u);
        }
        if (last != NULL) leaf.import_name = *last;
    }
    index = parser->state->leaves.len;
    (void)index;
    (void)cm_vec_push(&parser->state->leaves, &leaf);
}

static void cm_import_parse_tree(CmImportParser *parser, CmVec *prefix,
    int absolute);

static void cm_import_parse_group(CmImportParser *parser, CmVec *prefix,
    int absolute)
{
    if (cm_import_accept(parser, CM_IMPORT_TOKEN_RBRACE)) {
        parser->failed = 1;
        return;
    }
    for (;;) {
        cm_import_parse_tree(parser, prefix, absolute);
        if (parser->failed) return;
        if (cm_import_accept(parser, CM_IMPORT_TOKEN_RBRACE)) return;
        if (!cm_import_accept(parser, CM_IMPORT_TOKEN_COMMA)) {
            parser->failed = 1;
            return;
        }
        if (cm_import_accept(parser, CM_IMPORT_TOKEN_RBRACE)) return;
    }
}

static void cm_import_parse_tree(CmImportParser *parser, CmVec *prefix,
    int absolute)
{
    size_t original_length;
    CmImportToken token;
    CmResolveStringId name;

    original_length = prefix->len;
    if (original_length == 0u &&
        cm_import_accept(parser, CM_IMPORT_TOKEN_COLON2)) absolute = 1;
    if (cm_import_accept(parser, CM_IMPORT_TOKEN_LBRACE)) {
        cm_import_parse_group(parser, prefix, absolute);
        prefix->len = original_length;
        return;
    }
    if (cm_import_accept(parser, CM_IMPORT_TOKEN_STAR)) {
        cm_import_emit_leaf(parser, prefix, absolute, 1,
            CM_RESOLVE_STRING_NONE);
        prefix->len = original_length;
        return;
    }
    token = cm_import_take(parser);
    if (token.kind != CM_IMPORT_TOKEN_NAME) {
        parser->failed = 1;
        prefix->len = original_length;
        return;
    }
    name = cm_import_token_id(parser, token);
    (void)cm_vec_push(prefix, &name);
    if (cm_import_accept(parser, CM_IMPORT_TOKEN_AS)) {
        CmImportToken alias_token;
        CmResolveStringId alias;

        alias_token = cm_import_take(parser);
        if (alias_token.kind != CM_IMPORT_TOKEN_NAME) {
            parser->failed = 1;
        } else {
            alias = cm_import_token_id(parser, alias_token);
            cm_import_emit_leaf(parser, prefix, absolute, 0, alias);
        }
    } else if (cm_import_accept(parser, CM_IMPORT_TOKEN_COLON2)) {
        if (cm_import_accept(parser, CM_IMPORT_TOKEN_LBRACE)) {
            cm_import_parse_group(parser, prefix, absolute);
        } else if (cm_import_accept(parser, CM_IMPORT_TOKEN_STAR)) {
            cm_import_emit_leaf(parser, prefix, absolute, 1,
                CM_RESOLVE_STRING_NONE);
        } else {
            cm_import_parse_tree(parser, prefix, absolute);
        }
    } else {
        cm_import_emit_leaf(parser, prefix, absolute, 0,
            CM_RESOLVE_STRING_NONE);
    }
    prefix->len = original_length;
}

static int cm_parse_import(CmImportResolverState *state,
    const CmModuleGraph *graph, CmModuleId module,
    const CmResolveImport *import_directive)
{
    CmImportParser parser;
    CmVec prefix;
    size_t length;
    char *buffer;
    size_t old_leaf_count;

    length = cm_module_graph_string_length(graph, import_directive->tree);
    buffer = (char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, import_directive->tree,
        buffer, length + 1u)) {
        cm_free(buffer);
        return 0;
    }
    memset(&parser, 0, sizeof(parser));
    parser.state = state;
    parser.bytes = (const unsigned char *)buffer;
    parser.length = length;
    parser.module = module;
    parser.declaration = import_directive->declaration;
    parser.is_public = import_directive->visibility == CM_AST_VIS_PUBLIC;
    parser.is_crate_visible = parser.is_public
        || import_directive->visibility == CM_AST_VIS_CRATE;
    old_leaf_count = state->leaves.len;
    cm_vec_init(&prefix, sizeof(CmResolveStringId));
    cm_import_parse_tree(&parser, &prefix, 0);
    if (!parser.failed && cm_import_peek(&parser).kind !=
        CM_IMPORT_TOKEN_END) parser.failed = 1;
    cm_vec_destroy(&prefix);
    cm_free(buffer);
    if (parser.failed) {
        while (state->leaves.len > old_leaf_count) {
            CmImportLeaf leaf;

            if (cm_vec_pop(&state->leaves, &leaf)) {
                cm_vec_destroy(&leaf.bindings);
                cm_vec_destroy(&leaf.segments);
            }
        }
        return 0;
    }
    return 1;
}

static CmModuleId cm_direct_child(const CmImportResolverState *state,
    CmModuleId module_id, CmResolveStringId name)
{
    const CmImportModuleState *module;
    size_t index;

    module = cm_import_module_const(state, module_id);
    if (module == NULL) return CM_MODULE_NONE;
    for (index = 0u; index < module->children.len; ++index) {
        const CmModuleId *child_id;
        const CmImportModuleState *child;

        child_id = (const CmModuleId *)cm_vec_at_const(&module->children,
            index);
        child = child_id == NULL ? NULL :
            cm_import_module_const(state, *child_id);
        if (child != NULL && child->name == name) return child->id;
    }
    return CM_MODULE_NONE;
}

static const CmImportBinding *cm_extern_prelude_binding(
    const CmImportResolverState *state, CmResolveStringId name)
{
    const CmImportModuleState *root;
    const CmImportBinding *binding;

    root = cm_import_module_const(state, state->root);
    binding = cm_find_binding_const(root, CM_RESOLVE_NAMESPACE_TYPE, name);
    if (binding == NULL
        || binding->value.item_kind != CM_AST_ITEM_EXTERN_CRATE
        || binding->value.target_module == CM_MODULE_NONE) return NULL;
    return binding;
}

static CmResolvePrimitiveKind cm_builtin_primitive_kind(
    const CmImportResolverState *state, CmResolveStringId name)
{
    if (cm_import_string_is(state, name, "bool"))
        return CM_RESOLVE_PRIMITIVE_BOOL;
    if (cm_import_string_is(state, name, "char"))
        return CM_RESOLVE_PRIMITIVE_CHAR;
    if (cm_import_string_is(state, name, "str"))
        return CM_RESOLVE_PRIMITIVE_STR;
    if (cm_import_string_is(state, name, "i8"))
        return CM_RESOLVE_PRIMITIVE_I8;
    if (cm_import_string_is(state, name, "i16"))
        return CM_RESOLVE_PRIMITIVE_I16;
    if (cm_import_string_is(state, name, "i32"))
        return CM_RESOLVE_PRIMITIVE_I32;
    if (cm_import_string_is(state, name, "i64"))
        return CM_RESOLVE_PRIMITIVE_I64;
    if (cm_import_string_is(state, name, "i128"))
        return CM_RESOLVE_PRIMITIVE_I128;
    if (cm_import_string_is(state, name, "isize"))
        return CM_RESOLVE_PRIMITIVE_ISIZE;
    if (cm_import_string_is(state, name, "u8"))
        return CM_RESOLVE_PRIMITIVE_U8;
    if (cm_import_string_is(state, name, "u16"))
        return CM_RESOLVE_PRIMITIVE_U16;
    if (cm_import_string_is(state, name, "u32"))
        return CM_RESOLVE_PRIMITIVE_U32;
    if (cm_import_string_is(state, name, "u64"))
        return CM_RESOLVE_PRIMITIVE_U64;
    if (cm_import_string_is(state, name, "u128"))
        return CM_RESOLVE_PRIMITIVE_U128;
    if (cm_import_string_is(state, name, "usize"))
        return CM_RESOLVE_PRIMITIVE_USIZE;
    if (cm_import_string_is(state, name, "f16"))
        return CM_RESOLVE_PRIMITIVE_F16;
    if (cm_import_string_is(state, name, "f32"))
        return CM_RESOLVE_PRIMITIVE_F32;
    if (cm_import_string_is(state, name, "f64"))
        return CM_RESOLVE_PRIMITIVE_F64;
    if (cm_import_string_is(state, name, "f128"))
        return CM_RESOLVE_PRIMITIVE_F128;
    return CM_RESOLVE_PRIMITIVE_NONE;
}

static int cm_builtin_primitive_binding(const CmImportResolverState *state,
    CmModuleId module, CmResolveStringId name,
    CmResolvedBinding *out_binding)
{
    CmResolvePrimitiveKind primitive_kind;

    if (out_binding == NULL) return 0;
    primitive_kind = cm_builtin_primitive_kind(state, name);
    if (primitive_kind == CM_RESOLVE_PRIMITIVE_NONE) return 0;
    memset(out_binding, 0, sizeof(*out_binding));
    out_binding->revision = state->revision;
    out_binding->module = module;
    out_binding->name = name;
    out_binding->namespace_kind = CM_RESOLVE_NAMESPACE_TYPE;
    out_binding->primitive_kind = primitive_kind;
    out_binding->is_public = 1;
    out_binding->is_crate_visible = 1;
    return 1;
}

static void cm_intern_builtin_primitive_names(CmImportResolverState *state)
{
    static const char *const names[] = {
        "bool", "char", "str",
        "i8", "i16", "i32", "i64", "i128", "isize",
        "u8", "u16", "u32", "u64", "u128", "usize",
        "f16", "f32", "f64", "f128"
    };
    size_t index;

    for (index = 0u; index < CM_ARRAY_LEN(names); ++index)
        (void)cm_interner_intern_c_str(&state->strings, names[index]);
}

typedef enum CmImportScopeKind {
    CM_IMPORT_SCOPE_NONE = 0,
    CM_IMPORT_SCOPE_MODULE,
    CM_IMPORT_SCOPE_ENUM
} CmImportScopeKind;

typedef struct CmImportScope {
    CmImportScopeKind kind;
    CmModuleId module;
    const CmImportEnumState *enumeration;
    CmResolvedBinding binding;
} CmImportScope;

static CmImportScope cm_resolve_scope_segments(
    const CmImportResolverState *state, const CmImportLeaf *leaf,
    size_t count, CmModuleId *blocked_module,
    CmResolveStringId *blocked_name, int *ambiguous)
{
    CmImportScope result;
    CmModuleId current;
    size_t index;

    memset(&result, 0, sizeof(result));
    current = leaf->absolute ? state->root : leaf->module;
    index = 0u;
    if (count != 0u) {
        const CmResolveStringId *first;

        first = (const CmResolveStringId *)cm_vec_at_const(&leaf->segments,
            0u);
        if (first != NULL && cm_import_string_is(state, *first, "crate")) {
            current = state->root;
            index = 1u;
        } else if (first != NULL &&
            cm_import_string_is(state, *first, "self")) {
            index = 1u;
        }
    }
    while (index < count) {
        const CmResolveStringId *segment;
        const CmImportModuleState *module;

        segment = (const CmResolveStringId *)cm_vec_at_const(&leaf->segments,
            index);
        if (segment == NULL) return result;
        if (cm_import_string_is(state, *segment, "super")) {
            module = cm_import_module_const(state, current);
            if (module == NULL || module->parent == CM_MODULE_NONE) {
                *blocked_module = current;
                *blocked_name = *segment;
                return result;
            }
            current = module->parent;
        } else if (cm_import_string_is(state, *segment, "self")) {
            /* `self` is meaningful as an initial or final segment only. */
            if (index + 1u != count) {
                *blocked_module = current;
                *blocked_name = *segment;
                return result;
            }
        } else if (cm_import_string_is(state, *segment, "crate")) {
            *blocked_module = current;
            *blocked_name = *segment;
            return result;
        } else {
            const CmImportModuleState *current_module;
            const CmImportBinding *binding;

            current_module = cm_import_module_const(state, current);
            binding = cm_find_binding_const(current_module,
                CM_RESOLVE_NAMESPACE_TYPE, *segment);
            if (binding == NULL)
                binding = cm_extern_prelude_binding(state, *segment);
            if (binding != NULL && binding->value.is_ambiguous) {
                *ambiguous = 1;
                *blocked_module = current;
                *blocked_name = *segment;
                return result;
            }
            if (binding == NULL) {
                *blocked_module = current;
                *blocked_name = *segment;
                return result;
            }
            if (binding->value.target_module != CM_MODULE_NONE) {
                current = binding->value.target_module;
            } else if (index + 1u == count
                && binding->value.item_kind == CM_AST_ITEM_ENUM) {
                result.enumeration = cm_import_enumeration_const(state,
                    binding->value.declaration);
                if (result.enumeration == NULL) {
                    *blocked_module = current;
                    *blocked_name = *segment;
                    return result;
                }
                result.kind = CM_IMPORT_SCOPE_ENUM;
                result.module = result.enumeration->module;
                result.binding = binding->value;
                return result;
            } else {
                *blocked_module = current;
                *blocked_name = *segment;
                return result;
            }
        }
        ++index;
    }
    result.kind = CM_IMPORT_SCOPE_MODULE;
    result.module = current;
    return result;
}

static const CmImportBinding *cm_module_self_binding(
    const CmImportResolverState *state, CmModuleId module_id)
{
    const CmImportModuleState *module;
    const CmImportModuleState *parent;

    module = cm_import_module_const(state, module_id);
    if (module == NULL || module->parent == CM_MODULE_NONE) return NULL;
    parent = cm_import_module_const(state, module->parent);
    return cm_find_binding_const(parent, CM_RESOLVE_NAMESPACE_TYPE,
        module->name);
}

int cm_import_resolver_add_dependency(CmImportResolver *resolver,
    const char *name, const CmImportResolver *dependency,
    const CmModuleGraph *dependency_graph,
    CmModuleGraphRevision dependency_revision)
{
    CmImportResolverState *state = cm_import_state(resolver);
    CmImportDependency entry;
    if (state == NULL || name == NULL || dependency == NULL
        || dependency_graph == NULL
        || strlen(name) >= sizeof(entry.name)) return 0;
    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, name);
    entry.resolver = dependency;
    entry.graph = dependency_graph;
    entry.revision = dependency_revision;
    if (!cm_module_graph_get_root(dependency_graph, &entry.root)) return 0;
    return cm_vec_push(&state->dependencies, &entry) != NULL;
}

static const CmImportDependency *cm_import_find_dependency_view(
    const CmImportResolverState *state,
    const CmResolvePathSegmentView *segment)
{
    size_t index;
    for (index = 0u; index < state->dependencies.len; ++index) {
        const CmImportDependency *entry = (const CmImportDependency *)
            cm_vec_at_const(&state->dependencies, index);
        if (entry != NULL && segment->length == strlen(entry->name)
            && memcmp(segment->bytes, entry->name, segment->length) == 0)
            return entry;
    }
    return NULL;
}

static uint32_t cm_import_dependency_tag(
    const CmImportResolverState *state, const CmImportDependency *entry)
{
    return (uint32_t)(entry
        - (const CmImportDependency *)state->dependencies.data) + 1u;
}

static const CmImportDependency *cm_import_find_dependency(
    const CmImportResolverState *state, CmResolveStringId name)
{
    size_t index;
    for (index = 0u; index < state->dependencies.len; ++index) {
        const CmImportDependency *entry = (const CmImportDependency *)
            cm_vec_at_const(&state->dependencies, index);
        if (entry != NULL && cm_import_string_is(state, name, entry->name))
            return entry;
    }
    return NULL;
}

static size_t cm_import_segment_views(const CmImportResolverState *state,
    const CmResolveStringId *ids, size_t count,
    CmResolvePathSegmentView *views, size_t limit)
{
    size_t index;
    if (count > limit) return 0u;
    for (index = 0u; index < count; ++index) {
        const CmInternedString *text = cm_import_string(state, ids[index]);
        if (text == NULL) return 0u;
        views[index].bytes = text->bytes;
        views[index].length = text->len;
    }
    return count;
}

static CmResolveStringId cm_import_reintern_dependency_name(
    CmImportResolverState *state, const CmImportResolver *dep,
    CmResolveStringId dep_name)
{
    const CmImportResolverState *dep_state = cm_import_state_const(dep);
    const CmInternedString *text = dep_state == NULL ? NULL
        : cm_import_string(dep_state, dep_name);
    if (text == NULL) return CM_RESOLVE_STRING_NONE;
    return (CmResolveStringId)cm_interner_intern(&state->strings,
        text->bytes, text->len);
}

/*
 * Resolve one still-unresolved leaf whose first segment names a registered
 * external crate (M9-03): single, aliased, and glob use-trees.  Synthesized
 * bindings carry the dependency's source-qualified refs, valid across
 * graphs because both share one CmSourceSet.
 */
static int cm_resolve_leaf_dependency(CmImportResolverState *state,
    CmImportLeaf *leaf)
{
    const CmImportDependency *dep;
    CmImportModuleState *destination;
    const CmResolveStringId *ids;
    CmResolvePathSegmentView views[64];
    size_t count;
    uint32_t dependency_tag;
    int changed = 0;
    if (leaf->ever_resolved || leaf->segments.len == 0u
        || leaf->absolute) return 0;
    ids = (const CmResolveStringId *)leaf->segments.data;
    dep = cm_import_find_dependency(state, ids[0]);
    if (dep == NULL) return 0;
    {
        const CmImportDependency *base = (const CmImportDependency *)
            state->dependencies.data;
        dependency_tag = (uint32_t)(dep - base) + 1u;
    }
    destination = cm_import_module(state, leaf->module);
    if (destination == NULL) return 0;
    count = cm_import_segment_views(state, ids + 1u,
        leaf->segments.len - 1u, views, 64u);
    if (leaf->segments.len > 1u && count == 0u) return 0;
    if (!leaf->is_glob && count != 0u
        && views[count - 1u].length == 4u
        && memcmp(views[count - 1u].bytes, "self", 4u) == 0) {
        /*
         * `use core::x::{self, ...}` — drop the `self` segment and import
         * whatever the prefix names (module, enum, type) under the leaf's
         * published name via the namespace loop below.  A bare crate
         * `{self}` synthesizes the root module binding directly.
         */
        count -= 1u;
        if (count == 0u) {
            CmResolvedBinding imported;
            memset(&imported, 0, sizeof(imported));
            imported.item_kind = CM_AST_ITEM_MODULE;
            imported.is_public = 1;
            imported.is_crate_visible = 1;
            imported.revision = state->revision;
            imported.target_module = dep->root;
            imported.dependency = dependency_tag;
            imported.name = leaf->import_name;
            imported.module = leaf->module;
            imported.import_declaration = leaf->declaration;
            cm_import_apply_leaf_visibility(&imported, leaf);
            imported.is_import = 1;
            imported.is_ambiguous = 0;
            imported.is_anonymous = leaf->is_anonymous;
            cm_record_leaf_binding(leaf, &imported);
            changed = cm_add_binding(destination, &imported, 1);
            leaf->ever_resolved = 1;
            return changed;
        }
    }
    if (leaf->is_glob) {
        CmModuleId source_module = dep->root;
        int namespace_index;
        if (count != 0u) {
            CmResolvedBinding module_binding;
            if (cm_import_resolve_path_checked(dep->resolver, dep->graph,
                    dep->revision, dep->root, 0, views, count,
                    CM_RESOLVE_NAMESPACE_TYPE, &module_binding)
                    != CM_IMPORT_LOOKUP_OK
                || module_binding.target_module == CM_MODULE_NONE)
                return 0;
            source_module = module_binding.target_module;
        }
        for (namespace_index = 0; namespace_index < 3; ++namespace_index) {
            size_t binding_count = cm_import_binding_count(dep->resolver,
                source_module, (CmResolveNamespace)namespace_index);
            size_t index;
            for (index = 0u; index < binding_count; ++index) {
                CmResolvedBinding imported;
                if (!cm_import_get_binding(dep->resolver, source_module,
                        (CmResolveNamespace)namespace_index,
                        (uint32_t)index, &imported)) continue;
                if (!imported.is_public || imported.is_ambiguous) continue;
                imported.name = cm_import_reintern_dependency_name(state,
                    dep->resolver, imported.name);
                if (imported.name == CM_RESOLVE_STRING_NONE) continue;
                imported.revision = state->revision;
                imported.module = leaf->module;
                imported.import_declaration = leaf->declaration;
                imported.dependency = dependency_tag;
                cm_import_apply_leaf_visibility(&imported, leaf);
                imported.is_import = 1;
                imported.is_ambiguous = 0;
                imported.is_anonymous = leaf->is_anonymous;
                /* Macro semantics flow through the dependency-macro
                 * artifact; keep the namespace binding but leave it off
                 * the leaf so HIR import retention never stores it. */
                if ((CmResolveNamespace)namespace_index
                        != CM_RESOLVE_NAMESPACE_MACRO)
                    cm_record_leaf_binding(leaf, &imported);
                changed |= cm_add_binding(destination, &imported, 1);
            }
        }
        leaf->ever_resolved = 1;
        return changed;
    }
    {
        int namespace_index;
        int any = 0;
        for (namespace_index = 0; namespace_index < 3; ++namespace_index) {
            CmResolvedBinding imported;
            if (cm_import_resolve_path_checked(dep->resolver, dep->graph,
                    dep->revision, dep->root, 0, views, count,
                    (CmResolveNamespace)namespace_index, &imported)
                    != CM_IMPORT_LOOKUP_OK) continue;
            imported.revision = state->revision;
            imported.name = leaf->import_name;
            imported.module = leaf->module;
            imported.import_declaration = leaf->declaration;
            imported.dependency = dependency_tag;
            cm_import_apply_leaf_visibility(&imported, leaf);
            imported.is_import = 1;
            imported.is_ambiguous = 0;
            imported.is_anonymous = leaf->is_anonymous;
            if ((CmResolveNamespace)namespace_index
                    != CM_RESOLVE_NAMESPACE_MACRO)
                cm_record_leaf_binding(leaf, &imported);
            changed |= cm_add_binding(destination, &imported, 1);
            any = 1;
        }
        if (any) leaf->ever_resolved = 1;
    }
    return changed;
}

static int cm_resolve_leaf(CmImportResolverState *state, CmImportLeaf *leaf)
{
    CmImportModuleState *destination;
    CmModuleId blocked_module;
    CmResolveStringId blocked_name;
    int ambiguous;
    int changed;

    destination = cm_import_module(state, leaf->module);
    if (destination == NULL || leaf->segments.len == 0u) return 0;
    blocked_module = CM_MODULE_NONE;
    blocked_name = CM_RESOLVE_STRING_NONE;
    ambiguous = 0;
    changed = 0;
    if (leaf->is_glob) {
        CmImportScope scope;

        scope = cm_resolve_scope_segments(state, leaf,
            leaf->segments.len, &blocked_module, &blocked_name, &ambiguous);
        if (scope.kind == CM_IMPORT_SCOPE_MODULE) {
            const CmImportModuleState *source;
            int namespace_index;

            source = cm_import_module_const(state, scope.module);
            for (namespace_index = 0; namespace_index < 3;
                 ++namespace_index) {
                size_t index;

                for (index = 0u;
                     index < source->namespaces[namespace_index].len;
                     ++index) {
                    const CmImportBinding *source_binding;
                    CmResolvedBinding imported;

                    source_binding = (const CmImportBinding *)cm_vec_at_const(
                        &source->namespaces[namespace_index], index);
                    if (source_binding == NULL
                        || source_binding->value.is_ambiguous
                        || !cm_glob_binding_is_visible(state, leaf,
                            scope.module, &source_binding->value)) continue;
                    imported = source_binding->value;
                    imported.module = leaf->module;
                    imported.import_declaration = leaf->declaration;
                    cm_import_apply_leaf_visibility(&imported, leaf);
                    imported.is_import = 1;
                    imported.is_ambiguous = 0;
                    imported.is_anonymous = leaf->is_anonymous;
                    cm_record_leaf_binding(leaf, &imported);
                    changed |= cm_add_binding(destination, &imported, 1);
                }
            }
            leaf->ever_resolved = 1;
        } else if (scope.kind == CM_IMPORT_SCOPE_ENUM) {
            size_t variant_index;

            for (variant_index = 0u;
                 variant_index < scope.enumeration->variants.len;
                 ++variant_index) {
                const CmImportEnumVariant *variant;
                int namespace_index;

                variant = (const CmImportEnumVariant *)cm_vec_at_const(
                    &scope.enumeration->variants, variant_index);
                if (variant == NULL) continue;
                for (namespace_index = 0; namespace_index < 2;
                     ++namespace_index) {
                    CmResolvedBinding imported;

                    if (namespace_index == (int)CM_RESOLVE_NAMESPACE_VALUE
                        && variant->form == CM_AST_FIELDS_NAMED) continue;
                    memset(&imported, 0, sizeof(imported));
                    imported.module = leaf->module;
                    imported.name = variant->name;
                    imported.namespace_kind =
                        (CmResolveNamespace)namespace_index;
                    imported.declaration =
                        variant->declaration.enumeration;
                    imported.variant = variant->declaration;
                    imported.item_kind = CM_AST_ITEM_ENUM;
                    imported.import_declaration = leaf->declaration;
                    imported.is_public = leaf->is_public;
                    imported.is_crate_visible = leaf->is_crate_visible;
                    imported.is_import = 1;
                    imported.is_reexport = leaf->is_public;
                    imported.is_anonymous = leaf->is_anonymous;
                    cm_record_leaf_binding(leaf, &imported);
                    changed |= cm_add_binding(destination, &imported, 1);
                }
            }
            leaf->ever_resolved = 1;
        }
    } else {
        const CmResolveStringId *last;
        size_t parent_count;

        last = (const CmResolveStringId *)cm_vec_at_const(&leaf->segments,
            leaf->segments.len - 1u);
        parent_count = leaf->segments.len - 1u;
        if (last != NULL && cm_import_string_is(state, *last, "self")) {
            const CmImportBinding *source_binding;
            CmImportScope scope;
            CmResolvedBinding imported;
            int found;

            scope = cm_resolve_scope_segments(state, leaf,
                parent_count, &blocked_module, &blocked_name, &ambiguous);
            source_binding = scope.kind == CM_IMPORT_SCOPE_MODULE
                ? cm_module_self_binding(state, scope.module) : NULL;
            memset(&imported, 0, sizeof(imported));
            found = 0;
            if (scope.kind == CM_IMPORT_SCOPE_MODULE
                && scope.module == state->root) {
                imported.namespace_kind = CM_RESOLVE_NAMESPACE_TYPE;
                imported.item_kind = CM_AST_ITEM_MODULE;
                imported.target_module = state->root;
                imported.is_public = 1;
                imported.is_crate_visible = 1;
                found = 1;
            } else if (source_binding != NULL
                && !source_binding->value.is_ambiguous) {
                imported = source_binding->value;
                found = 1;
            } else if (scope.kind == CM_IMPORT_SCOPE_ENUM) {
                imported = scope.binding;
                found = 1;
            }
            if (found) {
                imported.module = leaf->module;
                imported.name = leaf->import_name;
                imported.import_declaration = leaf->declaration;
                cm_import_apply_leaf_visibility(&imported, leaf);
                imported.is_import = 1;
                imported.is_ambiguous = 0;
                imported.is_anonymous = leaf->is_anonymous;
                cm_record_leaf_binding(leaf, &imported);
                /* `as _` affects trait scope later but creates no name. */
                if (!leaf->is_anonymous)
                    changed |= cm_add_binding(destination, &imported, 2);
                leaf->ever_resolved = 1;
            }
        } else {
            CmImportScope scope;
            int namespace_index;
            int found;

            scope = cm_resolve_scope_segments(state, leaf,
                parent_count, &blocked_module, &blocked_name, &ambiguous);
            found = 0;
            if (scope.kind == CM_IMPORT_SCOPE_MODULE && last != NULL) {
                const CmImportModuleState *source;

                source = cm_import_module_const(state, scope.module);
                for (namespace_index = 0; namespace_index < 3;
                     ++namespace_index) {
                    const CmImportBinding *source_binding;
                    CmResolvedBinding imported;
                    CmResolvedBinding primitive;

                    source_binding = cm_find_binding_const(source,
                        (CmResolveNamespace)namespace_index, *last);
                    if (source_binding == NULL && namespace_index ==
                            (int)CM_RESOLVE_NAMESPACE_TYPE) {
                        source_binding = cm_extern_prelude_binding(state,
                            *last);
                    }
                    if (source_binding == NULL) {
                        if (namespace_index
                                != (int)CM_RESOLVE_NAMESPACE_TYPE
                            || leaf->absolute || parent_count != 0u
                            || !cm_builtin_primitive_binding(state,
                                leaf->module, *last, &primitive)) continue;
                        imported = primitive;
                    } else {
                        if (source_binding->value.is_ambiguous) {
                            ambiguous = 1;
                            continue;
                        }
                        if (namespace_index
                                == (int)CM_RESOLVE_NAMESPACE_VALUE
                            && source_binding->value.item_kind
                                == CM_AST_ITEM_STRUCT
                            && !cm_glob_binding_is_visible(state, leaf,
                                scope.module, &source_binding->value)) {
                            continue;
                        }
                        imported = source_binding->value;
                    }
                    imported.module = leaf->module;
                    imported.name = leaf->import_name;
                    imported.import_declaration = leaf->declaration;
                    cm_import_apply_leaf_visibility(&imported, leaf);
                    imported.is_import = 1;
                    imported.is_ambiguous = 0;
                    imported.is_anonymous = leaf->is_anonymous;
                    cm_record_leaf_binding(leaf, &imported);
                    /* Keep the resolved leaf without publishing `_`. */
                    if (!leaf->is_anonymous)
                        changed |= cm_add_binding(destination, &imported, 2);
                    found = 1;
                }
            } else if (scope.kind == CM_IMPORT_SCOPE_ENUM && last != NULL) {
                size_t variant_index;

                for (variant_index = 0u;
                     variant_index < scope.enumeration->variants.len;
                     ++variant_index) {
                    const CmImportEnumVariant *variant;

                    variant = (const CmImportEnumVariant *)cm_vec_at_const(
                        &scope.enumeration->variants, variant_index);
                    if (variant == NULL || variant->name != *last) continue;
                    for (namespace_index = 0; namespace_index < 2;
                         ++namespace_index) {
                        CmResolvedBinding imported;

                        if (namespace_index
                                == (int)CM_RESOLVE_NAMESPACE_VALUE
                            && variant->form == CM_AST_FIELDS_NAMED) {
                            continue;
                        }
                        memset(&imported, 0, sizeof(imported));
                        imported.module = leaf->module;
                        imported.name = leaf->import_name;
                        imported.namespace_kind =
                            (CmResolveNamespace)namespace_index;
                        imported.declaration =
                            variant->declaration.enumeration;
                        imported.variant = variant->declaration;
                        imported.item_kind = CM_AST_ITEM_ENUM;
                        imported.import_declaration = leaf->declaration;
                        imported.is_public = leaf->is_public;
                        imported.is_crate_visible = leaf->is_crate_visible;
                        imported.is_import = 1;
                        imported.is_reexport = leaf->is_public;
                        imported.is_anonymous = leaf->is_anonymous;
                        cm_record_leaf_binding(leaf, &imported);
                        if (!leaf->is_anonymous) {
                            changed |= cm_add_binding(destination,
                                &imported, 2);
                        }
                        found = 1;
                    }
                }
            }
            if (found) leaf->ever_resolved = 1;
            if (!found && blocked_module == CM_MODULE_NONE && last != NULL) {
                blocked_module = scope.module;
                blocked_name = *last;
            }
        }
    }
    if (!leaf->ever_resolved) {
        leaf->blocked_module = blocked_module;
        leaf->blocked_name = blocked_name;
    }
    if (ambiguous) leaf->saw_ambiguous = 1;
    return changed;
}

static int cm_leaf_dependency(const CmImportResolverState *state,
    size_t leaf_index)
{
    const CmImportLeaf *leaf;
    size_t index;

    leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves, leaf_index);
    if (leaf == NULL || leaf->ever_resolved ||
        leaf->blocked_module == CM_MODULE_NONE ||
        leaf->blocked_name == CM_RESOLVE_STRING_NONE) return -1;
    for (index = 0u; index < state->leaves.len; ++index) {
        const CmImportLeaf *candidate;

        candidate = (const CmImportLeaf *)cm_vec_at_const(&state->leaves,
            index);
        if (candidate != NULL && !candidate->ever_resolved &&
            candidate->module == leaf->blocked_module &&
            candidate->import_name == leaf->blocked_name) return (int)index;
    }
    return -1;
}

static int cm_leaf_is_in_cycle(const CmImportResolverState *state,
    size_t start)
{
    int current;
    size_t steps;

    current = (int)start;
    for (steps = 0u; steps <= state->leaves.len; ++steps) {
        current = cm_leaf_dependency(state, (size_t)current);
        if (current < 0) return 0;
        if ((size_t)current == start) return 1;
    }
    return 0;
}

static void cm_collect_prelude_bindings(CmImportResolverState *state)
{
    CmImportLeaf *prelude_leaf;
    CmImportLeaf error_leaf;
    size_t leaf_index;
    size_t match_count;

    if (state->prelude_declaration.source == 0u
        || state->prelude_declaration.item == CM_AST_ITEM_NONE) {
        return;
    }
    prelude_leaf = NULL;
    match_count = 0u;
    for (leaf_index = 0u; leaf_index < state->leaves.len; ++leaf_index) {
        CmImportLeaf *leaf;

        leaf = (CmImportLeaf *)cm_vec_at(&state->leaves, leaf_index);
        if (leaf != NULL && cm_item_ref_equal(leaf->declaration,
                state->prelude_declaration)) {
            prelude_leaf = leaf;
            match_count += 1u;
        }
    }
    /*
     * Keep the leaf snapshot instead of copying the source module namespace:
     * the ordinary glob resolver has already enforced public reachability and
     * retained the exact declaration/import provenance for each namespace.
     */
    if (state->prelude_invalid || match_count != 1u
        || prelude_leaf == NULL || !prelude_leaf->is_glob
        || prelude_leaf->is_anonymous || prelude_leaf->is_public
        || !prelude_leaf->ever_resolved) {
        memset(&error_leaf, 0, sizeof(error_leaf));
        error_leaf.module = state->root;
        error_leaf.declaration = state->prelude_declaration;
        cm_push_error(state, CM_IMPORT_ERROR_INVALID_TREE, &error_leaf,
            CM_RESOLVE_STRING_NONE, CM_RESOLVE_STRING_NONE);
        return;
    }
    for (leaf_index = 0u; leaf_index < prelude_leaf->bindings.len;
         ++leaf_index) {
        const CmResolvedBinding *binding;

        binding = (const CmResolvedBinding *)cm_vec_at_const(
            &prelude_leaf->bindings, leaf_index);
        if (binding != NULL) {
            (void)cm_vec_push(&state->prelude_bindings, binding);
        }
    }
}

static size_t cm_total_bindings(const CmImportResolverState *state)
{
    size_t total;
    size_t module_index;

    total = 0u;
    for (module_index = 0u; module_index < state->modules.len;
         ++module_index) {
        const CmImportModuleState *module;
        int namespace_index;

        module = (const CmImportModuleState *)cm_vec_at_const(
            &state->modules, module_index);
        if (module == NULL) continue;
        for (namespace_index = 0; namespace_index < 3; ++namespace_index)
            total += module->namespaces[namespace_index].len;
    }
    return total;
}

static void cm_stamp_bindings(CmImportResolverState *state,
    CmModuleGraphRevision revision)
{
    size_t module_index;
    size_t leaf_index;

    for (module_index = 0u; module_index < state->modules.len;
         ++module_index) {
        CmImportModuleState *module;
        int namespace_index;

        module = (CmImportModuleState *)cm_vec_at(&state->modules,
            module_index);
        if (module == NULL) continue;
        for (namespace_index = 0; namespace_index < 3; ++namespace_index) {
            size_t binding_index;

            for (binding_index = 0u;
                 binding_index < module->namespaces[namespace_index].len;
                 ++binding_index) {
                CmImportBinding *binding;

                binding = (CmImportBinding *)cm_vec_at(
                    &module->namespaces[namespace_index], binding_index);
                if (binding != NULL) binding->value.revision = revision;
            }
        }
    }
    for (leaf_index = 0u; leaf_index < state->leaves.len; ++leaf_index) {
        CmImportLeaf *leaf;
        size_t binding_index;

        leaf = (CmImportLeaf *)cm_vec_at(&state->leaves, leaf_index);
        if (leaf == NULL) continue;
        for (binding_index = 0u; binding_index < leaf->bindings.len;
             ++binding_index) {
            CmResolvedBinding *binding;

            binding = (CmResolvedBinding *)cm_vec_at(&leaf->bindings,
                binding_index);
            if (binding != NULL) binding->revision = revision;
        }
    }
    for (leaf_index = 0u; leaf_index < state->prelude_bindings.len;
         ++leaf_index) {
        CmResolvedBinding *binding;

        binding = (CmResolvedBinding *)cm_vec_at(&state->prelude_bindings,
            leaf_index);
        if (binding != NULL) binding->revision = revision;
    }
}

static int cm_mirror_graph(CmImportResolverState *state,
    const CmModuleGraph *graph)
{
    size_t module_count;
    CmModuleId module_id;

    module_count = cm_module_graph_module_count(graph);
    if (module_count == 0u) return 0;
    cm_intern_builtin_primitive_names(state);
    for (module_id = 1u; (size_t)module_id <= module_count; ++module_id) {
        CmResolveModuleInfo information;
        CmImportModuleState module;
        uint32_t child_index;
        int namespace_index;

        if (!cm_module_graph_get_module(graph, module_id, &information))
            return 0;
        memset(&module, 0, sizeof(module));
        module.id = information.id;
        module.parent = information.parent;
        module.name = cm_import_intern_graph_string(state, graph,
            information.name);
        cm_vec_init(&module.children, sizeof(CmModuleId));
        for (namespace_index = 0; namespace_index < 3; ++namespace_index)
            cm_vec_init(&module.namespaces[namespace_index],
                sizeof(CmImportBinding));
        for (child_index = 0u; child_index < information.child_count;
             ++child_index) {
            CmModuleId child;

            if (!cm_module_graph_get_child(graph, module_id, child_index,
                &child)) return 0;
            (void)cm_vec_push(&module.children, &child);
        }
        if (module.parent == CM_MODULE_NONE) state->root = module.id;
        (void)cm_vec_push(&state->modules, &module);
    }
    for (module_id = 1u; (size_t)module_id <= module_count; ++module_id) {
        CmResolveModuleInfo information;
        CmImportModuleState *module;
        uint32_t effective_index;
        int namespace_index;

        if (!cm_module_graph_get_module(graph, module_id, &information))
            return 0;
        module = cm_import_module(state, module_id);
        for (effective_index = 0u;
             effective_index < information.effective_item_count;
             ++effective_index) {
            CmResolveEffectiveItem effective;
            CmImportEnumState enumeration;
            uint32_t attribute_index;
            uint32_t prelude_attribute_count;
            uint32_t variant_index;

            if (cm_module_graph_get_effective_item(graph,
                    cm_module_graph_revision(graph), module_id,
                    effective_index, &effective) != CM_RESOLVE_VIEW_OK) {
                return 0;
            }
            prelude_attribute_count = 0u;
            for (attribute_index = 0u;
                 attribute_index < effective.attribute_count;
                 ++attribute_index) {
                int is_prelude;

                is_prelude = cm_import_effective_attribute_is(graph,
                    cm_module_graph_revision(graph), module_id,
                    effective_index, attribute_index, "prelude_import");
                if (is_prelude < 0) return 0;
                if (is_prelude != 0) prelude_attribute_count += 1u;
            }
            if (prelude_attribute_count != 0u) {
                if (state->prelude_declaration.source == 0u
                    && state->prelude_declaration.item
                        == CM_AST_ITEM_NONE) {
                    state->prelude_declaration = effective.declaration;
                } else if (!cm_item_ref_equal(state->prelude_declaration,
                        effective.declaration)) {
                    state->prelude_invalid = 1;
                }
                if (prelude_attribute_count != 1u
                    || effective.item_kind != CM_AST_ITEM_USE
                    || module_id != state->root) {
                    state->prelude_invalid = 1;
                }
            }
            if (effective.item_kind != CM_AST_ITEM_ENUM) continue;
            memset(&enumeration, 0, sizeof(enumeration));
            enumeration.module = module_id;
            enumeration.declaration = effective.declaration;
            cm_vec_init(&enumeration.variants, sizeof(CmImportEnumVariant));
            for (variant_index = 0u;
                 variant_index < effective.variant_count;
                 ++variant_index) {
                CmResolveEffectiveVariant effective_variant;
                CmImportEnumVariant variant;

                if (cm_module_graph_get_effective_variant(graph,
                        cm_module_graph_revision(graph), module_id,
                        effective.id, variant_index, &effective_variant)
                    != CM_RESOLVE_VIEW_OK) {
                    cm_vec_destroy(&enumeration.variants);
                    return 0;
                }
                memset(&variant, 0, sizeof(variant));
                variant.name = cm_import_intern_graph_string(state, graph,
                    effective_variant.name);
                variant.declaration = effective_variant.declaration;
                variant.form = effective_variant.form;
                (void)cm_vec_push(&enumeration.variants, &variant);
            }
            (void)cm_vec_push(&state->enumerations, &enumeration);
        }
        for (namespace_index = 0; namespace_index < 3; ++namespace_index) {
            uint32_t entry_count;
            uint32_t entry_index;

            entry_count = namespace_index == 0 ? information.type_count :
                (namespace_index == 1 ? information.value_count :
                 information.macro_count);
            for (entry_index = 0u; entry_index < entry_count;
                 ++entry_index) {
                CmResolveNamespaceEntry entry;
                CmResolvedBinding binding;

                if (!cm_module_graph_get_namespace_entry(graph, module_id,
                    (CmResolveNamespace)namespace_index, entry_index,
                    &entry)) return 0;
                memset(&binding, 0, sizeof(binding));
                binding.module = module_id;
                binding.name = cm_import_intern_graph_string(state, graph,
                    entry.name);
                binding.namespace_kind =
                    (CmResolveNamespace)namespace_index;
                binding.declaration = entry.declaration;
                binding.item_kind = entry.item_kind;
                binding.is_public = entry.visibility == CM_AST_VIS_PUBLIC;
                binding.is_crate_visible = binding.is_public
                    || entry.visibility == CM_AST_VIS_CRATE;
                if (entry.item_kind == CM_AST_ITEM_MODULE) {
                    binding.target_module = cm_direct_child(state, module_id,
                        binding.name);
                } else if (entry.item_kind == CM_AST_ITEM_EXTERN_CRATE) {
                    const CmAst *ast;
                    const CmAstItem *item;
                    const CmInternedString *name;

                    ast = NULL;
                    item = NULL;
                    name = NULL;
                    if (cm_module_graph_borrow_ast(graph, module_id, &ast)) {
                        item = cm_ast_get_item(ast,
                            entry.declaration.item);
                    }
                    if (item != NULL
                        && item->kind == CM_AST_ITEM_EXTERN_CRATE
                        && item->data.extern_crate_item.alias
                            != CM_INTERN_ID_NONE) {
                        name = cm_ast_get_string(ast, item->name);
                    }
                    if (name != NULL && name->len == strlen("self")
                        && memcmp(name->bytes, "self", name->len) == 0) {
                        binding.target_module = state->root;
                    }
                }
                (void)cm_add_binding(module, &binding, 3);
            }
        }
        {
            uint32_t import_index;

            for (import_index = 0u; import_index < information.import_count;
                 ++import_index) {
                CmResolveImport import_directive;
                CmImportLeaf error_leaf;

                if (!cm_module_graph_get_import(graph, module_id,
                    import_index, &import_directive)) return 0;
                if (!cm_parse_import(state, graph, module_id,
                    &import_directive)) {
                    memset(&error_leaf, 0, sizeof(error_leaf));
                    error_leaf.module = module_id;
                    error_leaf.declaration = import_directive.declaration;
                    cm_push_error(state, CM_IMPORT_ERROR_INVALID_TREE,
                        &error_leaf, CM_RESOLVE_STRING_NONE,
                        cm_import_intern_graph_string(state, graph,
                            import_directive.tree));
                }
            }
        }
    }
    return state->root != CM_MODULE_NONE;
}

static CmImportResult cm_import_invalid_result(
    CmImportResolverState *state)
{
    CmImportResult result;
    CmImportError error;

    memset(&result, 0, sizeof(result));
    cm_import_state_clear(state);
    memset(&error, 0, sizeof(error));
    error.kind = CM_IMPORT_ERROR_INVALID_ARGUMENT;
    (void)cm_vec_push(&state->errors, &error);
    result.error_count = state->errors.len;
    return result;
}

CmImportResult cm_import_resolve(CmImportResolver *resolver,
    const CmModuleGraph *graph, CmModuleGraphRevision expected_revision)
{
    CmImportResolverState *state;
    CmImportResult result;
    size_t pass;
    int changed;
    size_t index;

    memset(&result, 0, sizeof(result));
    state = cm_import_state(resolver);
    if (state == NULL) return result;
    if (state->generation == UINT64_MAX) abort();
    state->generation += UINT64_C(1);
    cm_import_state_clear(state);
    if (graph == NULL || expected_revision == CM_MODULE_GRAPH_REVISION_NONE ||
        cm_module_graph_revision(graph) != expected_revision ||
        cm_module_graph_error_count(graph) != 0u ||
        !cm_mirror_graph(state, graph)) {
        return cm_import_invalid_result(state);
    }
    pass = 0u;
    do {
        changed = 0;
        for (index = 0u; index < state->leaves.len; ++index) {
            CmImportLeaf *leaf;

            leaf = (CmImportLeaf *)cm_vec_at(&state->leaves, index);
            if (leaf == NULL) continue;
            changed |= cm_resolve_leaf(state, leaf);
            if (!leaf->ever_resolved)
                changed |= cm_resolve_leaf_dependency(state, leaf);
        }
        ++pass;
    } while (changed && pass <= state->leaves.len +
        cm_total_bindings(state) + 1u);

    cm_collect_prelude_bindings(state);

    for (index = 0u; index < state->leaves.len; ++index) {
        const CmImportLeaf *leaf;

        leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves, index);
        if (leaf == NULL || leaf->ever_resolved) continue;
        cm_push_error(state, leaf->saw_ambiguous ?
            CM_IMPORT_ERROR_AMBIGUOUS :
            (cm_leaf_is_in_cycle(state, index) ? CM_IMPORT_ERROR_CYCLE :
             CM_IMPORT_ERROR_UNRESOLVED), leaf, leaf->import_name,
            leaf->blocked_name);
    }
    for (index = 0u; index < state->modules.len; ++index) {
        const CmImportModuleState *module;
        int namespace_index;

        module = (const CmImportModuleState *)cm_vec_at_const(
            &state->modules, index);
        if (module == NULL) continue;
        for (namespace_index = 0; namespace_index < 3; ++namespace_index) {
            size_t binding_index;

            for (binding_index = 0u;
                 binding_index < module->namespaces[namespace_index].len;
                 ++binding_index) {
                const CmImportBinding *binding;
                CmImportLeaf leaf;

                binding = (const CmImportBinding *)cm_vec_at_const(
                    &module->namespaces[namespace_index], binding_index);
                if (binding == NULL || !binding->value.is_ambiguous)
                    continue;
                memset(&leaf, 0, sizeof(leaf));
                leaf.module = module->id;
                leaf.declaration = binding->value.import_declaration;
                cm_push_error(state, CM_IMPORT_ERROR_AMBIGUOUS, &leaf,
                    binding->value.name, CM_RESOLVE_STRING_NONE);
            }
        }
    }
    if (cm_module_graph_revision(graph) != expected_revision ||
        cm_module_graph_error_count(graph) != 0u) {
        return cm_import_invalid_result(state);
    }
    state->graph = graph;
    state->graph_lifetime_id = cm_module_graph_lifetime_id(graph);
    state->revision = expected_revision;
    cm_stamp_bindings(state, expected_revision);
    result.binding_count = cm_total_bindings(state);
    result.error_count = state->errors.len;
    result.revision = expected_revision;
    return result;
}

CmModuleGraphRevision cm_import_resolver_revision(
    const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state == NULL ? CM_MODULE_GRAPH_REVISION_NONE : state->revision;
}

uint64_t cm_import_resolver_lifetime_id(const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state == NULL ? UINT64_C(0) : state->lifetime_id;
}

uint64_t cm_import_resolver_generation(const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state == NULL ? UINT64_C(0) : state->generation;
}

uint64_t cm_import_resolver_graph_lifetime_id(
    const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state == NULL ? UINT64_C(0) : state->graph_lifetime_id;
}

int cm_import_resolver_matches_graph(const CmImportResolver *resolver,
    const CmModuleGraph *graph)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state != NULL && graph != NULL && state->graph == graph &&
        state->graph_lifetime_id != UINT64_C(0) &&
        state->graph_lifetime_id == cm_module_graph_lifetime_id(graph) &&
        state->revision != CM_MODULE_GRAPH_REVISION_NONE &&
        cm_module_graph_error_count(graph) == 0u &&
        cm_module_graph_revision(graph) == state->revision;
}

size_t cm_import_binding_count(const CmImportResolver *resolver,
    CmModuleId module, CmResolveNamespace namespace_kind)
{
    const CmImportResolverState *state;
    const CmImportModuleState *module_state;

    state = cm_import_state_const(resolver);
    module_state = cm_import_module_const(state, module);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE ||
        module_state == NULL || namespace_kind < CM_RESOLVE_NAMESPACE_TYPE ||
        namespace_kind > CM_RESOLVE_NAMESPACE_MACRO) return 0u;
    return module_state->namespaces[(int)namespace_kind].len;
}

int cm_import_get_binding(const CmImportResolver *resolver,
    CmModuleId module, CmResolveNamespace namespace_kind, uint32_t index,
    CmResolvedBinding *out_binding)
{
    const CmImportResolverState *state;
    const CmImportModuleState *module_state;
    const CmImportBinding *binding;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    state = cm_import_state_const(resolver);
    module_state = cm_import_module_const(state, module);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE ||
        module_state == NULL || out_binding == NULL ||
        namespace_kind < CM_RESOLVE_NAMESPACE_TYPE ||
        namespace_kind > CM_RESOLVE_NAMESPACE_MACRO) return 0;
    binding = (const CmImportBinding *)cm_vec_at_const(
        &module_state->namespaces[(int)namespace_kind], index);
    if (binding == NULL) return 0;
    *out_binding = binding->value;
    return 1;
}

size_t cm_import_declaration_binding_count(
    const CmImportResolver *resolver, CmModuleId module,
    CmResolveItemRef import_declaration)
{
    const CmImportResolverState *state;
    size_t count;
    size_t leaf_index;

    state = cm_import_state_const(resolver);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE ||
        module == CM_MODULE_NONE || import_declaration.source == 0u ||
        import_declaration.item == CM_AST_ITEM_NONE) return 0u;
    count = 0u;
    for (leaf_index = 0u; leaf_index < state->leaves.len; ++leaf_index) {
        const CmImportLeaf *leaf;

        leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves,
            leaf_index);
        if (leaf != NULL && leaf->module == module &&
            cm_item_ref_equal(leaf->declaration, import_declaration)) {
            count += leaf->bindings.len;
        }
    }
    return count;
}

int cm_import_get_declaration_binding(const CmImportResolver *resolver,
    CmModuleId module, CmResolveItemRef import_declaration, uint32_t index,
    CmResolvedBinding *out_binding)
{
    const CmImportResolverState *state;
    size_t result_index;
    size_t leaf_index;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    state = cm_import_state_const(resolver);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE ||
        module == CM_MODULE_NONE || import_declaration.source == 0u ||
        import_declaration.item == CM_AST_ITEM_NONE || out_binding == NULL) {
        return 0;
    }
    result_index = (size_t)index;
    for (leaf_index = 0u; leaf_index < state->leaves.len; ++leaf_index) {
        const CmImportLeaf *leaf;

        leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves,
            leaf_index);
        if (leaf == NULL || leaf->module != module ||
            !cm_item_ref_equal(leaf->declaration, import_declaration)) {
            continue;
        }
        if (result_index < leaf->bindings.len) {
            const CmResolvedBinding *binding;

            binding = (const CmResolvedBinding *)cm_vec_at_const(
                &leaf->bindings, result_index);
            if (binding == NULL) return 0;
            *out_binding = *binding;
            return 1;
        }
        result_index -= leaf->bindings.len;
    }
    return 0;
}

size_t cm_import_leaf_count(const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE)
        return 0u;
    return state->leaves.len;
}

int cm_import_get_leaf(const CmImportResolver *resolver, uint32_t index,
    CmImportLeafView *out_leaf)
{
    const CmImportResolverState *state;
    const CmImportLeaf *leaf;

    if (out_leaf != NULL) memset(out_leaf, 0, sizeof(*out_leaf));
    state = cm_import_state_const(resolver);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE
        || out_leaf == NULL) return 0;
    leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves,
        (size_t)index);
    if (leaf == NULL) return 0;
    out_leaf->revision = state->revision;
    out_leaf->module = leaf->module;
    out_leaf->declaration = leaf->declaration;
    out_leaf->import_name = leaf->import_name;
    out_leaf->segment_count = leaf->segments.len;
    out_leaf->binding_count = leaf->bindings.len;
    out_leaf->absolute = leaf->absolute;
    out_leaf->is_glob = leaf->is_glob;
    out_leaf->is_anonymous = leaf->is_anonymous;
    out_leaf->is_public = leaf->is_public;
    out_leaf->is_crate_visible = leaf->is_crate_visible;
    out_leaf->is_resolved = leaf->ever_resolved;
    out_leaf->saw_ambiguous = leaf->saw_ambiguous;
    return 1;
}

int cm_import_get_leaf_segment(const CmImportResolver *resolver,
    uint32_t leaf_index, uint32_t segment_index,
    CmResolvePathSegmentView *out_segment)
{
    const CmImportResolverState *state;
    const CmImportLeaf *leaf;
    const CmResolveStringId *segment_id;
    const CmInternedString *segment;

    if (out_segment != NULL) memset(out_segment, 0, sizeof(*out_segment));
    state = cm_import_state_const(resolver);
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE
        || out_segment == NULL) return 0;
    leaf = (const CmImportLeaf *)cm_vec_at_const(&state->leaves,
        (size_t)leaf_index);
    segment_id = leaf == NULL ? NULL
        : (const CmResolveStringId *)cm_vec_at_const(&leaf->segments,
            (size_t)segment_index);
    segment = segment_id == NULL ? NULL
        : cm_import_string(state, *segment_id);
    if (segment == NULL) return 0;
    out_segment->bytes = segment->bytes;
    out_segment->length = segment->len;
    return 1;
}

static int cm_import_path_segment_id(const CmImportResolverState *state,
    const CmResolvePathSegmentView *segment, CmResolveStringId *out_id)
{
    CmInternId id;

    if (segment == NULL || out_id == NULL || segment->bytes == NULL
        || segment->length == 0u) {
        return 0;
    }
    id = cm_interner_lookup(&state->strings, segment->bytes,
        segment->length);
    *out_id = (CmResolveStringId)id;
    return 1;
}

static int cm_import_path_segment_is(
    const CmResolvePathSegmentView *segment, const char *text)
{
    size_t length;

    if (segment == NULL || segment->bytes == NULL || text == NULL)
        return 0;
    length = strlen(text);
    return segment->length == length
        && memcmp(segment->bytes, text, length) == 0;
}

static CmImportLookupStatus cm_import_missing_lookup_status(
    const CmImportResolverState *state, CmModuleId module,
    CmResolveStringId name)
{
    size_t index;

    for (index = 0u; index < state->errors.len; ++index) {
        const CmImportError *error;

        error = (const CmImportError *)cm_vec_at_const(&state->errors,
            index);
        if (error == NULL || error->module != module
            || error->name != name) {
            continue;
        }
        if (error->kind == CM_IMPORT_ERROR_CYCLE)
            return CM_IMPORT_LOOKUP_CYCLE;
        if (error->kind == CM_IMPORT_ERROR_AMBIGUOUS)
            return CM_IMPORT_LOOKUP_AMBIGUOUS;
    }
    return CM_IMPORT_LOOKUP_NOT_FOUND;
}

static const CmResolvedBinding *cm_import_prelude_binding(
    const CmImportResolverState *state, CmResolveNamespace namespace_kind,
    CmResolveStringId name)
{
    size_t index;

    for (index = 0u; index < state->prelude_bindings.len; ++index) {
        const CmResolvedBinding *binding;

        binding = (const CmResolvedBinding *)cm_vec_at_const(
            &state->prelude_bindings, index);
        if (binding != NULL && binding->namespace_kind == namespace_kind
            && binding->name == name) {
            return binding;
        }
    }
    return NULL;
}

CmImportLookupStatus cm_import_resolve_path(
    const CmImportResolver *resolver, CmModuleId module, int absolute,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmResolveNamespace namespace_kind, CmResolvedBinding *out_binding)
{
    const CmImportResolverState *state;
    const CmImportModuleState *module_state;
    const CmImportBinding *binding;
    const CmImportEnumState *enumeration;
    CmModuleId current;
    size_t index;

    state = cm_import_state_const(resolver);
    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    if (state == NULL || state->revision == CM_MODULE_GRAPH_REVISION_NONE ||
        out_binding == NULL || segments == NULL
        || segment_count == 0u || (absolute != 0 && absolute != 1)
        || namespace_kind < CM_RESOLVE_NAMESPACE_TYPE
        || namespace_kind > CM_RESOLVE_NAMESPACE_MACRO
        || (!absolute && cm_import_module_const(state, module) == NULL)
        || state->root == CM_MODULE_NONE) {
        return CM_IMPORT_LOOKUP_INVALID;
    }
    current = absolute ? state->root : module;
    enumeration = NULL;
    index = 0u;
    if (segment_count != 0u) {
        if (cm_import_path_segment_is(&segments[0], "crate")) {
            current = state->root;
            index = 1u;
        } else if (cm_import_path_segment_is(&segments[0], "self")) {
            index = 1u;
        }
    }
    while (index + 1u < segment_count) {
        const CmImportModuleState *current_module;
        CmResolveStringId segment;
        int ambiguous;

        if (enumeration != NULL) return CM_IMPORT_LOOKUP_NOT_FOUND;
        if (cm_import_path_segment_is(&segments[index], "super")) {
            current_module = cm_import_module_const(state, current);
            if (current_module == NULL
                || current_module->parent == CM_MODULE_NONE) {
                return CM_IMPORT_LOOKUP_NOT_FOUND;
            }
            current = current_module->parent;
        } else if (cm_import_path_segment_is(&segments[index], "crate")
            || cm_import_path_segment_is(&segments[index], "self")) {
            return CM_IMPORT_LOOKUP_INVALID;
        } else {
            const CmImportBinding *scope_binding;
            const CmResolvedBinding *prelude_binding;

            if (!cm_import_path_segment_id(state, &segments[index],
                    &segment)) {
                return CM_IMPORT_LOOKUP_INVALID;
            }
            if (segment == CM_RESOLVE_STRING_NONE)
                return CM_IMPORT_LOOKUP_NOT_FOUND;
            ambiguous = 0;
            current_module = cm_import_module_const(state, current);
            scope_binding = cm_find_binding_const(current_module,
                CM_RESOLVE_NAMESPACE_TYPE, segment);
            prelude_binding = NULL;
            if (scope_binding == NULL && !absolute && index == 0u) {
                prelude_binding = cm_import_prelude_binding(state,
                    CM_RESOLVE_NAMESPACE_TYPE, segment);
            }
            if (scope_binding == NULL)
                scope_binding = cm_extern_prelude_binding(state, segment);
            if (scope_binding != NULL
                && scope_binding->value.is_ambiguous) {
                ambiguous = 1;
            }
            if (ambiguous) return CM_IMPORT_LOOKUP_AMBIGUOUS;
            if (scope_binding == NULL && prelude_binding == NULL
                && index == 0u && !absolute) {
                /* M9-03: `core::...` paths delegate to the registered
                 * dependency crate's own resolver. */
                const CmImportDependency *dep =
                    cm_import_find_dependency_view(state, &segments[0]);
                if (dep != NULL) {
                    CmImportLookupStatus dep_status;
                    dep_status = cm_import_resolve_path(dep->resolver,
                        dep->root, 0, segments + 1u, segment_count - 1u,
                        namespace_kind, out_binding);
                    if (dep_status == CM_IMPORT_LOOKUP_OK) {
                        out_binding->dependency =
                            cm_import_dependency_tag(state, dep);
                    }
                    return dep_status;
                }
            }
            if (scope_binding == NULL && prelude_binding == NULL)
                return cm_import_missing_lookup_status(state, current,
                    segment);
            if (scope_binding != NULL
                && scope_binding->value.dependency != 0u
                && scope_binding->value.target_module != CM_MODULE_NONE
                && (size_t)scope_binding->value.dependency
                    <= state->dependencies.len) {
                /* An imported dependency module: delegate the remaining
                 * path to that crate's resolver (M9-03). */
                const CmImportDependency *dep = (const CmImportDependency *)
                    cm_vec_at_const(&state->dependencies,
                        (size_t)scope_binding->value.dependency - 1u);
                CmImportLookupStatus dep_status;
                dep_status = cm_import_resolve_path(dep->resolver,
                    scope_binding->value.target_module, 0,
                    segments + index + 1u, segment_count - index - 1u,
                    namespace_kind, out_binding);
                if (dep_status == CM_IMPORT_LOOKUP_OK) {
                    out_binding->dependency =
                        cm_import_dependency_tag(state, dep);
                }
                return dep_status;
            }
            if (scope_binding != NULL
                && scope_binding->value.target_module != CM_MODULE_NONE) {
                current = scope_binding->value.target_module;
            } else if (prelude_binding != NULL
                && prelude_binding->target_module != CM_MODULE_NONE) {
                current = prelude_binding->target_module;
            } else if (((scope_binding != NULL
                        && scope_binding->value.item_kind
                            == CM_AST_ITEM_ENUM)
                    || (prelude_binding != NULL
                        && prelude_binding->item_kind == CM_AST_ITEM_ENUM))
                && index + 2u == segment_count) {
                enumeration = cm_import_enumeration_const(state,
                    scope_binding != NULL
                        ? scope_binding->value.declaration
                        : prelude_binding->declaration);
                if (enumeration == NULL)
                    return CM_IMPORT_LOOKUP_NOT_FOUND;
            } else {
                return CM_IMPORT_LOOKUP_NOT_FOUND;
            }
        }
        ++index;
    }
    if (index >= segment_count) return CM_IMPORT_LOOKUP_INVALID;
    {
        CmResolveStringId last;

        if (cm_import_path_segment_is(&segments[index], "crate")
            || cm_import_path_segment_is(&segments[index], "self")
            || cm_import_path_segment_is(&segments[index], "super")) {
            return CM_IMPORT_LOOKUP_INVALID;
        }
        if (!cm_import_path_segment_id(state, &segments[index], &last))
            return CM_IMPORT_LOOKUP_INVALID;
        if (last == CM_RESOLVE_STRING_NONE)
            return CM_IMPORT_LOOKUP_NOT_FOUND;
        if (enumeration == NULL) {
            const CmResolvedBinding *prelude_binding;

            module_state = cm_import_module_const(state, current);
            binding = cm_find_binding_const(module_state, namespace_kind,
                last);
            prelude_binding = NULL;
            if (binding == NULL && !absolute && segment_count == 1u) {
                prelude_binding = cm_import_prelude_binding(state,
                    namespace_kind, last);
            }
            if (binding == NULL
                && namespace_kind == CM_RESOLVE_NAMESPACE_TYPE) {
                binding = cm_extern_prelude_binding(state, last);
            }
            if (binding == NULL && prelude_binding == NULL
                && namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
                && !absolute && segment_count == 1u
                && cm_builtin_primitive_binding(state, module, last,
                    out_binding)) return CM_IMPORT_LOOKUP_OK;
            if (binding == NULL && prelude_binding == NULL
                && segment_count == 1u && !absolute
                && namespace_kind == CM_RESOLVE_NAMESPACE_TYPE) {
                const CmImportDependency *dep =
                    cm_import_find_dependency_view(state, &segments[0]);
                if (dep != NULL) {
                    memset(out_binding, 0, sizeof(*out_binding));
                    out_binding->revision = state->revision;
                    out_binding->module = current;
                    out_binding->name = last;
                    out_binding->namespace_kind = namespace_kind;
                    out_binding->item_kind = CM_AST_ITEM_MODULE;
                    out_binding->target_module = dep->root;
                    out_binding->is_public = 1;
                    out_binding->is_crate_visible = 1;
                    out_binding->dependency =
                        cm_import_dependency_tag(state, dep);
                    return CM_IMPORT_LOOKUP_OK;
                }
            }
            if (binding == NULL && prelude_binding == NULL)
                return cm_import_missing_lookup_status(state, current, last);
            if (binding == NULL) {
                if (prelude_binding->is_ambiguous)
                    return CM_IMPORT_LOOKUP_AMBIGUOUS;
                *out_binding = *prelude_binding;
                return CM_IMPORT_LOOKUP_OK;
            }
        } else {
            size_t variant_index;

            binding = NULL;
            for (variant_index = 0u;
                 variant_index < enumeration->variants.len;
                 ++variant_index) {
                const CmImportEnumVariant *variant;

                variant = (const CmImportEnumVariant *)cm_vec_at_const(
                    &enumeration->variants, variant_index);
                if (variant == NULL || variant->name != last
                    || namespace_kind == CM_RESOLVE_NAMESPACE_MACRO
                    || (namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
                        && variant->form == CM_AST_FIELDS_NAMED)) continue;
                memset(out_binding, 0, sizeof(*out_binding));
                out_binding->revision = state->revision;
                out_binding->module = enumeration->module;
                out_binding->name = variant->name;
                out_binding->namespace_kind = namespace_kind;
                out_binding->declaration = variant->declaration.enumeration;
                out_binding->variant = variant->declaration;
                out_binding->item_kind = CM_AST_ITEM_ENUM;
                out_binding->is_public = 1;
                out_binding->is_crate_visible = 1;
                return CM_IMPORT_LOOKUP_OK;
            }
            return CM_IMPORT_LOOKUP_NOT_FOUND;
        }
    }
    if (binding->value.is_ambiguous) return CM_IMPORT_LOOKUP_AMBIGUOUS;
    *out_binding = binding->value;
    return CM_IMPORT_LOOKUP_OK;
}

CmImportLookupStatus cm_import_resolve_path_checked(
    const CmImportResolver *resolver, const CmModuleGraph *graph,
    CmModuleGraphRevision expected_revision, CmModuleId module, int absolute,
    const CmResolvePathSegmentView *segments, size_t segment_count,
    CmResolveNamespace namespace_kind, CmResolvedBinding *out_binding)
{
    const CmImportResolverState *state;

    if (out_binding != NULL) memset(out_binding, 0, sizeof(*out_binding));
    state = cm_import_state_const(resolver);
    if (state == NULL || graph == NULL || out_binding == NULL
        || expected_revision == CM_MODULE_GRAPH_REVISION_NONE) {
        return CM_IMPORT_LOOKUP_INVALID;
    }
    if (cm_module_graph_revision(graph) != expected_revision)
        return CM_IMPORT_LOOKUP_STALE_REVISION;
    if (cm_module_graph_error_count(graph) != 0u)
        return CM_IMPORT_LOOKUP_FAILED_BUILD;
    if (state->graph != graph || state->revision != expected_revision)
        return CM_IMPORT_LOOKUP_STALE_REVISION;
    return cm_import_resolve_path(resolver, module, absolute, segments,
        segment_count, namespace_kind, out_binding);
}

size_t cm_import_error_count(const CmImportResolver *resolver)
{
    const CmImportResolverState *state;

    state = cm_import_state_const(resolver);
    return state == NULL ? 0u : state->errors.len;
}

int cm_import_get_error(const CmImportResolver *resolver, uint32_t index,
    CmImportError *out_error)
{
    const CmImportResolverState *state;
    const CmImportError *error;

    state = cm_import_state_const(resolver);
    if (state == NULL || out_error == NULL) return 0;
    error = (const CmImportError *)cm_vec_at_const(&state->errors, index);
    if (error == NULL) return 0;
    *out_error = *error;
    return 1;
}

size_t cm_import_string_length(const CmImportResolver *resolver,
    CmResolveStringId id)
{
    const CmImportResolverState *state;
    const CmInternedString *string;

    state = cm_import_state_const(resolver);
    if (state == NULL) return 0u;
    string = cm_import_string(state, id);
    return string == NULL ? 0u : string->len;
}

int cm_import_copy_string(const CmImportResolver *resolver,
    CmResolveStringId id, char *buffer, size_t buffer_size)
{
    const CmImportResolverState *state;
    const CmInternedString *string;

    state = cm_import_state_const(resolver);
    if (state == NULL || buffer == NULL) return 0;
    string = cm_import_string(state, id);
    if (string == NULL || buffer_size <= string->len) return 0;
    memcpy(buffer, string->bytes, string->len);
    buffer[string->len] = 0;
    return 1;
}

const char *cm_import_error_kind_name(CmImportErrorKind kind)
{
    switch (kind) {
    case CM_IMPORT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case CM_IMPORT_ERROR_INVALID_TREE: return "invalid import tree";
    case CM_IMPORT_ERROR_UNRESOLVED: return "unresolved import";
    case CM_IMPORT_ERROR_AMBIGUOUS: return "ambiguous import";
    case CM_IMPORT_ERROR_CYCLE: return "import cycle";
    }
    return "unknown import error";
}

const char *cm_import_lookup_status_name(CmImportLookupStatus status)
{
    switch (status) {
    case CM_IMPORT_LOOKUP_OK: return "ok";
    case CM_IMPORT_LOOKUP_NOT_FOUND: return "not found";
    case CM_IMPORT_LOOKUP_AMBIGUOUS: return "ambiguous";
    case CM_IMPORT_LOOKUP_CYCLE: return "cycle";
    case CM_IMPORT_LOOKUP_STALE_REVISION: return "stale revision";
    case CM_IMPORT_LOOKUP_FAILED_BUILD: return "failed build";
    case CM_IMPORT_LOOKUP_INVALID: return "invalid";
    }
    return "unknown import lookup status";
}
