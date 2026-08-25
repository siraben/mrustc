#include "cm/hir/model.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmHirPreboundAssociatedType {
    CmHirDefId definition;
    CmHirDefId parent_definition;
    CmHirModuleId owner_module;
    CmInternId name;
    CmSpan span;
} CmHirPreboundAssociatedType;

static int cm_hir_body_origin_valid(const CmHirBody *body);

static const void *cm_hir_get_id(const CmVec *arena, uint32_t id)
{
    if (id == 0u || (size_t)id > arena->len) {
        return NULL;
    }
    return cm_vec_at_const(arena, (size_t)id - 1u);
}

static void *cm_hir_get_id_mut(CmVec *arena, uint32_t id)
{
    if (id == 0u || (size_t)id > arena->len) {
        return NULL;
    }
    return cm_vec_at(arena, (size_t)id - 1u);
}

void cm_hir_context_record_semantic_mutation(CmHirContext *context)
{
    if (context == NULL) return;
    if (context->semantic_generation == UINT64_MAX) abort();
    context->semantic_generation += UINT64_C(1);
}

static CmHirStatus cm_hir_push(CmHirContext *context, CmVec *arena,
    const void *value, uint32_t *out_id)
{
    if (arena->len >= (size_t)UINT32_MAX) {
        return CM_HIR_ID_EXHAUSTED;
    }
    (void)cm_vec_push(arena, value);
    *out_id = (uint32_t)arena->len;
    cm_hir_context_record_semantic_mutation(context);
    return CM_HIR_OK;
}

static void *cm_hir_copy_array(CmHirContext *context, const void *values,
    uint32_t count, size_t element_size)
{
    void *copy;
    size_t byte_count;

    if (count == 0u) {
        return NULL;
    }
    if (!cm_size_mul((size_t)count, element_size, &byte_count)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    copy = cm_arena_alloc(&context->storage, byte_count, 16u);
    memcpy(copy, values, byte_count);
    return copy;
}

static int cm_hir_span_is_ordered(CmSpan span)
{
    return span.start <= span.end;
}

static int cm_hir_intern_id_valid(const CmHirContext *context,
    CmInternId id)
{
    return cm_interner_get(&context->strings, id) != NULL;
}

static int cm_hir_intern_id_nonempty(const CmHirContext *context,
    CmInternId id)
{
    const CmInternedString *string;

    string = cm_interner_get(&context->strings, id);
    return string != NULL && string->len != 0u;
}

static int cm_hir_intern_matches(const CmHirContext *context,
    CmInternId id, const char *text)
{
    const CmInternedString *string;
    size_t length;

    string = cm_interner_get(&context->strings, id);
    if (string == NULL) return 0;
    length = strlen(text);
    return string->len == length && memcmp(string->bytes, text, length) == 0;
}

CmHirDefId cm_hir_def_id_none(void)
{
    CmHirDefId id;

    id.crate_id = CM_HIR_CRATE_NONE;
    id.index = CM_HIR_DEF_INDEX_NONE;
    return id;
}

int cm_hir_def_id_is_none(CmHirDefId id)
{
    return id.crate_id == CM_HIR_CRATE_NONE
        && id.index == CM_HIR_DEF_INDEX_NONE;
}

int cm_hir_def_id_equal(CmHirDefId left, CmHirDefId right)
{
    return left.crate_id == right.crate_id && left.index == right.index;
}

static int cm_hir_def_id_has_valid_shape(CmHirDefId id)
{
    return (id.crate_id == CM_HIR_CRATE_NONE)
        == (id.index == CM_HIR_DEF_INDEX_NONE);
}

void cm_hir_context_init(CmHirContext *context)
{
    memset(context, 0, sizeof(*context));
    cm_arena_init(&context->storage, 4096u);
    cm_interner_init(&context->strings, 4096u);
    cm_vec_init(&context->crates, sizeof(CmHirCrate));
    cm_vec_init(&context->modules, sizeof(CmHirModule));
    cm_vec_init(&context->items, sizeof(CmHirItem));
    cm_vec_init(&context->bodies, sizeof(CmHirBody));
    cm_vec_init(&context->closures, sizeof(CmHirClosure));
    cm_vec_init(&context->expressions, sizeof(CmHirExpr));
    cm_vec_init(&context->types, sizeof(CmHirType));
    cm_vec_init(&context->generic_parameters, sizeof(CmHirGenericParam));
    cm_vec_init(&context->definitions, sizeof(CmHirDefinition));
    cm_vec_init(&context->prebound_associated_types,
        sizeof(CmHirPreboundAssociatedType));
    context->semantic_generation = UINT64_C(1);
    context->rewind_generation = UINT64_C(1);
}

void cm_hir_context_destroy(CmHirContext *context)
{
    size_t expression_index;

    for (expression_index = 0u;
         expression_index < context->expressions.len;
         ++expression_index) {
        CmHirExpr *expression;

        expression = (CmHirExpr *)cm_vec_at(&context->expressions,
            expression_index);
        cm_hir_release_expr_owned_storage(expression);
    }
    cm_vec_destroy(&context->prebound_associated_types);
    cm_vec_destroy(&context->definitions);
    cm_vec_destroy(&context->generic_parameters);
    cm_vec_destroy(&context->types);
    cm_vec_destroy(&context->expressions);
    cm_vec_destroy(&context->closures);
    cm_vec_destroy(&context->bodies);
    cm_vec_destroy(&context->items);
    cm_vec_destroy(&context->modules);
    cm_vec_destroy(&context->crates);
    cm_interner_destroy(&context->strings);
    cm_arena_destroy(&context->storage);
    memset(context, 0, sizeof(*context));
}

CmHirStatus cm_hir_context_mark(CmHirContext *context,
    CmHirContextMark *out_mark)
{
    if (context == NULL || out_mark == NULL) return CM_HIR_INVALID_ARGUMENT;
    memset(out_mark, 0, sizeof(*out_mark));
    out_mark->storage = cm_arena_mark(&context->storage);
    out_mark->strings = cm_interner_mark(&context->strings);
    out_mark->crates = context->crates.len;
    out_mark->modules = context->modules.len;
    out_mark->items = context->items.len;
    out_mark->bodies = context->bodies.len;
    out_mark->closures = context->closures.len;
    out_mark->expressions = context->expressions.len;
    out_mark->types = context->types.len;
    out_mark->generic_parameters = context->generic_parameters.len;
    out_mark->definitions = context->definitions.len;
    out_mark->prebound_associated_types =
        context->prebound_associated_types.len;
    out_mark->context = context;
    out_mark->active = 1;
    return CM_HIR_OK;
}

static int cm_hir_context_mark_is_valid(const CmHirContext *context,
    const CmHirContextMark *mark)
{
    return context != NULL && mark != NULL && mark->active
        && mark->context == context
        && mark->crates <= context->crates.len
        && mark->modules <= context->modules.len
        && mark->items <= context->items.len
        && mark->bodies <= context->bodies.len
        && mark->closures <= context->closures.len
        && mark->expressions <= context->expressions.len
        && mark->types <= context->types.len
        && mark->generic_parameters <= context->generic_parameters.len
        && mark->definitions <= context->definitions.len
        && mark->prebound_associated_types
            <= context->prebound_associated_types.len
        && cm_arena_mark_is_valid(&context->storage, mark->storage)
        && cm_interner_mark_is_valid(&context->strings, mark->strings);
}

CmHirStatus cm_hir_context_rewind(CmHirContext *context,
    CmHirContextMark *mark)
{
    size_t index;

    if (!cm_hir_context_mark_is_valid(context, mark)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    /* Rewind changes both observer identities.  Exhaustion must stop before
     * releasing owned payloads or truncating any context storage. */
    if (context->rewind_generation == UINT64_MAX
        || context->semantic_generation == UINT64_MAX) abort();
    for (index = mark->expressions; index < context->expressions.len;
         ++index) {
        cm_hir_release_expr_owned_storage(
            (CmHirExpr *)cm_vec_at(&context->expressions, index));
    }
    cm_vec_resize(&context->prebound_associated_types,
        mark->prebound_associated_types);
    cm_vec_resize(&context->definitions, mark->definitions);
    cm_vec_resize(&context->generic_parameters, mark->generic_parameters);
    cm_vec_resize(&context->types, mark->types);
    cm_vec_resize(&context->expressions, mark->expressions);
    cm_vec_resize(&context->closures, mark->closures);
    cm_vec_resize(&context->bodies, mark->bodies);
    cm_vec_resize(&context->items, mark->items);
    cm_vec_resize(&context->modules, mark->modules);
    cm_vec_resize(&context->crates, mark->crates);
    cm_interner_rewind(&context->strings, mark->strings);
    cm_interner_discard_mark(&context->strings, mark->strings);
    cm_arena_rewind(&context->storage, mark->storage);
    cm_arena_discard_mark(&context->storage, mark->storage);
    context->rewind_generation += UINT64_C(1);
    cm_hir_context_record_semantic_mutation(context);
    mark->context = NULL;
    mark->active = 0;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_context_commit(CmHirContext *context,
    CmHirContextMark *mark)
{
    if (!cm_hir_context_mark_is_valid(context, mark)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    cm_interner_discard_mark(&context->strings, mark->strings);
    cm_arena_discard_mark(&context->storage, mark->storage);
    mark->context = NULL;
    mark->active = 0;
    return CM_HIR_OK;
}

void cm_hir_release_expr_owned_storage(CmHirExpr *expression)
{
    void *owned_storage;

    if (expression == NULL) return;
    owned_storage = NULL;
    if (expression->kind == CM_HIR_EXPR_CALL
        && expression->data.call.owned_storage != NULL) {
        owned_storage = expression->data.call.owned_storage;
        expression->data.call.type_substitutions = NULL;
        expression->data.call.arguments = NULL;
        expression->data.call.owned_storage = NULL;
    } else if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
        && expression->data.qualified_call.owned_storage != NULL) {
        owned_storage = expression->data.qualified_call.owned_storage;
        expression->data.qualified_call.arguments = NULL;
        expression->data.qualified_call.owned_storage = NULL;
    } else if (expression->kind == CM_HIR_EXPR_AGGREGATE
        && expression->data.aggregate.owned_storage != NULL) {
        owned_storage = expression->data.aggregate.owned_storage;
        expression->data.aggregate.fields = NULL;
        expression->data.aggregate.owned_storage = NULL;
    }
    cm_free(owned_storage);
}

CmInternId cm_hir_intern(CmHirContext *context, const char *text)
{
    if (context == NULL || text == NULL) {
        return CM_INTERN_ID_NONE;
    }
    return cm_interner_intern_c_str(&context->strings, text);
}

const CmHirCrate *cm_hir_get_crate(const CmHirContext *context,
    CmHirCrateId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirCrate *)cm_hir_get_id(&context->crates, id);
}

const CmHirModule *cm_hir_get_module(const CmHirContext *context,
    CmHirModuleId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirModule *)cm_hir_get_id(&context->modules, id);
}

const CmHirItem *cm_hir_get_item(const CmHirContext *context,
    CmHirItemId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirItem *)cm_hir_get_id(&context->items, id);
}

const CmHirBody *cm_hir_get_body(const CmHirContext *context,
    CmHirBodyId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirBody *)cm_hir_get_id(&context->bodies, id);
}

const CmHirClosure *cm_hir_get_closure(const CmHirContext *context,
    CmHirClosureId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirClosure *)cm_hir_get_id(&context->closures, id);
}

const CmHirExpr *cm_hir_get_expr(const CmHirContext *context,
    CmHirExprId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirExpr *)cm_hir_get_id(&context->expressions, id);
}

const CmHirType *cm_hir_get_type(const CmHirContext *context,
    CmHirTypeId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirType *)cm_hir_get_id(&context->types, id);
}

const CmHirGenericParam *cm_hir_get_generic_param(
    const CmHirContext *context, CmHirGenericParamId id)
{
    if (context == NULL) {
        return NULL;
    }
    return (const CmHirGenericParam *)cm_hir_get_id(
        &context->generic_parameters, id);
}

const CmHirDefinition *cm_hir_lookup_definition(const CmHirContext *context,
    CmHirDefId id)
{
    size_t index;

    if (context == NULL || cm_hir_def_id_is_none(id)) {
        return NULL;
    }
    /* A freshly lowered local crate owns the only crate arena and allocates
     * definition indices in the same one-based order as this vector.  Keep
     * the general scan below for multi-crate/metadata contexts, where foreign
     * definitions may be interleaved and the index is only crate-local. */
    if (context->crates.len == 1u
        && (size_t)id.crate_id == context->crates.len
        && id.index != CM_HIR_DEF_INDEX_NONE
        && (size_t)id.index <= context->definitions.len) {
        const CmHirDefinition *definition;

        definition = (const CmHirDefinition *)cm_vec_at_const(
            &context->definitions, (size_t)id.index - 1u);
        if (definition != NULL && cm_hir_def_id_equal(definition->id, id)) {
            return definition;
        }
    }
    for (index = 0u; index < context->definitions.len; ++index) {
        const CmHirDefinition *definition;

        definition = (const CmHirDefinition *)cm_vec_at_const(
            &context->definitions, index);
        if (cm_hir_def_id_equal(definition->id, id)) {
            return definition;
        }
    }
    return NULL;
}

static CmHirDefinition *cm_hir_lookup_definition_mut(CmHirContext *context,
    CmHirDefId id)
{
    size_t index;

    if (context->crates.len == 1u
        && (size_t)id.crate_id == context->crates.len
        && id.index != CM_HIR_DEF_INDEX_NONE
        && (size_t)id.index <= context->definitions.len) {
        CmHirDefinition *definition;

        definition = (CmHirDefinition *)cm_vec_at(&context->definitions,
            (size_t)id.index - 1u);
        if (definition != NULL && cm_hir_def_id_equal(definition->id, id)) {
            return definition;
        }
    }
    for (index = 0u; index < context->definitions.len; ++index) {
        CmHirDefinition *definition;

        definition = (CmHirDefinition *)cm_vec_at(&context->definitions,
            index);
        if (cm_hir_def_id_equal(definition->id, id)) {
            return definition;
        }
    }
    return NULL;
}

static CmHirStatus cm_hir_reserve_definition(CmHirContext *context,
    CmHirCrateId crate_id, CmHirDefinitionKind kind, CmSpan span,
    CmHirDefId *out_definition)
{
    CmHirCrate *crate_value;
    CmHirDefinition definition;
    CmHirStatus status;
    uint32_t ignored_id;

    crate_value = (CmHirCrate *)cm_hir_get_id_mut(&context->crates,
        crate_id);
    if (crate_value == NULL) {
        return CM_HIR_INVALID_ID;
    }
    if (crate_value->next_definition_index == UINT32_MAX
        || context->definitions.len >= (size_t)UINT32_MAX) {
        return CM_HIR_ID_EXHAUSTED;
    }
    memset(&definition, 0, sizeof(definition));
    definition.id.crate_id = crate_id;
    definition.id.index = crate_value->next_definition_index;
    definition.kind = kind;
    definition.state = CM_HIR_DEFINITION_RESERVED;
    definition.span = span;
    status = cm_hir_push(context, &context->definitions, &definition,
        &ignored_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    crate_value->next_definition_index += 1u;
    *out_definition = definition.id;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_reserve_item_definition(CmHirContext *context,
    CmHirCrateId crate_id, CmSpan span, CmHirDefId *out_definition)
{
    if (context == NULL || out_definition == NULL
        || !cm_hir_span_is_ordered(span)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_definition = cm_hir_def_id_none();
    return cm_hir_reserve_definition(context, crate_id,
        CM_HIR_DEFINITION_ITEM, span, out_definition);
}

CmHirStatus cm_hir_reserve_item_definition_as(CmHirContext *context,
    CmHirCrateId crate_id, CmHirItemKind item_kind, CmSpan span,
    CmHirDefId *out_definition)
{
    CmHirDefinition *definition;
    CmHirStatus status;

    if (out_definition != NULL) {
        *out_definition = cm_hir_def_id_none();
    }
    if (context == NULL || out_definition == NULL
        || !cm_hir_span_is_ordered(span)
        || (unsigned int)item_kind >
            (unsigned int)CM_HIR_ITEM_TRAIT_ALIAS) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    status = cm_hir_reserve_definition(context, crate_id,
        CM_HIR_DEFINITION_ITEM, span, out_definition);
    if (status != CM_HIR_OK) return status;
    definition = cm_hir_lookup_definition_mut(context, *out_definition);
    if (definition == NULL) return CM_HIR_INVARIANT_VIOLATION;
    definition->reserved_item_kind = item_kind;
    definition->has_reserved_item_kind = 1;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_reserve_enum_variant_definition(CmHirContext *context,
    CmHirCrateId crate_id, CmSpan span, CmHirDefId *out_definition)
{
    if (context == NULL || out_definition == NULL
        || !cm_hir_span_is_ordered(span)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_definition = cm_hir_def_id_none();
    return cm_hir_reserve_definition(context, crate_id,
        CM_HIR_DEFINITION_ENUM_VARIANT, span, out_definition);
}

CmHirStatus cm_hir_add_macro_definition(CmHirContext *context,
    CmHirModuleId owner_module, CmInternId name,
    CmHirMacroDefinitionForm form, CmSpan span,
    CmHirDefId *out_definition)
{
    const CmHirModule *module;
    CmHirDefinition *definition;
    CmHirStatus status;

    if (out_definition != NULL) {
        *out_definition = cm_hir_def_id_none();
    }
    if (context == NULL || out_definition == NULL
        || !cm_hir_intern_id_nonempty(context, name)
        || !cm_hir_span_is_ordered(span)
        || (unsigned int)form
            > (unsigned int)CM_HIR_MACRO_DECLARATIVE_DEFINITION) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    module = cm_hir_get_module(context, owner_module);
    if (module == NULL) return CM_HIR_INVALID_ID;
    status = cm_hir_reserve_definition(context, module->crate_id,
        CM_HIR_DEFINITION_MACRO, span, out_definition);
    if (status != CM_HIR_OK) return status;
    definition = cm_hir_lookup_definition_mut(context, *out_definition);
    if (definition == NULL) return CM_HIR_INVARIANT_VIOLATION;
    definition->state = CM_HIR_DEFINITION_BOUND;
    definition->entity.macro_definition.owner_module = owner_module;
    definition->entity.macro_definition.name = name;
    definition->entity.macro_definition.form = form;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_create_crate(CmHirContext *context, CmInternId name,
    CmHirEdition edition, CmSpan span, CmHirCrateId *out_crate,
    CmHirModuleId *out_root_module)
{
    CmHirCrate crate_value;
    CmHirModule module;
    CmHirDefinition definition;
    CmHirCrateId crate_id;
    CmHirModuleId module_id;
    uint32_t definition_id;
    CmHirStatus status;

    if (context == NULL || out_crate == NULL || out_root_module == NULL
        || !cm_hir_intern_id_valid(context, name)
        || !cm_hir_span_is_ordered(span)
        || (unsigned int)edition > (unsigned int)CM_HIR_EDITION_2024) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_crate = CM_HIR_CRATE_NONE;
    *out_root_module = CM_HIR_MODULE_NONE;
    if (context->crates.len >= (size_t)UINT32_MAX
        || context->modules.len >= (size_t)UINT32_MAX
        || context->definitions.len >= (size_t)UINT32_MAX) {
        return CM_HIR_ID_EXHAUSTED;
    }
    memset(&crate_value, 0, sizeof(crate_value));
    crate_value.name = name;
    crate_value.edition = edition;
    crate_value.span = span;
    crate_value.next_definition_index = 2u;
    status = cm_hir_push(context, &context->crates, &crate_value, &crate_id);
    if (status != CM_HIR_OK) {
        return status;
    }

    memset(&module, 0, sizeof(module));
    module.crate_id = crate_id;
    module.name = name;
    module.span = span;
    module.definition.crate_id = crate_id;
    module.definition.index = 1u;
    status = cm_hir_push(context, &context->modules, &module, &module_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    ((CmHirCrate *)cm_hir_get_id_mut(&context->crates, crate_id))
        ->root_module = module_id;

    memset(&definition, 0, sizeof(definition));
    definition.id = module.definition;
    definition.kind = CM_HIR_DEFINITION_MODULE;
    definition.state = CM_HIR_DEFINITION_BOUND;
    definition.span = span;
    definition.entity.module_id = module_id;
    status = cm_hir_push(context, &context->definitions, &definition,
        &definition_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    *out_crate = crate_id;
    *out_root_module = module_id;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_add_module(CmHirContext *context, CmHirCrateId crate_id,
    CmHirModuleId parent, CmInternId name, CmSpan span,
    CmHirModuleId *out_id)
{
    const CmHirModule *parent_module;
    CmHirModule module;
    CmHirDefinition definition;
    CmHirDefId definition_id;
    CmHirStatus status;

    if (context == NULL || out_id == NULL
        || !cm_hir_intern_id_valid(context, name)
        || !cm_hir_span_is_ordered(span)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_MODULE_NONE;
    parent_module = cm_hir_get_module(context, parent);
    if (parent_module == NULL || parent_module->crate_id != crate_id) {
        return CM_HIR_INVALID_ID;
    }
    if (context->modules.len >= (size_t)UINT32_MAX) {
        return CM_HIR_ID_EXHAUSTED;
    }
    status = cm_hir_reserve_definition(context, crate_id,
        CM_HIR_DEFINITION_MODULE, span, &definition_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    memset(&module, 0, sizeof(module));
    module.crate_id = crate_id;
    module.parent = parent;
    module.definition = definition_id;
    module.name = name;
    module.span = span;
    status = cm_hir_push(context, &context->modules, &module, out_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    definition = *cm_hir_lookup_definition(context, definition_id);
    definition.state = CM_HIR_DEFINITION_BOUND;
    definition.entity.module_id = *out_id;
    *cm_hir_lookup_definition_mut(context, definition_id) = definition;
    return CM_HIR_OK;
}

static int cm_hir_attributes_valid(const CmHirContext *context,
    const CmHirAttribute *attributes, uint32_t attribute_count)
{
    uint32_t index;

    if (attribute_count != 0u && attributes == NULL) return 0;
    for (index = 0u; index < attribute_count; ++index) {
        if (!cm_hir_intern_id_valid(context, attributes[index].metadata)
            || attributes[index].span.source == 0u
            || !cm_hir_span_is_ordered(attributes[index].span)
            || attributes[index].source_attribute == 0u) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_visibility_valid(const CmHirContext *context,
    const CmHirVisibility *visibility);

static int cm_hir_import_visibility_valid(const CmHirContext *context,
    CmHirModuleId owner, const CmHirVisibility *visibility)
{
    CmHirModuleId current;

    if (!cm_hir_visibility_valid(context, visibility)) return 0;
    if (visibility->kind != CM_HIR_VIS_RESTRICTED) return 1;
    current = owner;
    while (current != CM_HIR_MODULE_NONE) {
        const CmHirModule *module;

        module = cm_hir_get_module(context, current);
        if (module == NULL) return 0;
        if (cm_hir_def_id_equal(module->definition,
                visibility->restriction)) {
            return 1;
        }
        current = module->parent;
    }
    return 0;
}

CmHirStatus cm_hir_set_crate_inner_attributes(CmHirContext *context,
    CmHirCrateId crate_id, const CmHirAttribute *attributes,
    uint32_t attribute_count)
{
    CmHirCrate *crate_value;

    if (context == NULL
        || !cm_hir_attributes_valid(context, attributes,
            attribute_count)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    crate_value = (CmHirCrate *)cm_hir_get_id_mut(&context->crates,
        crate_id);
    if (crate_value == NULL) return CM_HIR_INVALID_ID;
    if (crate_value->inner_attributes != NULL
        || crate_value->inner_attribute_count != 0u) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    crate_value->inner_attributes = (CmHirAttribute *)cm_hir_copy_array(
        context, attributes, attribute_count, sizeof(CmHirAttribute));
    crate_value->inner_attribute_count = attribute_count;
    if (attribute_count != 0u) {
        cm_hir_context_record_semantic_mutation(context);
    }
    return CM_HIR_OK;
}

CmHirStatus cm_hir_set_module_inner_attributes(CmHirContext *context,
    CmHirModuleId module_id, const CmHirAttribute *attributes,
    uint32_t attribute_count)
{
    CmHirModule *module;

    if (context == NULL
        || !cm_hir_attributes_valid(context, attributes,
            attribute_count)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    module = (CmHirModule *)cm_hir_get_id_mut(&context->modules, module_id);
    if (module == NULL) return CM_HIR_INVALID_ID;
    if (module->inner_attributes != NULL
        || module->inner_attribute_count != 0u) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    module->inner_attributes = (CmHirAttribute *)cm_hir_copy_array(context,
        attributes, attribute_count, sizeof(CmHirAttribute));
    module->inner_attribute_count = attribute_count;
    if (attribute_count != 0u) {
        cm_hir_context_record_semantic_mutation(context);
    }
    return CM_HIR_OK;
}

CmHirStatus cm_hir_set_module_outer_attributes(CmHirContext *context,
    CmHirModuleId module_id, const CmHirAttribute *attributes,
    uint32_t attribute_count)
{
    CmHirModule *module;

    if (context == NULL
        || !cm_hir_attributes_valid(context, attributes,
            attribute_count)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    module = (CmHirModule *)cm_hir_get_id_mut(&context->modules, module_id);
    if (module == NULL) return CM_HIR_INVALID_ID;
    if (attribute_count != 0u && module->parent == CM_HIR_MODULE_NONE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    if (module->outer_attributes != NULL
        || module->outer_attribute_count != 0u) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    module->outer_attributes = (CmHirAttribute *)cm_hir_copy_array(context,
        attributes, attribute_count, sizeof(CmHirAttribute));
    module->outer_attribute_count = attribute_count;
    if (attribute_count != 0u) {
        cm_hir_context_record_semantic_mutation(context);
    }
    return CM_HIR_OK;
}

static int cm_hir_import_variant_target_valid(const CmHirContext *context,
    const CmHirDefinition *target, CmHirNamespace namespace_kind)
{
    const CmHirItem *enumeration;
    const CmHirVariant *variant;

    if (target->kind != CM_HIR_DEFINITION_ENUM_VARIANT) return 1;
    if (namespace_kind != CM_HIR_NAMESPACE_TYPE
        && namespace_kind != CM_HIR_NAMESPACE_VALUE) return 0;
    if (target->state == CM_HIR_DEFINITION_RESERVED) return 1;
    if (target->state != CM_HIR_DEFINITION_BOUND) return 0;
    enumeration = cm_hir_get_item(context,
        target->entity.enum_variant.enum_item_id);
    if (enumeration == NULL || enumeration->kind != CM_HIR_ITEM_ENUM
        || target->entity.enum_variant.variant_index
            >= enumeration->data.enum_item.variant_count) {
        return 0;
    }
    variant = &enumeration->data.enum_item.variants[
        target->entity.enum_variant.variant_index];
    if (!cm_hir_def_id_equal(variant->definition, target->id)) return 0;
    return namespace_kind == CM_HIR_NAMESPACE_TYPE
        || variant->form != CM_HIR_AGGREGATE_NAMED;
}

CmHirStatus cm_hir_set_module_imports(CmHirContext *context,
    CmHirModuleId module_id, const CmHirImport *imports,
    uint32_t import_count)
{
    CmHirModule *module;
    CmHirImport *copies;
    uint32_t import_index;

    if (context == NULL || (import_count != 0u && imports == NULL)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    module = (CmHirModule *)cm_hir_get_id_mut(&context->modules, module_id);
    if (module == NULL) return CM_HIR_INVALID_ID;
    if (import_count == 0u) return CM_HIR_OK;
    if (module->imports != NULL || module->import_count != 0u) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    for (import_index = 0u; import_index < import_count; ++import_index) {
        const CmHirImport *import_value;
        uint32_t binding_index;

        import_value = &imports[import_index];
        if ((unsigned int)import_value->kind
                > (unsigned int)CM_HIR_IMPORT_EXTERN_CRATE
            || !cm_hir_intern_id_nonempty(context, import_value->tree)
            || !cm_hir_import_visibility_valid(context, module_id,
                &import_value->visibility)
            || import_value->span.source == 0u
            || !cm_hir_span_is_ordered(import_value->span)
            || import_value->source_item == 0u
            || !cm_hir_attributes_valid(context, import_value->attributes,
                import_value->attribute_count)
            || (import_value->kind == CM_HIR_IMPORT_EXTERN_CRATE
                && import_value->binding_count != 1u)
            || (import_value->binding_count != 0u
                && import_value->bindings == NULL)) {
            return CM_HIR_INVALID_ARGUMENT;
        }
        for (binding_index = 0u;
             binding_index < import_value->binding_count;
             ++binding_index) {
            const CmHirImportBinding *binding;
            const CmHirDefinition *target;

            binding = &import_value->bindings[binding_index];
            target = cm_hir_def_id_is_none(binding->target) ? NULL
                : cm_hir_lookup_definition(context, binding->target);
            if (!cm_hir_intern_id_nonempty(context, binding->name)) {
                return CM_HIR_INVALID_ID;
            }
            if ((unsigned int)binding->namespace_kind >
                    (unsigned int)CM_HIR_NAMESPACE_MACRO
                || (unsigned int)binding->primitive_kind >
                    (unsigned int)CM_HIR_PRIMITIVE_F128
                || (binding->is_anonymous != 0
                    && binding->is_anonymous != 1)
                || (binding->is_anonymous
                    != cm_hir_intern_matches(context, binding->name, "_"))
                || (binding->primitive_kind != CM_HIR_PRIMITIVE_NONE
                    && (target != NULL
                        || binding->namespace_kind
                            != CM_HIR_NAMESPACE_TYPE
                        || binding->is_anonymous))
                || (import_value->kind == CM_HIR_IMPORT_EXTERN_CRATE
                    && (binding->namespace_kind != CM_HIR_NAMESPACE_TYPE
                        || target == NULL
                        || target->kind != CM_HIR_DEFINITION_MODULE
                        || binding->primitive_kind
                            != CM_HIR_PRIMITIVE_NONE
                        || binding->is_anonymous))
                || (target != NULL
                    && target->kind == CM_HIR_DEFINITION_MODULE
                    && binding->namespace_kind != CM_HIR_NAMESPACE_TYPE)
                || (binding->namespace_kind == CM_HIR_NAMESPACE_MACRO
                    && (target == NULL
                        || target->kind != CM_HIR_DEFINITION_MACRO
                        || target->state != CM_HIR_DEFINITION_BOUND))
                || (target != NULL
                    && target->kind == CM_HIR_DEFINITION_MACRO
                    && binding->namespace_kind
                        != CM_HIR_NAMESPACE_MACRO)
                || (target != NULL
                    && !cm_hir_import_variant_target_valid(context, target,
                        binding->namespace_kind))) {
                return CM_HIR_INVALID_ARGUMENT;
            }
            if (binding->primitive_kind == CM_HIR_PRIMITIVE_NONE
                && target == NULL) return CM_HIR_INVALID_ID;
        }
    }
    copies = (CmHirImport *)cm_hir_copy_array(context, imports,
        import_count, sizeof(CmHirImport));
    for (import_index = 0u; import_index < import_count; ++import_index) {
        copies[import_index].attributes =
            (CmHirAttribute *)cm_hir_copy_array(context,
                imports[import_index].attributes,
                imports[import_index].attribute_count,
                sizeof(CmHirAttribute));
        copies[import_index].bindings =
            (CmHirImportBinding *)cm_hir_copy_array(context,
                imports[import_index].bindings,
                imports[import_index].binding_count,
                sizeof(CmHirImportBinding));
    }
    module->imports = copies;
    module->import_count = import_count;
    cm_hir_context_record_semantic_mutation(context);
    return CM_HIR_OK;
}

static int cm_hir_type_id_valid(const CmHirContext *context, CmHirTypeId id)
{
    return cm_hir_get_type(context, id) != NULL;
}

static int cm_hir_region_valid(const CmHirContext *context,
    const CmHirRegion *region)
{
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            region->data.parameter);
        return parameter != NULL
            && parameter->kind == CM_HIR_GENERIC_LIFETIME;
    }
    case CM_HIR_REGION_LATE_BOUND:
    case CM_HIR_REGION_INFER:
        return 1;
    case CM_HIR_REGION_ERROR:
        return cm_hir_intern_id_valid(context,
            region->data.error_reason);
    }
    return 0;
}

static int cm_hir_lifetime_binder_valid(const CmHirContext *context,
    const CmHirLifetimeBinder *binder, CmSpan container_span,
    int require_nonempty);

static int cm_hir_requirement_merge_region(const CmHirRegion *region,
    uint32_t *requirement)
{
    uint32_t needed;

    if (region->kind != CM_HIR_REGION_LATE_BOUND) return 1;
    if (region->data.binder_index == UINT32_MAX) return 0;
    needed = region->data.binder_index + 1u;
    if (needed > *requirement) *requirement = needed;
    return 1;
}

static int cm_hir_requirement_merge_type(const CmHirContext *context,
    CmHirTypeId type_id, uint32_t *requirement)
{
    const CmHirType *child;

    child = cm_hir_get_type(context, type_id);
    if (child == NULL) return 0;
    if (child->late_bound_requirement > *requirement) {
        *requirement = child->late_bound_requirement;
    }
    return 1;
}

static int cm_hir_requirement_merge_argument(const CmHirContext *context,
    const CmHirGenericArg *argument, uint32_t *requirement)
{
    if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
        return cm_hir_requirement_merge_region(&argument->data.lifetime,
            requirement);
    }
    if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
        return cm_hir_requirement_merge_type(context, argument->data.type,
            requirement);
    }
    return argument->kind == CM_HIR_GENERIC_ARG_CONST
        && cm_hir_requirement_merge_type(context,
            argument->data.constant.type, requirement);
}

static int cm_hir_requirement_merge_named(const CmHirContext *context,
    const CmHirNamedType *named, uint32_t *requirement)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_requirement_merge_argument(context,
                &named->arguments[index], requirement)) return 0;
    }
    return 1;
}

/* Type IDs only point at already committed nodes, so this is O(immediate
 * children).  Nested function pointers consume their cached requirement. */
static int cm_hir_type_late_bound_requirement(
    const CmHirContext *context, const CmHirType *type,
    uint32_t *out_requirement)
{
    uint32_t requirement;
    uint32_t index;

    if (context == NULL || type == NULL || out_requirement == NULL) return 0;
    requirement = 0u;
#define CM_HIR_REQUIRE_TYPE(child_id) do { \
        if (!cm_hir_requirement_merge_type(context, (child_id), \
                &requirement)) return 0; \
    } while (0)
    switch (type->kind) {
    case CM_HIR_TYPE_REFERENCE_KIND:
        if (!cm_hir_requirement_merge_region(
                &type->data.reference_type.region, &requirement)) return 0;
        CM_HIR_REQUIRE_TYPE(type->data.reference_type.pointee);
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        CM_HIR_REQUIRE_TYPE(type->data.raw_pointer_type.pointee);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            CM_HIR_REQUIRE_TYPE(type->data.tuple_type.elements[index]);
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        CM_HIR_REQUIRE_TYPE(type->data.array_type.element);
        CM_HIR_REQUIRE_TYPE(type->data.array_type.length.type);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        CM_HIR_REQUIRE_TYPE(type->data.slice_type.element);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (!cm_hir_lifetime_binder_valid(context,
                &type->data.fn_pointer_type.binder, type->span, 0)) return 0;
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            const CmHirType *child = cm_hir_get_type(context,
                type->data.fn_pointer_type.parameters[index]);
            if (child == NULL || child->late_bound_requirement
                    > type->data.fn_pointer_type.binder.lifetime_count)
                return 0;
        }
        {
            const CmHirType *child = cm_hir_get_type(context,
                type->data.fn_pointer_type.return_type);
            if (child == NULL || child->late_bound_requirement
                    > type->data.fn_pointer_type.binder.lifetime_count)
                return 0;
        }
        requirement = 0u;
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        if (!cm_hir_requirement_merge_named(context,
                &type->data.named_type, &requirement)) return 0;
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        CM_HIR_REQUIRE_TYPE(type->data.projection_type.self_type);
        if (!cm_hir_requirement_merge_named(context,
                &type->data.projection_type.trait_type, &requirement)
            || !cm_hir_requirement_merge_named(context,
                &type->data.projection_type.associated_type, &requirement))
            return 0;
        break;
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if (!cm_hir_requirement_merge_region(
                &type->data.dyn_trait_type.region, &requirement)) return 0;
        if (type->data.dyn_trait_type.has_principal
            && !cm_hir_requirement_merge_named(context,
                &type->data.dyn_trait_type.principal_trait, &requirement))
            return 0;
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!cm_hir_requirement_merge_named(context,
                    &type->data.dyn_trait_type.auto_traits[index],
                    &requirement)) return 0;
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index)
            CM_HIR_REQUIRE_TYPE(
                type->data.dyn_trait_type.equalities[index].value);
        break;
    default:
        break;
    }
#undef CM_HIR_REQUIRE_TYPE
    *out_requirement = requirement;
    return 1;
}

static int cm_hir_const_valid(const CmHirContext *context,
    const CmHirConstArg *constant)
{
    if (!cm_hir_type_id_valid(context, constant->type)
        || (unsigned int)constant->kind >
            (unsigned int)CM_HIR_CONST_ERROR) {
        return 0;
    }
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
    case CM_HIR_CONST_INFER:
        return 1;
    case CM_HIR_CONST_PARAMETER:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            constant->data.parameter);
        return parameter != NULL && parameter->kind == CM_HIR_GENERIC_CONST;
    }
    case CM_HIR_CONST_UNEVALUATED:
    {
        const CmHirDefinition *definition;

        definition = cm_hir_lookup_definition(context,
            constant->data.definition);
        return definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ITEM;
    }
    case CM_HIR_CONST_ERROR:
        return cm_hir_intern_id_valid(context,
            constant->data.error_reason);
    }
    return 0;
}

static int cm_hir_generic_arg_valid(const CmHirContext *context,
    const CmHirGenericArg *argument)
{
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_hir_region_valid(context, &argument->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_hir_type_id_valid(context, argument->data.type);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_const_valid(context, &argument->data.constant);
    }
    return 0;
}

static int cm_hir_named_type_valid(const CmHirContext *context,
    const CmHirNamedType *named)
{
    const CmHirDefinition *definition;
    uint32_t index;

    definition = cm_hir_lookup_definition(context, named->definition);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || (named->argument_count != 0u && named->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_generic_arg_valid(context, &named->arguments[index])) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_generic_argument_matches_parameter(
    const CmHirGenericArg *argument, const CmHirGenericParam *parameter)
{
    if (argument == NULL || parameter == NULL) return 0;
    switch (parameter->kind) {
    case CM_HIR_GENERIC_LIFETIME:
        return argument->kind == CM_HIR_GENERIC_ARG_LIFETIME;
    case CM_HIR_GENERIC_TYPE:
        return argument->kind == CM_HIR_GENERIC_ARG_TYPE;
    case CM_HIR_GENERIC_CONST:
        return argument->kind == CM_HIR_GENERIC_ARG_CONST;
    }
    return 0;
}

static int cm_hir_named_type_matches_item_parameters(
    const CmHirContext *context, const CmHirNamedType *named,
    const CmHirItem *target)
{
    uint32_t index;

    if (named == NULL || target == NULL
        || named->argument_count != target->generic_parameter_count
        || (named->argument_count != 0u && named->arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            target->generic_parameter_start + index);
        if (parameter == NULL || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner, target->definition)
            || !cm_hir_generic_argument_matches_parameter(
                &named->arguments[index], parameter)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_named_type_matches_reserved_parameters(
    const CmHirContext *context, const CmHirNamedType *named,
    CmHirDefId definition)
{
    uint32_t index;
    size_t parameter_index;
    size_t owned_count;

    owned_count = 0u;
    for (parameter_index = 0u;
         parameter_index < context->generic_parameters.len;
         ++parameter_index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &context->generic_parameters, parameter_index);
        if (parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, definition)) {
            owned_count += 1u;
        }
    }
    if (owned_count != (size_t)named->argument_count) return 0;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericParam *matched;

        matched = NULL;
        for (parameter_index = 0u;
             parameter_index < context->generic_parameters.len;
             ++parameter_index) {
            const CmHirGenericParam *parameter;

            parameter = (const CmHirGenericParam *)cm_vec_at_const(
                &context->generic_parameters, parameter_index);
            if (parameter != NULL && parameter->index == index
                && cm_hir_def_id_equal(parameter->owner, definition)) {
                matched = parameter;
                break;
            }
        }
        if (!cm_hir_generic_argument_matches_parameter(
                &named->arguments[index], matched)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_region_equal(const CmHirRegion *left,
    const CmHirRegion *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_REGION_STATIC:
    case CM_HIR_REGION_ERASED:
        return 1;
    case CM_HIR_REGION_EARLY_BOUND:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_REGION_LATE_BOUND:
        return left->data.binder_index == right->data.binder_index;
    case CM_HIR_REGION_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_REGION_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_hir_const_arg_equal(const CmHirConstArg *left,
    const CmHirConstArg *right)
{
    if (left->kind != right->kind || left->type != right->type) return 0;
    switch (left->kind) {
    case CM_HIR_CONST_VALUE:
        return left->data.value.low_bits == right->data.value.low_bits
            && left->data.value.high_bits == right->data.value.high_bits;
    case CM_HIR_CONST_PARAMETER:
        return left->data.parameter == right->data.parameter;
    case CM_HIR_CONST_UNEVALUATED:
        return cm_hir_def_id_equal(left->data.definition,
            right->data.definition);
    case CM_HIR_CONST_INFER:
        return left->data.inference_variable
            == right->data.inference_variable;
    case CM_HIR_CONST_ERROR:
        return left->data.error_reason == right->data.error_reason;
    }
    return 0;
}

static int cm_hir_generic_arg_equal(const CmHirGenericArg *left,
    const CmHirGenericArg *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_hir_region_equal(&left->data.lifetime,
            &right->data.lifetime);
    case CM_HIR_GENERIC_ARG_TYPE:
        return left->data.type == right->data.type;
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_const_arg_equal(&left->data.constant,
            &right->data.constant);
    }
    return 0;
}

static int cm_hir_named_type_equal(const CmHirNamedType *left,
    const CmHirNamedType *right)
{
    uint32_t index;

    if (!cm_hir_def_id_equal(left->definition, right->definition)
        || left->argument_count != right->argument_count) {
        return 0;
    }
    for (index = 0u; index < left->argument_count; ++index) {
        if (!cm_hir_generic_arg_equal(&left->arguments[index],
                &right->arguments[index])) {
            return 0;
        }
    }
    return 1;
}

static CmHirNamedType cm_hir_copy_named_type(CmHirContext *context,
    const CmHirNamedType *named)
{
    CmHirNamedType copy;

    copy = *named;
    copy.arguments = (CmHirGenericArg *)cm_hir_copy_array(context,
        named->arguments, named->argument_count, sizeof(CmHirGenericArg));
    return copy;
}

static const CmHirItem *cm_hir_bound_definition_item(
    const CmHirContext *context, CmHirDefId id);
static const CmHirPreboundAssociatedType *cm_hir_find_prebound_associated(
    const CmHirContext *context, CmHirDefId associated,
    CmHirDefId parent);

static int cm_hir_def_id_less(CmHirDefId left, CmHirDefId right)
{
    return left.crate_id < right.crate_id
        || (left.crate_id == right.crate_id && left.index < right.index);
}

static int cm_hir_dyn_trait_valid(const CmHirContext *context,
    const CmHirType *type)
{
    const CmHirItem *item;
    uint32_t index;

    if ((type->data.dyn_trait_type.has_principal != 0
            && type->data.dyn_trait_type.has_principal != 1)
        || (type->data.dyn_trait_type.auto_trait_count == 0u)
            != (type->data.dyn_trait_type.auto_traits == NULL)
        || (type->data.dyn_trait_type.equality_count == 0u)
            != (type->data.dyn_trait_type.equalities == NULL)
        || (!type->data.dyn_trait_type.has_principal
            && type->data.dyn_trait_type.auto_trait_count == 0u)
        || (!type->data.dyn_trait_type.has_principal
            && type->data.dyn_trait_type.equality_count != 0u)
        || !cm_hir_region_valid(context,
            &type->data.dyn_trait_type.region)) {
        return 0;
    }
    if (type->data.dyn_trait_type.has_principal) {
        if (!cm_hir_named_type_valid(context,
                &type->data.dyn_trait_type.principal_trait)) return 0;
        item = cm_hir_bound_definition_item(context,
            type->data.dyn_trait_type.principal_trait.definition);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT
            || item->data.trait_item.is_auto
            || !cm_hir_named_type_matches_item_parameters(context,
                &type->data.dyn_trait_type.principal_trait, item)) return 0;
    } else if (!cm_hir_def_id_is_none(
            type->data.dyn_trait_type.principal_trait.definition)
        || type->data.dyn_trait_type.principal_trait.arguments != NULL
        || type->data.dyn_trait_type.principal_trait.argument_count != 0u) {
        return 0;
    }
    for (index = 0u; index < type->data.dyn_trait_type.equality_count;
         ++index) {
        const CmHirAssociatedTypeEquality *equality;
        const CmHirDefinition *definition;
        const CmHirItem *associated;

        equality = &type->data.dyn_trait_type.equalities[index];
        definition = cm_hir_lookup_definition(context,
            equality->associated_type);
        associated = cm_hir_bound_definition_item(context,
            equality->associated_type);
        if (definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM
            || !cm_hir_type_id_valid(context, equality->value)
            || !cm_hir_span_is_ordered(equality->span)
            || equality->span.source != type->span.source
            || equality->span.start < type->span.start
            || equality->span.end > type->span.end
            || (index != 0u
                && !cm_hir_def_id_less(
                    type->data.dyn_trait_type.equalities[index - 1u]
                        .associated_type,
                    equality->associated_type))) {
            return 0;
        }
        if (associated != NULL) {
            if (associated->kind != CM_HIR_ITEM_TYPE_ALIAS
                || associated->generic_parameter_count != 0u
                || associated->data.type_alias_item.target
                    != CM_HIR_TYPE_NONE
                || !cm_hir_def_id_equal(associated->parent_definition,
                    type->data.dyn_trait_type.principal_trait.definition)) {
                return 0;
            }
        } else if (definition->state != CM_HIR_DEFINITION_RESERVED
            || !definition->has_reserved_item_kind
            || definition->reserved_item_kind != CM_HIR_ITEM_TYPE_ALIAS
            || cm_hir_find_prebound_associated(context,
                equality->associated_type,
                type->data.dyn_trait_type.principal_trait.definition)
                == NULL) {
            return 0;
        }
    }
    for (index = 0u;
         index < type->data.dyn_trait_type.auto_trait_count; ++index) {
        const CmHirNamedType *marker;

        marker = &type->data.dyn_trait_type.auto_traits[index];
        if (!cm_hir_named_type_valid(context, marker)) return 0;
        item = cm_hir_bound_definition_item(context, marker->definition);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT
            || !item->data.trait_item.is_auto
            || !cm_hir_named_type_matches_item_parameters(context,
                marker, item)
            || (index != 0u && !cm_hir_def_id_less(
                type->data.dyn_trait_type.auto_traits[index - 1u].definition,
                marker->definition))) {
            return 0;
        }
    }
    return 1;
}

static CmHirNamedType *cm_hir_copy_named_types(CmHirContext *context,
    const CmHirNamedType *named, uint32_t count)
{
    CmHirNamedType *copy;
    uint32_t index;

    copy = (CmHirNamedType *)cm_hir_copy_array(context, named, count,
        sizeof(*copy));
    if (copy == NULL) return NULL;
    for (index = 0u; index < count; ++index) {
        copy[index] = cm_hir_copy_named_type(context, &named[index]);
    }
    return copy;
}

static const CmHirItem *cm_hir_bound_definition_item(
    const CmHirContext *context, CmHirDefId id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(context, id);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(context, definition->entity.item_id);
    if (item == NULL || !cm_hir_def_id_equal(item->definition, id)) {
        return NULL;
    }
    return item;
}

static const CmHirPreboundAssociatedType *cm_hir_find_prebound_associated(
    const CmHirContext *context, CmHirDefId associated,
    CmHirDefId parent)
{
    size_t index;

    for (index = 0u; index < context->prebound_associated_types.len;
         ++index) {
        const CmHirPreboundAssociatedType *prebound;

        prebound = (const CmHirPreboundAssociatedType *)cm_vec_at_const(
            &context->prebound_associated_types, index);
        if (prebound != NULL
            && cm_hir_def_id_equal(prebound->definition, associated)
            && cm_hir_def_id_equal(prebound->parent_definition, parent)) {
            return prebound;
        }
    }
    return NULL;
}

static int cm_hir_projection_valid(const CmHirContext *context,
    const CmHirType *type)
{
    const CmHirDefinition *associated_definition;
    const CmHirDefinition *trait_definition;
    const CmHirPreboundAssociatedType *prebound;
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;

    if (!cm_hir_type_id_valid(context,
            type->data.projection_type.self_type)
        || !cm_hir_named_type_valid(context,
            &type->data.projection_type.trait_type)
        || !cm_hir_named_type_valid(context,
            &type->data.projection_type.associated_type)) {
        return 0;
    }
    trait_item = cm_hir_bound_definition_item(context,
        type->data.projection_type.trait_type.definition);
    trait_definition = cm_hir_lookup_definition(context,
        type->data.projection_type.trait_type.definition);
    associated_definition = cm_hir_lookup_definition(context,
        type->data.projection_type.associated_type.definition);
    associated_item = cm_hir_bound_definition_item(context,
        type->data.projection_type.associated_type.definition);
    if (associated_item != NULL) {
        return trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
            && cm_hir_named_type_matches_item_parameters(context,
                &type->data.projection_type.trait_type, trait_item)
            && associated_item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && associated_item->data.type_alias_item.target
                == CM_HIR_TYPE_NONE
            && cm_hir_def_id_equal(associated_item->parent_definition,
                trait_item->definition)
            && cm_hir_named_type_matches_item_parameters(context,
                &type->data.projection_type.associated_type,
                associated_item);
    }
    prebound = cm_hir_find_prebound_associated(context,
        type->data.projection_type.associated_type.definition,
        type->data.projection_type.trait_type.definition);
    if (trait_item != NULL) {
        return trait_item->kind == CM_HIR_ITEM_TRAIT
            && cm_hir_named_type_matches_item_parameters(context,
                &type->data.projection_type.trait_type,
                trait_item)
            && associated_definition != NULL
            && associated_definition->kind == CM_HIR_DEFINITION_ITEM
            && associated_definition->state == CM_HIR_DEFINITION_RESERVED
            && associated_definition->has_reserved_item_kind
            && associated_definition->reserved_item_kind
                == CM_HIR_ITEM_TYPE_ALIAS
            && type->data.projection_type.associated_type.argument_count
                == 0u
            && prebound != NULL;
    }
    return trait_definition != NULL
        && trait_definition->kind == CM_HIR_DEFINITION_ITEM
        && trait_definition->state == CM_HIR_DEFINITION_RESERVED
        && trait_definition->has_reserved_item_kind
        && trait_definition->reserved_item_kind == CM_HIR_ITEM_TRAIT
        && cm_hir_named_type_matches_reserved_parameters(context,
            &type->data.projection_type.trait_type,
            trait_definition->id)
        && type->data.projection_type.associated_type.argument_count == 0u
        && prebound != NULL;
}

CmHirStatus cm_hir_add_type(CmHirContext *context, const CmHirType *type,
    CmHirTypeId *out_id)
{
    CmHirType copy;
    uint32_t late_bound_requirement;
    uint32_t index;
    int valid;

    if (context == NULL || type == NULL || out_id == NULL
        || !cm_hir_span_is_ordered(type->span)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_TYPE_NONE;
    valid = 1;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
        valid = cm_hir_intern_id_valid(context,
            type->data.error_type.reason);
        break;
    case CM_HIR_TYPE_INFER_KIND:
        valid = (unsigned int)type->data.infer_type.kind <=
            (unsigned int)CM_HIR_INFER_FLOAT;
        break;
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        break;
    case CM_HIR_TYPE_INTEGER_KIND:
        valid = (unsigned int)type->data.integer_type.kind <=
            (unsigned int)CM_HIR_INT_USIZE;
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        valid = (unsigned int)type->data.float_type.kind <=
            (unsigned int)CM_HIR_FLOAT_F128;
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        valid = cm_hir_type_id_valid(context,
            type->data.reference_type.pointee)
            && cm_hir_region_valid(context,
                &type->data.reference_type.region)
            && (unsigned int)type->data.reference_type.mutability <=
                (unsigned int)CM_HIR_MUTABLE;
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        valid = cm_hir_type_id_valid(context,
            type->data.raw_pointer_type.pointee)
            && (unsigned int)type->data.raw_pointer_type.mutability <=
                (unsigned int)CM_HIR_MUTABLE;
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        valid = type->data.tuple_type.element_count == 0u
            || type->data.tuple_type.elements != NULL;
        for (index = 0u; valid
             && index < type->data.tuple_type.element_count; ++index) {
            valid = cm_hir_type_id_valid(context,
                type->data.tuple_type.elements[index]);
        }
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        valid = cm_hir_type_id_valid(context,
            type->data.array_type.element)
            && cm_hir_const_valid(context, &type->data.array_type.length);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        valid = cm_hir_type_id_valid(context,
            type->data.slice_type.element);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        valid = cm_hir_type_id_valid(context,
            type->data.fn_pointer_type.return_type)
            && (type->data.fn_pointer_type.parameter_count == 0u)
                == (type->data.fn_pointer_type.parameters == NULL)
            && cm_hir_lifetime_binder_valid(context,
                &type->data.fn_pointer_type.binder, type->span, 0)
            && cm_hir_intern_id_valid(context,
                type->data.fn_pointer_type.abi)
            && (unsigned int)type->data.fn_pointer_type.safety <=
                (unsigned int)CM_HIR_UNSAFE
            && (type->data.fn_pointer_type.is_variadic == 0
                || type->data.fn_pointer_type.is_variadic == 1);
        for (index = 0u; valid
             && index < type->data.fn_pointer_type.parameter_count; ++index) {
            valid = cm_hir_type_id_valid(context,
                type->data.fn_pointer_type.parameters[index]);
        }
        break;
    case CM_HIR_TYPE_CLOSURE_KIND:
        valid = cm_hir_get_closure(context,
            type->data.closure_type.closure) != NULL;
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        valid = cm_hir_named_type_valid(context, &type->data.named_type);
        break;
    case CM_HIR_TYPE_SELF_KIND:
    {
        const CmHirDefinition *definition;
        const CmHirItem *owner;

        definition = cm_hir_lookup_definition(context,
            type->data.self_type.owner);
        if (definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ITEM
            && definition->state == CM_HIR_DEFINITION_RESERVED
            && definition->has_reserved_item_kind
            && (definition->reserved_item_kind == CM_HIR_ITEM_TRAIT
                || definition->reserved_item_kind == CM_HIR_ITEM_IMPL
                || definition->reserved_item_kind
                    == CM_HIR_ITEM_TRAIT_ALIAS)) {
            valid = 1;
            break;
        }
        owner = cm_hir_bound_definition_item(context,
            type->data.self_type.owner);
        valid = owner != NULL && (owner->kind == CM_HIR_ITEM_TRAIT
            || owner->kind == CM_HIR_ITEM_IMPL
            || owner->kind == CM_HIR_ITEM_TRAIT_ALIAS);
        break;
    }
    case CM_HIR_TYPE_PARAMETER_KIND:
    {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            type->data.parameter_type.parameter);
        valid = parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE;
        break;
    }
    case CM_HIR_TYPE_PROJECTION_KIND:
        valid = cm_hir_projection_valid(context, type);
        break;
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        valid = cm_hir_dyn_trait_valid(context, type);
        break;
    default:
        valid = 0;
        break;
    }
    if (!valid) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_type_late_bound_requirement(context, type,
            &late_bound_requirement)) {
        return CM_HIR_INVALID_ID;
    }
    copy = *type;
    copy.late_bound_requirement = late_bound_requirement;
    switch (copy.kind) {
    case CM_HIR_TYPE_TUPLE_KIND:
        copy.data.tuple_type.elements = (CmHirTypeId *)cm_hir_copy_array(
            context, type->data.tuple_type.elements,
            type->data.tuple_type.element_count, sizeof(CmHirTypeId));
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        copy.data.fn_pointer_type.parameters =
            (CmHirTypeId *)cm_hir_copy_array(context,
                type->data.fn_pointer_type.parameters,
                type->data.fn_pointer_type.parameter_count,
                sizeof(CmHirTypeId));
        copy.data.fn_pointer_type.binder.lifetimes =
            (CmInternId *)cm_hir_copy_array(context,
                type->data.fn_pointer_type.binder.lifetimes,
                type->data.fn_pointer_type.binder.lifetime_count,
                sizeof(CmInternId));
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        copy.data.named_type = cm_hir_copy_named_type(context,
            &type->data.named_type);
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        copy.data.projection_type.trait_type = cm_hir_copy_named_type(
            context, &type->data.projection_type.trait_type);
        copy.data.projection_type.associated_type = cm_hir_copy_named_type(
            context, &type->data.projection_type.associated_type);
        break;
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if (type->data.dyn_trait_type.has_principal) {
            copy.data.dyn_trait_type.principal_trait = cm_hir_copy_named_type(
                context, &type->data.dyn_trait_type.principal_trait);
        }
        copy.data.dyn_trait_type.equalities =
            (CmHirAssociatedTypeEquality *)cm_hir_copy_array(context,
                type->data.dyn_trait_type.equalities,
                type->data.dyn_trait_type.equality_count,
                sizeof(*type->data.dyn_trait_type.equalities));
        copy.data.dyn_trait_type.auto_traits = cm_hir_copy_named_types(
            context, type->data.dyn_trait_type.auto_traits,
            type->data.dyn_trait_type.auto_trait_count);
        break;
    default:
        break;
    }
    return cm_hir_push(context, &context->types, &copy, out_id);
}

static int cm_hir_visibility_valid(const CmHirContext *context,
    const CmHirVisibility *visibility)
{
    if ((unsigned int)visibility->kind >
        (unsigned int)CM_HIR_VIS_RESTRICTED) {
        return 0;
    }
    if (visibility->kind == CM_HIR_VIS_RESTRICTED) {
        const CmHirDefinition *definition;

        definition = cm_hir_lookup_definition(context,
            visibility->restriction);
        return definition != NULL
            && definition->kind == CM_HIR_DEFINITION_MODULE;
    }
    return cm_hir_def_id_is_none(visibility->restriction);
}

static int cm_hir_fields_valid(const CmHirContext *context,
    const CmHirField *fields, uint32_t count, CmHirAggregateForm form)
{
    uint32_t index;

    if ((unsigned int)form > (unsigned int)CM_HIR_AGGREGATE_NAMED
        || (form == CM_HIR_AGGREGATE_UNIT && count != 0u)
        || (count != 0u && fields == NULL)) {
        return 0;
    }
    for (index = 0u; index < count; ++index) {
        if (!cm_hir_type_id_valid(context, fields[index].type)
            || !cm_hir_visibility_valid(context,
                &fields[index].visibility)
            || (form == CM_HIR_AGGREGATE_NAMED
                && !cm_hir_intern_id_valid(context, fields[index].name))
            || (form != CM_HIR_AGGREGATE_NAMED
                && fields[index].name != CM_INTERN_ID_NONE)
            || !cm_hir_span_is_ordered(fields[index].span)) {
            return 0;
        }
    }
    return 1;
}

static CmHirStatus cm_hir_item_parent_status(const CmHirContext *context,
    const CmHirModule *module, const CmHirItem *item)
{
    const CmHirItem *parent;
    const CmHirItem *implemented_trait;

    if (item->is_specializable != 0 && item->is_specializable != 1) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    if (!cm_hir_def_id_has_valid_shape(item->parent_definition)) {
        return CM_HIR_INVALID_ID;
    }
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return item->is_specializable
            ? CM_HIR_INVARIANT_VIOLATION : CM_HIR_OK;
    }
    if (item->parent_definition.crate_id != module->crate_id
        || cm_hir_lookup_definition(context,
            item->parent_definition) == NULL) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_def_id_is_none(item->definition)
        && cm_hir_def_id_equal(item->definition,
            item->parent_definition)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    parent = cm_hir_bound_definition_item(context,
        item->parent_definition);
    if (parent == NULL
        || (parent->kind != CM_HIR_ITEM_TRAIT
            && parent->kind != CM_HIR_ITEM_IMPL)
        || parent->owner_module != item->owner_module) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if ((parent->kind == CM_HIR_ITEM_TRAIT
            && parent->data.trait_item.is_auto)
        || (parent->kind == CM_HIR_ITEM_IMPL
            && parent->data.impl_item.polarity == CM_HIR_IMPL_NEGATIVE)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (item->kind != CM_HIR_ITEM_FUNCTION
        && item->kind != CM_HIR_ITEM_TYPE_ALIAS
        && item->kind != CM_HIR_ITEM_CONST) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (!item->is_specializable) return CM_HIR_OK;
    if (parent->kind != CM_HIR_ITEM_IMPL
        || !parent->data.impl_item.has_trait
        || parent->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    implemented_trait = cm_hir_bound_definition_item(context,
        parent->data.impl_item.trait_type.definition);
    if (implemented_trait == NULL
        || implemented_trait->kind != CM_HIR_ITEM_TRAIT
        || implemented_trait->data.trait_item.is_auto) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    return CM_HIR_OK;
}

static int cm_hir_type_self_owner_valid(const CmHirContext *context,
    CmHirTypeId type_id, CmHirDefId expected_owner, size_t depth);
static int cm_hir_named_self_owner_valid(const CmHirContext *context,
    const CmHirNamedType *named, CmHirDefId expected_owner, size_t depth);
static int cm_hir_type_late_bound_free(const CmHirContext *context,
    CmHirTypeId type_id, size_t depth);

static int cm_hir_argument_late_bound_free(const CmHirContext *context,
    const CmHirGenericArg *argument, size_t depth)
{
    if (argument->kind == CM_HIR_GENERIC_ARG_LIFETIME) {
        return argument->data.lifetime.kind != CM_HIR_REGION_LATE_BOUND;
    }
    if (argument->kind == CM_HIR_GENERIC_ARG_TYPE) {
        return cm_hir_type_late_bound_free(context, argument->data.type,
            depth + 1u);
    }
    return argument->kind == CM_HIR_GENERIC_ARG_CONST
        && cm_hir_type_late_bound_free(context,
            argument->data.constant.type, depth + 1u);
}

static int cm_hir_named_late_bound_free(const CmHirContext *context,
    const CmHirNamedType *named, size_t depth)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_argument_late_bound_free(context,
                &named->arguments[index], depth)) return 0;
    }
    return 1;
}

static int cm_hir_type_late_bound_free(const CmHirContext *context,
    CmHirTypeId type_id, size_t depth)
{
    const CmHirType *type;

    (void)depth;
    type = cm_hir_get_type(context, type_id);
    return type != NULL && type->late_bound_requirement == 0u;
}

static int cm_hir_impl_effective_polarity(const CmHirItem *item,
    CmHirImplPolarity *out_polarity)
{
    if (item == NULL || out_polarity == NULL
        || (unsigned int)item->data.impl_item.polarity
            > (unsigned int)CM_HIR_IMPL_RESERVATION) return 0;
    *out_polarity = item->data.impl_item.polarity;
    return 1;
}

static int cm_hir_impl_item_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    const CmHirItem *trait_item;
    CmHirImplPolarity polarity;

    if (!cm_hir_type_id_valid(context, item->data.impl_item.self_type)
        || !cm_hir_type_late_bound_free(context,
            item->data.impl_item.self_type, 0u)
        || !cm_hir_type_self_owner_valid(context,
            item->data.impl_item.self_type, item->definition, 0u)
        || (item->data.impl_item.has_trait != 0
            && item->data.impl_item.has_trait != 1)
        || !cm_hir_impl_effective_polarity(item, &polarity)
        || (item->data.impl_item.is_const != 0
            && item->data.impl_item.is_const != 1)
        || (unsigned int)item->data.impl_item.safety
            > (unsigned int)CM_HIR_UNSAFE) {
        return 0;
    }
    if (!item->data.impl_item.has_trait) {
        return polarity == CM_HIR_IMPL_POSITIVE
            && !item->data.impl_item.is_const
            && item->data.impl_item.safety == CM_HIR_SAFE
            && cm_hir_def_id_is_none(
                item->data.impl_item.trait_type.definition)
            && item->data.impl_item.trait_type.arguments == NULL
            && item->data.impl_item.trait_type.argument_count == 0u;
    }
    if (!cm_hir_named_type_valid(context,
            &item->data.impl_item.trait_type)
        || !cm_hir_named_late_bound_free(context,
            &item->data.impl_item.trait_type, 0u)
        || !cm_hir_named_self_owner_valid(context,
            &item->data.impl_item.trait_type, item->definition, 0u)) {
        return 0;
    }
    trait_item = cm_hir_bound_definition_item(context,
        item->data.impl_item.trait_type.definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || !cm_hir_named_type_matches_item_parameters(context,
            &item->data.impl_item.trait_type, trait_item)
        || (item->data.impl_item.is_const
            && !trait_item->data.trait_item.is_const)) {
        return 0;
    }
    if (polarity == CM_HIR_IMPL_NEGATIVE) {
        /* Negative impls are authenticated by their resolved trait identity;
         * auto-ness is relevant to the auto-trait solver, not to HIR
         * declaration validity.  The lowerer already enforces safe,
         * itemless negative headers. */
        return item->data.impl_item.safety == CM_HIR_SAFE;
    }
    return trait_item->data.trait_item.safety
        == item->data.impl_item.safety;
}

static int cm_hir_impl_alias_link_is_unique(const CmHirContext *context,
    const CmHirItem *item)
{
    size_t index;

    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *existing;

        existing = (const CmHirItem *)cm_vec_at_const(&context->items,
            index);
        if (existing != NULL && existing->kind == CM_HIR_ITEM_TYPE_ALIAS
            && cm_hir_def_id_equal(existing->parent_definition,
                item->parent_definition)
            && cm_hir_def_id_equal(
                existing->data.type_alias_item.trait_item_definition,
                item->data.type_alias_item.trait_item_definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_impl_function_link_is_unique(const CmHirContext *context,
    const CmHirItem *item)
{
    size_t index;

    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *existing;

        existing = (const CmHirItem *)cm_vec_at_const(&context->items,
            index);
        if (existing != NULL && existing->kind == CM_HIR_ITEM_FUNCTION
            && cm_hir_def_id_equal(existing->parent_definition,
                item->parent_definition)
            && cm_hir_def_id_equal(existing->data.function_item
                    .trait_item_definition,
                item->data.function_item.trait_item_definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_impl_value_link_is_unique(const CmHirContext *context,
    const CmHirItem *item)
{
    size_t index;

    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *existing;

        existing = (const CmHirItem *)cm_vec_at_const(&context->items,
            index);
        if (existing != NULL && existing->kind == CM_HIR_ITEM_CONST
            && cm_hir_def_id_equal(existing->parent_definition,
                item->parent_definition)
            && cm_hir_def_id_equal(existing->data.value_item
                    .trait_item_definition,
                item->data.value_item.trait_item_definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_generic_arg_self_owner_valid(const CmHirContext *context,
    const CmHirGenericArg *argument, CmHirDefId expected_owner, size_t depth)
{
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return 1;
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_hir_type_self_owner_valid(context, argument->data.type,
            expected_owner, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_type_self_owner_valid(context,
            argument->data.constant.type, expected_owner, depth + 1u);
    }
    return 0;
}

static int cm_hir_named_self_owner_valid(const CmHirContext *context,
    const CmHirNamedType *named, CmHirDefId expected_owner, size_t depth)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_generic_arg_self_owner_valid(context,
                &named->arguments[index], expected_owner, depth)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_type_self_owner_valid(const CmHirContext *context,
    CmHirTypeId type_id, CmHirDefId expected_owner, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (depth > context->types.len) return 0;
    type = cm_hir_get_type(context, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
        return 1;
    case CM_HIR_TYPE_SELF_KIND:
        return !cm_hir_def_id_is_none(expected_owner)
            && cm_hir_def_id_equal(type->data.self_type.owner,
                expected_owner);
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_hir_type_self_owner_valid(context,
            type->data.reference_type.pointee, expected_owner, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_hir_type_self_owner_valid(context,
            type->data.raw_pointer_type.pointee, expected_owner, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_hir_type_self_owner_valid(context,
                    type->data.tuple_type.elements[index], expected_owner,
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_hir_type_self_owner_valid(context,
                type->data.array_type.element, expected_owner, depth + 1u)
            && cm_hir_type_self_owner_valid(context,
                type->data.array_type.length.type, expected_owner,
                depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_type_self_owner_valid(context,
            type->data.slice_type.element, expected_owner, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_hir_type_self_owner_valid(context,
                    type->data.fn_pointer_type.parameters[index],
                    expected_owner, depth + 1u)) {
                return 0;
            }
        }
        return cm_hir_type_self_owner_valid(context,
            type->data.fn_pointer_type.return_type, expected_owner,
            depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_hir_named_self_owner_valid(context, &type->data.named_type,
            expected_owner, depth);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return cm_hir_get_closure(context,
            type->data.closure_type.closure) != NULL;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_hir_type_self_owner_valid(context,
                type->data.projection_type.self_type, expected_owner,
                depth + 1u)
            && cm_hir_named_self_owner_valid(context,
                &type->data.projection_type.trait_type, expected_owner,
                depth)
            && cm_hir_named_self_owner_valid(context,
                &type->data.projection_type.associated_type, expected_owner,
                depth);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if (type->data.dyn_trait_type.has_principal
            && !cm_hir_named_self_owner_valid(context,
                &type->data.dyn_trait_type.principal_trait,
                expected_owner, depth)) return 0;
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!cm_hir_named_self_owner_valid(context,
                    &type->data.dyn_trait_type.auto_traits[index],
                    expected_owner, depth)) return 0;
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index) {
            if (!cm_hir_type_self_owner_valid(context,
                    type->data.dyn_trait_type.equalities[index].value,
                    expected_owner, depth + 1u)) return 0;
        }
        return 1;
    }
    return 0;
}

static int cm_hir_body_self_roots_valid(const CmHirContext *context,
    CmHirBodyId body_id, CmHirDefId expected_owner)
{
    const CmHirBody *body;
    uint32_t index;

    if (body_id == CM_HIR_BODY_NONE) return 1;
    body = cm_hir_get_body(context, body_id);
    if (body == NULL
        || !cm_hir_type_self_owner_valid(context, body->expected_type,
            expected_owner, 0u)
        || !cm_hir_type_late_bound_free(context, body->expected_type, 0u)) {
        return 0;
    }
    for (index = 0u; index < body->local_count; ++index) {
        if (!cm_hir_type_self_owner_valid(context, body->locals[index].type,
                expected_owner, 0u)
            || !cm_hir_type_late_bound_free(context,
                body->locals[index].type, 0u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_fields_self_roots_valid(const CmHirContext *context,
    const CmHirField *fields, uint32_t field_count,
    CmHirDefId expected_owner)
{
    uint32_t index;

    for (index = 0u; index < field_count; ++index) {
        if (!cm_hir_type_self_owner_valid(context, fields[index].type,
                expected_owner, 0u)
            || !cm_hir_type_late_bound_free(context,
                fields[index].type, 0u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_parameter_local_type_matches(
    const CmHirContext *context, const CmHirFunctionParameter *parameter,
    const CmHirLocal *local)
{
    const CmHirType *local_type;
    const CmHirType *parameter_type;

    if (parameter->binding_mode == CM_HIR_PARAMETER_BINDING_MOVE) {
        return local->type == parameter->type;
    }
    if (parameter->binding_mode
            == CM_HIR_PARAMETER_BINDING_DEREF_SHARED) {
        parameter_type = cm_hir_get_type(context, parameter->type);
        return parameter_type != NULL
            && parameter_type->kind == CM_HIR_TYPE_REFERENCE_KIND
            && parameter_type->data.reference_type.mutability
                == CM_HIR_IMMUTABLE
            && local->type == parameter_type->data.reference_type.pointee
            && local->mutability == CM_HIR_IMMUTABLE;
    }
    local_type = cm_hir_get_type(context, local->type);
    return (parameter->binding_mode == CM_HIR_PARAMETER_BINDING_REF_SHARED
            || parameter->binding_mode
                == CM_HIR_PARAMETER_BINDING_REF_MUTABLE)
        && local_type != NULL
        && local_type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && local_type->data.reference_type.region.kind == CM_HIR_REGION_INFER
        && local_type->data.reference_type.pointee == parameter->type
        && local_type->data.reference_type.mutability
            == (parameter->binding_mode
                    == CM_HIR_PARAMETER_BINDING_REF_MUTABLE
                ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE)
        && local->mutability == CM_HIR_IMMUTABLE;
}

static int cm_hir_parameter_tuple_payload_empty(
    const CmHirFunctionParameter *parameter)
{
    uint32_t index;

    for (index = 0u; index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT;
         ++index) {
        if (parameter->tuple_bindings[index].name != CM_INTERN_ID_NONE
            || parameter->tuple_bindings[index].span.source != 0u
            || parameter->tuple_bindings[index].span.start != 0u
            || parameter->tuple_bindings[index].span.end != 0u) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_parameter_newtype_payload_empty(
    const CmHirFunctionParameter *parameter)
{
    return parameter->newtype_binding.name == CM_INTERN_ID_NONE
        && parameter->newtype_binding.span.source == 0u
        && parameter->newtype_binding.span.start == 0u
        && parameter->newtype_binding.span.end == 0u;
}

static int cm_hir_parameter_pattern_payload_empty(
    const CmHirFunctionParameter *parameter)
{
    return cm_hir_parameter_tuple_payload_empty(parameter)
        && cm_hir_parameter_newtype_payload_empty(parameter);
}

static int cm_hir_parameter_tuple_payload_valid(
    const CmHirContext *context, const CmHirFunctionParameter *parameter)
{
    const CmHirType *tuple_type;
    uint32_t binding_count;
    uint32_t index;
    uint32_t other_index;

    if (parameter->name != CM_INTERN_ID_NONE
        || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
        || !cm_hir_parameter_newtype_payload_empty(parameter)) {
        return 0;
    }
    tuple_type = cm_hir_get_type(context, parameter->type);
    if (tuple_type == NULL || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
        || tuple_type->data.tuple_type.element_count == 0u
        || tuple_type->data.tuple_type.element_count
            > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
        || tuple_type->data.tuple_type.elements == NULL) {
        return 0;
    }
    binding_count = tuple_type->data.tuple_type.element_count;
    for (index = 0u; index < binding_count; ++index) {
        const CmHirTupleParameterBinding *binding;

        binding = &parameter->tuple_bindings[index];
        if (!cm_hir_intern_id_nonempty(context, binding->name)
            || cm_hir_intern_matches(context, binding->name, "_")
            || !cm_hir_span_is_ordered(binding->span)
            || binding->span.source != parameter->span.source
            || binding->span.start < parameter->span.start
            || binding->span.end > parameter->span.end) {
            return 0;
        }
        for (other_index = 0u; other_index < index; ++other_index) {
            if (binding->name
                == parameter->tuple_bindings[other_index].name) {
                return 0;
            }
        }
    }
    for (; index < CM_HIR_TUPLE_PARAMETER_BINDING_COUNT; ++index) {
        const CmHirTupleParameterBinding *binding;

        binding = &parameter->tuple_bindings[index];
        if (binding->name != CM_INTERN_ID_NONE
            || binding->span.source != 0u
            || binding->span.start != 0u
            || binding->span.end != 0u) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_parameter_newtype_payload_valid(
    const CmHirContext *context, const CmHirFunctionParameter *parameter,
    CmHirTypeId *out_field_type)
{
    const CmHirType *parameter_type;
    const CmHirGenericArg *argument;
    const CmHirItem *aggregate;
    const CmHirGenericParam *generic;
    const CmHirType *declared_field_type;
    const CmHirNewtypeParameterBinding *binding;

    if (out_field_type != NULL) *out_field_type = CM_HIR_TYPE_NONE;
    if (parameter->name != CM_INTERN_ID_NONE
        || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
        || !cm_hir_parameter_tuple_payload_empty(parameter)) {
        return 0;
    }
    binding = &parameter->newtype_binding;
    if (!cm_hir_intern_id_nonempty(context, binding->name)
        || cm_hir_intern_matches(context, binding->name, "_")
        || !cm_hir_span_is_ordered(binding->span)
        || binding->span.source != parameter->span.source
        || binding->span.start < parameter->span.start
        || binding->span.end > parameter->span.end) {
        return 0;
    }
    parameter_type = cm_hir_get_type(context, parameter->type);
    if (parameter_type == NULL
        || parameter_type->kind != CM_HIR_TYPE_ADT_KIND
        || parameter_type->data.named_type.argument_count != 1u
        || parameter_type->data.named_type.arguments == NULL) {
        return 0;
    }
    argument = &parameter_type->data.named_type.arguments[0];
    aggregate = cm_hir_bound_definition_item(context,
        parameter_type->data.named_type.definition);
    if (argument->kind != CM_HIR_GENERIC_ARG_TYPE
        || aggregate == NULL || aggregate->kind != CM_HIR_ITEM_STRUCT
        || aggregate->data.aggregate_item.form != CM_HIR_AGGREGATE_TUPLE
        || aggregate->data.aggregate_item.field_count != 1u
        || aggregate->data.aggregate_item.fields == NULL
        || aggregate->generic_parameter_count != 1u
        || aggregate->generic_parameter_start
            == CM_HIR_GENERIC_PARAM_NONE) {
        return 0;
    }
    generic = cm_hir_get_generic_param(context,
        aggregate->generic_parameter_start);
    declared_field_type = cm_hir_get_type(context,
        aggregate->data.aggregate_item.fields[0].type);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || generic->index != 0u
        || !cm_hir_def_id_equal(generic->owner, aggregate->definition)
        || declared_field_type == NULL
        || declared_field_type->kind != CM_HIR_TYPE_PARAMETER_KIND
        || declared_field_type->data.parameter_type.parameter
            != aggregate->generic_parameter_start) {
        return 0;
    }
    if (out_field_type != NULL) *out_field_type = argument->data.type;
    return 1;
}

static int cm_hir_function_body_matches_signature(
    const CmHirContext *context, const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;
    const CmHirBody *body;
    uint32_t index;
    uint32_t local_index;

    if (item->data.function_item.body == CM_HIR_BODY_NONE) return 1;
    signature = &item->data.function_item.signature;
    body = cm_hir_get_body(context, item->data.function_item.body);
    if (body == NULL || body->expected_type != signature->return_type
        || body->parameter_count != signature->parameter_count) {
        return 0;
    }
    local_index = 0u;
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirFunctionParameter *parameter;

        parameter = &signature->parameters[index];
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            if (local_index >= body->local_count
                || body->locals[local_index].parameter_index != index
                || body->locals[local_index].parameter_binding_index != 0u
                || body->locals[local_index].name != parameter->name
                || !cm_hir_parameter_local_type_matches(context, parameter,
                    &body->locals[local_index])) {
                return 0;
            }
            local_index += 1u;
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_TUPLE_PATTERN) {
            const CmHirType *tuple_type;
            uint32_t binding_count;
            uint32_t binding_index;

            tuple_type = cm_hir_get_type(context, parameter->type);
            if (tuple_type == NULL
                || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND
                || tuple_type->data.tuple_type.element_count == 0u
                || tuple_type->data.tuple_type.element_count
                    > CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || tuple_type->data.tuple_type.elements == NULL) {
                return 0;
            }
            binding_count = tuple_type->data.tuple_type.element_count;
            for (binding_index = 0u;
                 binding_index < binding_count;
                 ++binding_index) {
                const CmHirLocal *local;
                const CmHirTupleParameterBinding *binding;

                if (local_index >= body->local_count) return 0;
                local = &body->locals[local_index];
                binding = &parameter->tuple_bindings[binding_index];
                if (local->parameter_index != index
                    || local->parameter_binding_index != binding_index
                    || local->name != binding->name
                    || local->type
                        != tuple_type->data.tuple_type.elements[binding_index]
                    || local->mutability != CM_HIR_IMMUTABLE
                    || local->span.source != binding->span.source
                    || local->span.start != binding->span.start
                    || local->span.end != binding->span.end) {
                    return 0;
                }
                local_index += 1u;
            }
            continue;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            const CmHirLocal *local;
            CmHirTypeId field_type;

            if (!cm_hir_parameter_newtype_payload_valid(context, parameter,
                    &field_type)
                || local_index >= body->local_count) {
                return 0;
            }
            local = &body->locals[local_index];
            if (local->parameter_index != index
                || local->parameter_binding_index != 0u
                || local->name != parameter->newtype_binding.name
                || local->type != field_type
                || local->mutability != CM_HIR_IMMUTABLE
                || local->span.source
                    != parameter->newtype_binding.span.source
                || local->span.start
                    != parameter->newtype_binding.span.start
                || local->span.end != parameter->newtype_binding.span.end) {
                return 0;
            }
            local_index += 1u;
            continue;
        }
        return 0;
    }
    return local_index == body->local_count
        || body->locals[local_index].parameter_index
            == CM_HIR_PARAMETER_INDEX_NONE;
}

static int cm_hir_tuple_parameter_placement_valid(
    const CmHirContext *context, const CmHirItem *item,
    uint32_t parameter_index)
{
    const CmHirFunctionSignature *signature;
    const CmHirFunctionParameter *parameter;
    const CmHirType *tuple_type;
    const CmHirItem *parent;

    signature = &item->data.function_item.signature;
    parameter = &signature->parameters[parameter_index];
    tuple_type = cm_hir_get_type(context, parameter->type);
    if (tuple_type == NULL || tuple_type->kind != CM_HIR_TYPE_TUPLE_KIND) {
        return 0;
    }
    if (tuple_type->data.tuple_type.element_count == 2u) {
        return cm_hir_def_id_is_none(item->parent_definition)
            && item->data.function_item.body != CM_HIR_BODY_NONE
            && signature->receiver == CM_HIR_RECEIVER_NONE;
    }
    if (tuple_type->data.tuple_type.element_count != 1u
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_intern_matches(context, signature->abi, "rust-call")
        || signature->receiver == CM_HIR_RECEIVER_NONE
        || signature->parameter_count != 2u
        || parameter_index != 1u) {
        return 0;
    }
    parent = cm_hir_bound_definition_item(context,
        item->parent_definition);
    return parent != NULL && parent->kind == CM_HIR_ITEM_IMPL;
}

static int cm_hir_receiver_shape_valid(const CmHirContext *context,
    const CmHirFunctionSignature *signature, CmHirDefId expected_owner)
{
    const CmHirFunctionParameter *parameter;
    const CmHirType *type;
    const CmHirType *pointee;

    if (signature->receiver == CM_HIR_RECEIVER_NONE) return 1;
    if (signature->parameter_count == 0u) return 0;
    parameter = &signature->parameters[0];
    if (parameter->binding_kind != CM_HIR_BINDING_NAMED
        || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
        || !cm_hir_intern_matches(context, parameter->name, "self")) {
        return 0;
    }
    if (signature->receiver == CM_HIR_RECEIVER_CUSTOM) {
        return cm_hir_custom_receiver_type_valid(context, parameter->type,
            expected_owner);
    }
    type = cm_hir_get_type(context, parameter->type);
    if (type == NULL) return 0;
    if (signature->receiver == CM_HIR_RECEIVER_VALUE) {
        return type->kind == CM_HIR_TYPE_SELF_KIND
            && cm_hir_def_id_equal(type->data.self_type.owner,
                expected_owner);
    }
    if (type->kind != CM_HIR_TYPE_REFERENCE_KIND) return 0;
    pointee = cm_hir_get_type(context, type->data.reference_type.pointee);
    if (pointee == NULL || pointee->kind != CM_HIR_TYPE_SELF_KIND
        || !cm_hir_def_id_equal(pointee->data.self_type.owner,
            expected_owner)) {
        return 0;
    }
    return type->data.reference_type.mutability
        == (signature->receiver == CM_HIR_RECEIVER_REF_MUTABLE
            ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE);
}

static int cm_hir_custom_receiver_type_valid_inner(
    const CmHirContext *context, CmHirTypeId type_id,
    CmHirDefId expected_owner, size_t depth)
{
    const CmHirType *type;
    const CmHirNamedType *named;
    const CmHirGenericArg *argument;

    if (context == NULL || cm_hir_def_id_is_none(expected_owner)
        || depth > context->types.len) {
        return 0;
    }
    type = cm_hir_get_type(context, type_id);
    if (type == NULL) return 0;
    if (type->kind == CM_HIR_TYPE_SELF_KIND) {
        return cm_hir_def_id_equal(type->data.self_type.owner,
            expected_owner);
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        return cm_hir_custom_receiver_type_valid_inner(context,
            type->data.reference_type.pointee, expected_owner, depth + 1u);
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND
        && type->kind != CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
        return 0;
    }
    named = &type->data.named_type;
    if (named->argument_count != 1u || named->arguments == NULL) return 0;
    argument = &named->arguments[0];
    return argument->kind == CM_HIR_GENERIC_ARG_TYPE
        && cm_hir_custom_receiver_type_valid_inner(context,
            argument->data.type, expected_owner, depth + 1u);
}

int cm_hir_custom_receiver_type_valid(const CmHirContext *context,
    CmHirTypeId type, CmHirDefId expected_owner)
{
    return cm_hir_custom_receiver_type_valid_inner(context, type,
        expected_owner, 0u);
}

static int cm_hir_function_self_roots_valid(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId expected_owner)
{
    const CmHirFunctionSignature *signature;
    uint32_t index;

    signature = &item->data.function_item.signature;
    for (index = 0u; index < signature->parameter_count; ++index) {
        if (!cm_hir_type_self_owner_valid(context,
                signature->parameters[index].type, expected_owner, 0u)
            || !cm_hir_type_late_bound_free(context,
                signature->parameters[index].type, 0u)) {
            return 0;
        }
    }
    if (!cm_hir_type_self_owner_valid(context, signature->return_type,
            expected_owner, 0u)
        || !cm_hir_type_late_bound_free(context,
            signature->return_type, 0u)) {
        return 0;
    }
    return cm_hir_body_self_roots_valid(context,
        item->data.function_item.body, expected_owner);
}

static int cm_hir_function_item_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    const CmHirFunctionSignature *signature;
    const CmHirItem *parent;
    const CmHirItem *trait_declaration;
    uint32_t index;

    signature = &item->data.function_item.signature;
    if (!cm_hir_type_id_valid(context, signature->return_type)
        || !cm_hir_intern_id_valid(context, signature->abi)
        || (signature->parameter_count != 0u
            && signature->parameters == NULL)
        || (unsigned int)signature->receiver >
            (unsigned int)CM_HIR_RECEIVER_CUSTOM
        || (signature->receiver != CM_HIR_RECEIVER_NONE
            && signature->parameter_count == 0u)
        || (unsigned int)signature->safety > (unsigned int)CM_HIR_UNSAFE
        || (signature->is_const != 0 && signature->is_const != 1)
        || (signature->is_async != 0 && signature->is_async != 1)
        || (signature->is_variadic != 0 && signature->is_variadic != 1)
        || (item->data.function_item.has_default_body != 0
            && item->data.function_item.has_default_body != 1)
        || (item->data.function_item.body != CM_HIR_BODY_NONE
            && cm_hir_get_body(context,
                item->data.function_item.body) == NULL)) {
        return 0;
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirFunctionParameter *parameter;
        int binding_valid;

        parameter = &signature->parameters[index];
        if (parameter->binding_kind == CM_HIR_BINDING_NAMED) {
            binding_valid = cm_hir_intern_id_nonempty(context,
                    parameter->name)
                && !cm_hir_intern_matches(context, parameter->name, "_")
                && cm_hir_parameter_pattern_payload_empty(parameter);
        } else if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) {
            binding_valid = parameter->name == CM_INTERN_ID_NONE
                && parameter->binding_mode == CM_HIR_PARAMETER_BINDING_MOVE
                && cm_hir_parameter_pattern_payload_empty(parameter);
        } else if (parameter->binding_kind
                == CM_HIR_BINDING_TUPLE_PATTERN) {
            binding_valid = cm_hir_parameter_tuple_payload_valid(context,
                    parameter)
                && cm_hir_tuple_parameter_placement_valid(context, item,
                    index);
        } else if (parameter->binding_kind
                == CM_HIR_BINDING_NEWTYPE_PATTERN) {
            binding_valid = cm_hir_parameter_newtype_payload_valid(context,
                    parameter, NULL)
                && item->data.function_item.body != CM_HIR_BODY_NONE;
        } else {
            binding_valid = 0;
        }
        if (!binding_valid
            || (unsigned int)parameter->binding_mode
                > (unsigned int)CM_HIR_PARAMETER_BINDING_DEREF_SHARED
            || !cm_hir_type_id_valid(context, parameter->type)
            || !cm_hir_span_is_ordered(parameter->span)) {
            return 0;
        }
    }
    if (!cm_hir_function_body_matches_signature(context, item)) return 0;
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return item->data.function_item.has_default_body == 0
            && signature->receiver == CM_HIR_RECEIVER_NONE
            && cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            && cm_hir_function_self_roots_valid(context, item,
                cm_hir_def_id_none());
    }
    parent = cm_hir_bound_definition_item(context,
        item->parent_definition);
    if (parent == NULL) return 0;
    if (parent->kind == CM_HIR_ITEM_TRAIT) {
        return cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            && (item->data.function_item.body == CM_HIR_BODY_NONE
                || item->data.function_item.has_default_body == 1)
            && cm_hir_receiver_shape_valid(context, signature,
                item->parent_definition)
            && cm_hir_function_self_roots_valid(context, item,
                item->parent_definition);
    }
    if (parent->kind != CM_HIR_ITEM_IMPL
        || parent->data.impl_item.polarity == CM_HIR_IMPL_NEGATIVE
        || item->data.function_item.has_default_body != 0
        || item->data.function_item.body == CM_HIR_BODY_NONE) {
        return 0;
    }
    if (!parent->data.impl_item.has_trait) {
        return cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            && cm_hir_receiver_shape_valid(context, signature,
                item->parent_definition)
            && cm_hir_function_self_roots_valid(context, item,
                item->parent_definition);
    }
    if (cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)) return 0;
    trait_declaration = cm_hir_bound_definition_item(context,
        item->data.function_item.trait_item_definition);
    return trait_declaration != NULL
        && trait_declaration->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_is_none(trait_declaration->data.function_item
                .trait_item_definition)
        && cm_hir_def_id_equal(trait_declaration->parent_definition,
            parent->data.impl_item.trait_type.definition)
        && trait_declaration->name == item->name
        && trait_declaration->generic_parameter_count
            == item->generic_parameter_count
        && trait_declaration->data.function_item.signature.abi
            == item->data.function_item.signature.abi
        && trait_declaration->data.function_item.signature.is_async
            == item->data.function_item.signature.is_async
        && cm_hir_impl_function_link_is_unique(context, item)
        && cm_hir_receiver_shape_valid(context, signature,
            item->parent_definition)
        && cm_hir_function_self_roots_valid(context, item,
            item->parent_definition);
}

static const CmHirItem *cm_hir_bound_item_or_candidate(
    const CmHirContext *context, CmHirDefId definition,
    const CmHirItem *candidate, CmHirDefId candidate_definition)
{
    if (candidate != NULL
        && cm_hir_def_id_equal(definition, candidate_definition)) {
        return candidate;
    }
    return cm_hir_bound_definition_item(context, definition);
}

static CmHirDefId cm_hir_item_self_owner(const CmHirItem *item,
    CmHirDefId actual_definition)
{
    if (!cm_hir_def_id_is_none(item->parent_definition)) {
        return item->parent_definition;
    }
    if (item->kind == CM_HIR_ITEM_TRAIT || item->kind == CM_HIR_ITEM_IMPL
        || item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
        return actual_definition;
    }
    return cm_hir_def_id_none();
}

static int cm_hir_predicate_argument_in_scope(const CmHirContext *context,
    const CmHirGenericArg *argument, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth);
static int cm_hir_predicate_type_in_scope(const CmHirContext *context,
    CmHirTypeId type_id, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth);

static int cm_hir_predicate_parameter_in_scope(
    const CmHirContext *context, CmHirGenericParamId referenced,
    CmHirDefId owner_definition, CmHirDefId parent_definition)
{
    const CmHirGenericParam *parameter;

    parameter = cm_hir_get_generic_param(context, referenced);
    return parameter != NULL
        && (cm_hir_def_id_equal(parameter->owner, owner_definition)
            || (!cm_hir_def_id_is_none(parent_definition)
                && cm_hir_def_id_equal(parameter->owner,
                    parent_definition)));
}

static int cm_hir_predicate_region_in_scope(const CmHirContext *context,
    const CmHirRegion *region, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder)
{
    if (region->kind == CM_HIR_REGION_EARLY_BOUND) {
        return cm_hir_predicate_parameter_in_scope(context,
            region->data.parameter, owner_definition, parent_definition);
    }
    if (region->kind == CM_HIR_REGION_LATE_BOUND) {
        return binder != NULL
            && region->data.binder_index < binder->lifetime_count;
    }
    return 1;
}

static int cm_hir_predicate_const_in_scope(const CmHirContext *context,
    const CmHirConstArg *constant, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth)
{
    if (!cm_hir_predicate_type_in_scope(context, constant->type,
            owner_definition, parent_definition, binder, depth + 1u)) {
        return 0;
    }
    return constant->kind != CM_HIR_CONST_PARAMETER
        || cm_hir_predicate_parameter_in_scope(context,
            constant->data.parameter, owner_definition, parent_definition);
}

static int cm_hir_predicate_named_in_scope(const CmHirContext *context,
    const CmHirNamedType *named, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_predicate_argument_in_scope(context,
                &named->arguments[index], owner_definition,
                parent_definition, binder, depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_predicate_type_in_scope(const CmHirContext *context,
    CmHirTypeId type_id, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (depth > context->types.len) return 0;
    type = cm_hir_get_type(context, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_SELF_KIND:
        return 1;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_hir_predicate_region_in_scope(context,
                &type->data.reference_type.region, owner_definition,
                parent_definition, binder)
            && cm_hir_predicate_type_in_scope(context,
                type->data.reference_type.pointee, owner_definition,
                parent_definition, binder, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_hir_predicate_type_in_scope(context,
            type->data.raw_pointer_type.pointee, owner_definition,
            parent_definition, binder, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_hir_predicate_type_in_scope(context,
                    type->data.tuple_type.elements[index], owner_definition,
                    parent_definition, binder, depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_hir_predicate_type_in_scope(context,
                type->data.array_type.element, owner_definition,
                parent_definition, binder, depth + 1u)
            && cm_hir_predicate_const_in_scope(context,
                &type->data.array_type.length, owner_definition,
                parent_definition, binder, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_predicate_type_in_scope(context,
            type->data.slice_type.element, owner_definition,
            parent_definition, binder, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_hir_predicate_type_in_scope(context,
                    type->data.fn_pointer_type.parameters[index],
                    owner_definition, parent_definition,
                    &type->data.fn_pointer_type.binder,
                    depth + 1u)) {
                return 0;
            }
        }
        return cm_hir_predicate_type_in_scope(context,
            type->data.fn_pointer_type.return_type, owner_definition,
            parent_definition, &type->data.fn_pointer_type.binder,
            depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return cm_hir_predicate_named_in_scope(context,
            &type->data.named_type, owner_definition, parent_definition,
            binder, depth);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return 0;
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_hir_predicate_parameter_in_scope(context,
            type->data.parameter_type.parameter, owner_definition,
            parent_definition);
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_hir_predicate_type_in_scope(context,
                type->data.projection_type.self_type, owner_definition,
                parent_definition, binder, depth + 1u)
            && cm_hir_predicate_named_in_scope(context,
                &type->data.projection_type.trait_type, owner_definition,
                parent_definition, binder, depth)
            && cm_hir_predicate_named_in_scope(context,
                &type->data.projection_type.associated_type,
                owner_definition, parent_definition, binder, depth);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if ((type->data.dyn_trait_type.has_principal
                && !cm_hir_predicate_named_in_scope(context,
                    &type->data.dyn_trait_type.principal_trait,
                    owner_definition, parent_definition, binder, depth))
            || !cm_hir_predicate_region_in_scope(context,
                &type->data.dyn_trait_type.region, owner_definition,
                parent_definition, binder)) return 0;
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!cm_hir_predicate_named_in_scope(context,
                    &type->data.dyn_trait_type.auto_traits[index],
                    owner_definition, parent_definition, binder, depth)) {
                return 0;
            }
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index) {
            if (!cm_hir_predicate_type_in_scope(context,
                    type->data.dyn_trait_type.equalities[index].value,
                    owner_definition, parent_definition, binder,
                    depth + 1u)) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int cm_hir_predicate_argument_in_scope(const CmHirContext *context,
    const CmHirGenericArg *argument, CmHirDefId owner_definition,
    CmHirDefId parent_definition, const CmHirLifetimeBinder *binder,
    size_t depth)
{
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_hir_predicate_region_in_scope(context,
            &argument->data.lifetime, owner_definition, parent_definition,
            binder);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_hir_predicate_type_in_scope(context,
            argument->data.type, owner_definition, parent_definition,
            binder, depth + 1u);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_predicate_const_in_scope(context,
            &argument->data.constant, owner_definition, parent_definition,
            binder, depth + 1u);
    }
    return 0;
}

static int cm_hir_associated_equalities_valid(
    const CmHirContext *context, const CmHirItem *owner,
    CmHirDefId owner_definition, CmHirDefId trait_definition,
    const CmHirAssociatedTypeEquality *equalities, uint32_t equality_count,
    const CmHirItem *candidate, CmHirDefId candidate_definition,
    CmHirDefId self_owner, int allow_inherited);

static int cm_hir_lifetime_binder_valid(const CmHirContext *context,
    const CmHirLifetimeBinder *binder, CmSpan container_span,
    int require_nonempty)
{
    uint32_t binder_index;
    uint32_t prior_index;

    if (binder->lifetime_count > CM_HIR_LIFETIME_BINDER_LIMIT) return 0;
    if (binder->lifetime_count == 0u) {
        return !require_nonempty && binder->lifetimes == NULL
            && binder->span.source == 0u && binder->span.start == 0u
            && binder->span.end == 0u;
    }
    if (binder->lifetimes == NULL
        || binder->span.start >= binder->span.end
        || binder->span.source != container_span.source
        || binder->span.start < container_span.start
        || binder->span.end > container_span.end) {
        return 0;
    }
    for (binder_index = 0u; binder_index < binder->lifetime_count;
         ++binder_index) {
        const CmInternedString *name;

        name = cm_interner_get(&context->strings,
            binder->lifetimes[binder_index]);
        if (name == NULL || name->len == 0u) return 0;
        for (prior_index = 0u; prior_index < binder_index;
             ++prior_index) {
            if (binder->lifetimes[prior_index]
                    == binder->lifetimes[binder_index]) {
                return 0;
            }
        }
    }
    return 1;
}

static const CmHirPredicateScope *cm_hir_predicate_scope_get(
    const CmHirItem *item, CmHirPredicateScopeId scope)
{
    if (scope == CM_HIR_PREDICATE_SCOPE_NONE
        || scope > item->predicate_scope_count
        || item->predicate_scopes == NULL) {
        return NULL;
    }
    return &item->predicate_scopes[scope - 1u];
}

static int cm_hir_item_predicate_scopes_valid(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId owner_definition)
{
    CmHirDefId self_owner;
    uint32_t scope_index;

    if ((item->predicate_scope_count != 0u
            && item->predicate_scopes == NULL)
        || (item->predicate_scope_count == 0u
            && item->predicate_scopes != NULL)
        || (item->predicate_count != 0u && item->predicates == NULL)
        || (item->predicate_count == 0u && item->predicates != NULL)
        || (item->outlives_predicate_count != 0u
            && item->outlives_predicates == NULL)
        || (item->outlives_predicate_count == 0u
            && item->outlives_predicates != NULL)) {
        return 0;
    }
    self_owner = cm_hir_item_self_owner(item, owner_definition);
    for (scope_index = 0u; scope_index < item->predicate_scope_count;
         ++scope_index) {
        const CmHirPredicateScope *scope;
        uint32_t trait_count;
        uint32_t outlives_count;
        uint32_t index;

        scope = &item->predicate_scopes[scope_index];
        if ((unsigned int)scope->subject_kind
                > (unsigned int)CM_HIR_OUTLIVES_LIFETIME
            || !cm_hir_span_is_ordered(scope->span)
            || scope->span.source != item->span.source
            || scope->span.start < item->span.start
            || scope->span.end > item->span.end
            || !cm_hir_lifetime_binder_valid(context, &scope->binder,
                scope->span, 1)
            || (scope->trait_predicate_count == 0u
                && scope->outlives_predicate_count == 0u)) {
            return 0;
        }
        if (scope->subject_kind == CM_HIR_OUTLIVES_TYPE) {
            if (!cm_hir_type_id_valid(context, scope->subject.type)
                || !cm_hir_type_self_owner_valid(context,
                    scope->subject.type, self_owner, 0u)
                || !cm_hir_predicate_type_in_scope(context,
                    scope->subject.type, owner_definition,
                    item->parent_definition, &scope->binder, 0u)) {
                return 0;
            }
        } else if (!cm_hir_region_valid(context,
                &scope->subject.lifetime)
            || !cm_hir_predicate_region_in_scope(context,
                &scope->subject.lifetime, owner_definition,
                item->parent_definition, &scope->binder)) {
            return 0;
        }
        trait_count = 0u;
        for (index = 0u; index < item->predicate_count; ++index) {
            if (item->predicates[index].scope == scope_index + 1u) {
                trait_count += 1u;
            }
        }
        outlives_count = 0u;
        for (index = 0u; index < item->outlives_predicate_count; ++index) {
            if (item->outlives_predicates[index].scope == scope_index + 1u) {
                outlives_count += 1u;
            }
        }
        if (trait_count != scope->trait_predicate_count
            || outlives_count != scope->outlives_predicate_count) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_trait_predicate_valid(const CmHirContext *context,
    const CmHirItem *owner, const CmHirTraitPredicate *predicate,
    const CmHirItem *candidate, CmHirDefId candidate_definition,
    CmHirDefId owner_definition)
{
    const CmHirModule *module;
    const CmHirDefinition *target_definition;
    const CmHirItem *target;
    const CmHirLifetimeBinder *active_binder;
    const CmHirPredicateScope *scope;
    CmHirDefId self_owner;

    scope = cm_hir_predicate_scope_get(owner, predicate->scope);
    if (predicate->scope == CM_HIR_PREDICATE_SCOPE_NONE) {
        if (!cm_hir_lifetime_binder_valid(context, &predicate->binder,
                predicate->span, 0)) return 0;
        active_binder = predicate->binder.lifetime_count == 0u
            ? NULL : &predicate->binder;
    } else {
        if (scope == NULL || predicate->binder.lifetime_count != 0u
            || scope->subject_kind != CM_HIR_OUTLIVES_TYPE
            || scope->subject.type != predicate->subject
            || scope->span.source != predicate->span.source
            || scope->span.start != predicate->span.start
            || scope->span.end != predicate->span.end
            || !cm_hir_lifetime_binder_valid(context, &predicate->binder,
                predicate->span, 0)) {
            return 0;
        }
        active_binder = &scope->binder;
    }

    module = cm_hir_get_module(context, owner->owner_module);
    self_owner = cm_hir_item_self_owner(owner, owner_definition);
    if (module == NULL
        || (unsigned int)predicate->modifier
            > (unsigned int)CM_HIR_PREDICATE_CONST
        || !cm_hir_span_is_ordered(predicate->span)
        || !cm_hir_type_id_valid(context, predicate->subject)
        || !cm_hir_type_self_owner_valid(context, predicate->subject,
            self_owner, 0u)
        || !cm_hir_predicate_type_in_scope(context, predicate->subject,
            owner_definition, owner->parent_definition,
            predicate->scope == CM_HIR_PREDICATE_SCOPE_NONE
                ? NULL : active_binder, 0u)
        || !cm_hir_named_type_valid(context, &predicate->trait_type)
        || !cm_hir_named_self_owner_valid(context, &predicate->trait_type,
            self_owner, 0u)
        || !cm_hir_predicate_named_in_scope(context,
            &predicate->trait_type, owner_definition,
            owner->parent_definition, active_binder, 0u)) {
        return 0;
    }
    target_definition = cm_hir_lookup_definition(context,
        predicate->trait_type.definition);
    if (target_definition == NULL
        || target_definition->kind != CM_HIR_DEFINITION_ITEM) {
        return 0;
    }
    if (target_definition->state == CM_HIR_DEFINITION_RESERVED
        && target_definition->has_reserved_item_kind
        && target_definition->reserved_item_kind != CM_HIR_ITEM_TRAIT
        && target_definition->reserved_item_kind
            != CM_HIR_ITEM_TRAIT_ALIAS) {
        return 0;
    }
    target = cm_hir_bound_item_or_candidate(context,
        predicate->trait_type.definition, candidate, candidate_definition);
    if (target != NULL
        && ((target->kind != CM_HIR_ITEM_TRAIT
                && target->kind != CM_HIR_ITEM_TRAIT_ALIAS)
            || (target->kind == CM_HIR_ITEM_TRAIT_ALIAS
                && predicate->equality_count != 0u)
            || !cm_hir_named_type_matches_item_parameters(context,
                &predicate->trait_type, target))) {
        return 0;
    }
    if (!cm_hir_associated_equalities_valid(context, owner,
            owner_definition, predicate->trait_type.definition,
            predicate->equalities,
            predicate->equality_count, candidate, candidate_definition,
            self_owner, 1)) {
        return 0;
    }
    {
        uint32_t index;

        for (index = 0u; index < predicate->equality_count; ++index) {
            if (!cm_hir_predicate_type_in_scope(context,
                    predicate->equalities[index].value, owner_definition,
                    owner->parent_definition, active_binder, 0u)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_item_predicates_valid(const CmHirContext *context,
    const CmHirItem *item, const CmHirItem *candidate,
    CmHirDefId candidate_definition, CmHirDefId owner_definition)
{
    uint32_t index;

    if (!cm_hir_item_predicate_scopes_valid(context, item,
            owner_definition)
        || (item->predicate_count != 0u && item->predicates == NULL)
        || (item->predicate_count == 0u && item->predicates != NULL)) {
        return 0;
    }
    for (index = 0u; index < item->predicate_count; ++index) {
        if (!cm_hir_trait_predicate_valid(context, item,
                &item->predicates[index], candidate, candidate_definition,
                owner_definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_outlives_predicate_valid(const CmHirContext *context,
    const CmHirItem *owner, const CmHirOutlivesPredicate *predicate,
    CmHirDefId owner_definition)
{
    const CmHirLifetimeBinder *active_binder;
    const CmHirPredicateScope *scope;
    CmHirDefId self_owner;

    self_owner = cm_hir_item_self_owner(owner, owner_definition);
    scope = cm_hir_predicate_scope_get(owner, predicate->scope);
    active_binder = scope == NULL ? NULL : &scope->binder;
    if ((unsigned int)predicate->subject_kind
            > (unsigned int)CM_HIR_OUTLIVES_LIFETIME
        || (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
            && (scope == NULL
                || scope->subject_kind != predicate->subject_kind
                || scope->span.source != predicate->span.source
                || scope->span.start != predicate->span.start
                || scope->span.end != predicate->span.end))
        || !cm_hir_span_is_ordered(predicate->span)
        || !cm_hir_region_valid(context, &predicate->bound)
        || !cm_hir_predicate_region_in_scope(context, &predicate->bound,
            owner_definition, owner->parent_definition, active_binder)) {
        return 0;
    }
    if (predicate->subject_kind == CM_HIR_OUTLIVES_TYPE) {
        return (scope == NULL
                || scope->subject.type == predicate->subject.type)
            && cm_hir_type_id_valid(context, predicate->subject.type)
            && cm_hir_type_self_owner_valid(context,
                predicate->subject.type, self_owner, 0u)
            && cm_hir_predicate_type_in_scope(context,
                predicate->subject.type, owner_definition,
                owner->parent_definition, active_binder, 0u);
    }
    return (scope == NULL || cm_hir_region_equal(
                &scope->subject.lifetime, &predicate->subject.lifetime))
        && cm_hir_region_valid(context, &predicate->subject.lifetime)
        && cm_hir_predicate_region_in_scope(context,
            &predicate->subject.lifetime, owner_definition,
            owner->parent_definition, active_binder);
}

static int cm_hir_item_outlives_predicates_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId owner_definition)
{
    uint32_t index;

    if ((item->outlives_predicate_count != 0u
            && item->outlives_predicates == NULL)
        || (item->outlives_predicate_count == 0u
            && item->outlives_predicates != NULL)) {
        return 0;
    }
    for (index = 0u; index < item->outlives_predicate_count; ++index) {
        if (!cm_hir_outlives_predicate_valid(context, item,
                &item->outlives_predicates[index], owner_definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_associated_declaration_shape_valid(
    const CmHirItem *associated)
{
    return associated != NULL && associated->kind == CM_HIR_ITEM_TYPE_ALIAS
        && associated->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_is_none(associated->data.type_alias_item
                .trait_item_definition)
        && associated->generic_parameter_count == 0u;
}

static int cm_hir_trait_reaches_candidate(const CmHirContext *context,
    CmHirDefId start, CmHirDefId target, const CmHirItem *candidate,
    CmHirDefId candidate_definition, size_t depth)
{
    const CmHirItem *trait_item;
    uint32_t index;

    if (cm_hir_def_id_equal(start, target)) return 1;
    if (depth > context->items.len) return 0;
    trait_item = cm_hir_bound_item_or_candidate(context, start, candidate,
        candidate_definition);
    if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
        return 0;
    }
    for (index = 0u; index < trait_item->data.trait_item.supertrait_count;
         ++index) {
        if (cm_hir_trait_reaches_candidate(context,
                trait_item->data.trait_item.supertraits[index]
                    .trait_type.definition,
                target, candidate, candidate_definition, depth + 1u)) {
            return 1;
        }
    }
    return 0;
}

static int cm_hir_associated_name_unambiguous(
    const CmHirContext *context, const CmHirItem *associated,
    CmHirDefId trait_definition, const CmHirItem *candidate,
    CmHirDefId candidate_definition)
{
    size_t index;
    uint32_t matches;

    matches = 0u;
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (cm_hir_associated_declaration_shape_valid(item)
            && item->name == associated->name
            && cm_hir_trait_reaches_candidate(context, trait_definition,
                item->parent_definition, candidate, candidate_definition,
                0u)) {
            ++matches;
        }
    }
    if (candidate != NULL
        && cm_hir_associated_declaration_shape_valid(candidate)
        && candidate->name == associated->name
        && cm_hir_trait_reaches_candidate(context, trait_definition,
            candidate->parent_definition, candidate, candidate_definition,
            0u)) {
        ++matches;
    }
    return matches == 1u;
}

static int cm_hir_associated_equalities_valid(
    const CmHirContext *context, const CmHirItem *owner,
    CmHirDefId owner_definition, CmHirDefId trait_definition,
    const CmHirAssociatedTypeEquality *equalities, uint32_t equality_count,
    const CmHirItem *candidate, CmHirDefId candidate_definition,
    CmHirDefId self_owner, int allow_inherited)
{
    const CmHirModule *module;
    const CmHirItem *resolved_trait;
    uint32_t index;

    module = cm_hir_get_module(context, owner->owner_module);
    resolved_trait = cm_hir_bound_item_or_candidate(context,
        trait_definition, candidate, candidate_definition);
    if (module == NULL
        || (equality_count != 0u && equalities == NULL)
        || (equality_count == 0u && equalities != NULL)) {
        return 0;
    }
    for (index = 0u; index < equality_count; ++index) {
        const CmHirAssociatedTypeEquality *equality;
        const CmHirDefinition *associated_definition;
        const CmHirItem *associated;
        uint32_t prior;

        equality = &equalities[index];
        associated_definition = cm_hir_lookup_definition(context,
            equality->associated_type);
        if (associated_definition == NULL
            || associated_definition->kind != CM_HIR_DEFINITION_ITEM
            || equality->associated_type.crate_id != module->crate_id
            || !cm_hir_type_id_valid(context, equality->value)
            || !cm_hir_type_self_owner_valid(context, equality->value,
                self_owner, 0u)
            || !cm_hir_span_is_ordered(equality->span)
            || (!cm_hir_def_id_is_none(owner_definition)
                && cm_hir_def_id_equal(equality->associated_type,
                    owner_definition))) {
            return 0;
        }
        associated = cm_hir_bound_item_or_candidate(context,
            equality->associated_type, candidate, candidate_definition);
        if (associated != NULL
            && (!cm_hir_associated_declaration_shape_valid(associated)
                || (!allow_inherited
                    && !cm_hir_def_id_equal(
                        associated->parent_definition, trait_definition))
                || (allow_inherited && resolved_trait != NULL
                    && (!cm_hir_trait_reaches_candidate(context,
                        trait_definition, associated->parent_definition,
                        candidate, candidate_definition, 0u)
                        || !cm_hir_associated_name_unambiguous(context,
                            associated, trait_definition, candidate,
                            candidate_definition))))) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            const CmHirItem *prior_associated;

            if (cm_hir_def_id_equal(equalities[prior].associated_type,
                    equality->associated_type)) {
                return 0;
            }
            prior_associated = cm_hir_bound_item_or_candidate(context,
                equalities[prior].associated_type, candidate,
                candidate_definition);
            if (associated != NULL && prior_associated != NULL
                && associated->name == prior_associated->name) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_associated_type_bound_valid(
    const CmHirContext *context, const CmHirItem *owner,
    const CmHirAssociatedTypeBound *bound, const CmHirItem *candidate,
    CmHirDefId candidate_definition)
{
    const CmHirModule *module;
    const CmHirDefinition *trait_definition;
    const CmHirItem *trait_item;

    module = cm_hir_get_module(context, owner->owner_module);
    if (module == NULL
        || (unsigned int)bound->modifier
            > (unsigned int)CM_HIR_ASSOC_BOUND_RELAXED
        || !cm_hir_span_is_ordered(bound->span)
        || !cm_hir_named_type_valid(context, &bound->trait_type)
        || !cm_hir_named_late_bound_free(context, &bound->trait_type, 0u)
        || !cm_hir_named_self_owner_valid(context, &bound->trait_type,
            owner->parent_definition, 0u)
        || (bound->modifier == CM_HIR_ASSOC_BOUND_RELAXED
            && bound->equality_count != 0u)
        || (!cm_hir_def_id_is_none(owner->definition)
            && cm_hir_def_id_equal(bound->trait_type.definition,
                owner->definition))) {
        return 0;
    }
    trait_definition = cm_hir_lookup_definition(context,
        bound->trait_type.definition);
    if (trait_definition == NULL
        || trait_definition->kind != CM_HIR_DEFINITION_ITEM) {
        return 0;
    }
    if (trait_definition->state == CM_HIR_DEFINITION_RESERVED
        && trait_definition->has_reserved_item_kind
        && trait_definition->reserved_item_kind != CM_HIR_ITEM_TRAIT
        && trait_definition->reserved_item_kind
            != CM_HIR_ITEM_TRAIT_ALIAS) {
        return 0;
    }
    trait_item = cm_hir_bound_item_or_candidate(context,
        bound->trait_type.definition, candidate, candidate_definition);
    if (trait_item != NULL
        && ((trait_item->kind != CM_HIR_ITEM_TRAIT
                && trait_item->kind != CM_HIR_ITEM_TRAIT_ALIAS)
            || (trait_item->kind == CM_HIR_ITEM_TRAIT_ALIAS
                && bound->equality_count != 0u)
            || !cm_hir_named_type_matches_item_parameters(context,
                &bound->trait_type, trait_item))) {
        return 0;
    }
    if (!cm_hir_associated_equalities_valid(context, owner,
            owner->definition, bound->trait_type.definition,
            bound->equalities, bound->equality_count, candidate,
            candidate_definition, owner->parent_definition, 0)) return 0;
    {
        uint32_t index;

        for (index = 0u; index < bound->equality_count; ++index) {
            if (!cm_hir_type_late_bound_free(context,
                    bound->equalities[index].value, 0u)) return 0;
        }
    }
    return 1;
}

static int cm_hir_associated_type_bounds_valid(
    const CmHirContext *context, const CmHirItem *item)
{
    uint32_t index;

    if ((item->data.type_alias_item.bound_count != 0u
            && item->data.type_alias_item.bounds == NULL)
        || (item->data.type_alias_item.bound_count == 0u
            && item->data.type_alias_item.bounds != NULL)) {
        return 0;
    }
    for (index = 0u; index < item->data.type_alias_item.bound_count;
         ++index) {
        uint32_t prior;

        if (!cm_hir_associated_type_bound_valid(context, item,
                &item->data.type_alias_item.bounds[index], NULL,
                cm_hir_def_id_none())) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (cm_hir_def_id_equal(item->data.type_alias_item.bounds[prior]
                        .trait_type.definition,
                    item->data.type_alias_item.bounds[index]
                        .trait_type.definition)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_type_alias_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    const CmHirItem *parent;
    const CmHirItem *trait_declaration;

    if (cm_hir_def_id_is_none(item->parent_definition)) {
        return cm_hir_type_id_valid(context,
                item->data.type_alias_item.target)
            && cm_hir_type_late_bound_free(context,
                item->data.type_alias_item.target, 0u)
            && cm_hir_type_self_owner_valid(context,
                item->data.type_alias_item.target, cm_hir_def_id_none(), 0u)
            && cm_hir_def_id_is_none(
                item->data.type_alias_item.trait_item_definition)
            && item->data.type_alias_item.bound_count == 0u
            && item->data.type_alias_item.bounds == NULL;
    }
    parent = cm_hir_bound_definition_item(context,
        item->parent_definition);
    if (parent == NULL) return 0;
    if (parent->kind == CM_HIR_ITEM_TRAIT) {
        return item->data.type_alias_item.target == CM_HIR_TYPE_NONE
            && cm_hir_def_id_is_none(
                item->data.type_alias_item.trait_item_definition)
            && cm_hir_associated_type_bounds_valid(context, item);
    }
    if (parent->kind != CM_HIR_ITEM_IMPL
        || !cm_hir_type_id_valid(context,
            item->data.type_alias_item.target)
        || !cm_hir_type_late_bound_free(context,
            item->data.type_alias_item.target, 0u)
        || !cm_hir_type_self_owner_valid(context,
            item->data.type_alias_item.target, item->parent_definition, 0u)
        || cm_hir_def_id_is_none(
            item->data.type_alias_item.trait_item_definition)
        || item->data.type_alias_item.bound_count != 0u
        || item->data.type_alias_item.bounds != NULL
        || parent->data.impl_item.has_trait != 1
        || parent->data.impl_item.polarity == CM_HIR_IMPL_NEGATIVE) {
        return 0;
    }
    trait_declaration = cm_hir_bound_definition_item(context,
        item->data.type_alias_item.trait_item_definition);
    return trait_declaration != NULL
        && trait_declaration->kind == CM_HIR_ITEM_TYPE_ALIAS
        && trait_declaration->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_is_none(
            trait_declaration->data.type_alias_item.trait_item_definition)
        && cm_hir_def_id_equal(trait_declaration->parent_definition,
            parent->data.impl_item.trait_type.definition)
        && trait_declaration->name == item->name
        && trait_declaration->generic_parameter_count
            == item->generic_parameter_count
        && cm_hir_impl_alias_link_is_unique(context, item);
}

static int cm_hir_value_item_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    const CmHirBody *body;
    const CmHirItem *parent;
    const CmHirItem *trait_declaration;
    CmHirDefId expected_owner;

    expected_owner = item->parent_definition;
    if (!cm_hir_type_id_valid(context, item->data.value_item.type)
        || !cm_hir_type_late_bound_free(context,
            item->data.value_item.type, 0u)
        || !cm_hir_type_self_owner_valid(context,
            item->data.value_item.type, expected_owner, 0u)
        || (unsigned int)item->data.value_item.mutability >
            (unsigned int)CM_HIR_MUTABLE
        || (item->data.value_item.has_default_body != 0
            && item->data.value_item.has_default_body != 1)
        || !cm_hir_body_self_roots_valid(context,
            item->data.value_item.body, expected_owner)) {
        return 0;
    }
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        if (!cm_hir_def_id_is_none(
                item->data.value_item.trait_item_definition)
            || item->data.value_item.has_default_body != 0
            || item->data.value_item.body == CM_HIR_BODY_NONE) return 0;
    } else {
        parent = cm_hir_bound_definition_item(context,
            item->parent_definition);
        if (parent == NULL || item->kind != CM_HIR_ITEM_CONST) return 0;
        if (parent->kind == CM_HIR_ITEM_TRAIT) {
            if (item->data.value_item.mutability != CM_HIR_IMMUTABLE
                || !cm_hir_def_id_is_none(
                    item->data.value_item.trait_item_definition)) return 0;
            if (item->data.value_item.body != CM_HIR_BODY_NONE
                && item->data.value_item.has_default_body != 1) return 0;
            if (item->data.value_item.body == CM_HIR_BODY_NONE) return 1;
            body = cm_hir_get_body(context, item->data.value_item.body);
            return body != NULL
                && body->expected_type == item->data.value_item.type;
        }
        if (parent->kind != CM_HIR_ITEM_IMPL
            || parent->data.impl_item.polarity == CM_HIR_IMPL_NEGATIVE
            || item->data.value_item.has_default_body != 0
            || item->data.value_item.body == CM_HIR_BODY_NONE
            || item->data.value_item.mutability != CM_HIR_IMMUTABLE) {
            return 0;
        }
        if (!parent->data.impl_item.has_trait) {
            if (!cm_hir_def_id_is_none(
                    item->data.value_item.trait_item_definition)) return 0;
        } else {
            trait_declaration = cm_hir_bound_definition_item(context,
                item->data.value_item.trait_item_definition);
            if (trait_declaration == NULL
                || trait_declaration->kind != CM_HIR_ITEM_CONST
                || !cm_hir_def_id_equal(
                    trait_declaration->parent_definition,
                    parent->data.impl_item.trait_type.definition)
                || trait_declaration->name != item->name
                || !cm_hir_impl_value_link_is_unique(context, item)) {
                return 0;
            }
        }
    }
    body = cm_hir_get_body(context, item->data.value_item.body);
    return body != NULL
        && body->expected_type == item->data.value_item.type;
}

static int cm_hir_trait_item_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    const CmHirModule *module;
    uint32_t index;

    module = cm_hir_get_module(context, item->owner_module);
    if (module == NULL
        || (unsigned int)item->data.trait_item.safety >
            (unsigned int)CM_HIR_UNSAFE
        || (item->data.trait_item.is_auto != 0
            && item->data.trait_item.is_auto != 1)
        || (item->data.trait_item.is_const != 0
            && item->data.trait_item.is_const != 1)
        || (item->data.trait_item.supertrait_count != 0u
            && item->data.trait_item.supertraits == NULL)) {
        return 0;
    }
    if (item->data.trait_item.is_auto
        && (item->generic_parameter_count != 0u
            || item->data.trait_item.supertrait_count != 0u
            || item->predicate_scope_count != 0u
            || item->predicate_count != 0u
            || item->outlives_predicate_count != 0u)) {
        return 0;
    }
    for (index = 0u; index < item->data.trait_item.supertrait_count;
         ++index) {
        const CmHirSupertrait *supertrait;
        const CmHirDefinition *definition;
        uint32_t prior;

        supertrait = &item->data.trait_item.supertraits[index];
        if ((unsigned int)supertrait->modifier >
                (unsigned int)CM_HIR_SUPERTRAIT_CONST_IF_CONST
            || !cm_hir_span_is_ordered(supertrait->span)
            || !cm_hir_named_type_valid(context,
                &supertrait->trait_type)
            || !cm_hir_named_late_bound_free(context,
                &supertrait->trait_type, 0u)
            || (!cm_hir_def_id_is_none(item->definition)
                && cm_hir_def_id_equal(supertrait->trait_type.definition,
                    item->definition))) {
            return 0;
        }
        if (!cm_hir_associated_equalities_valid(context, item,
                item->definition, supertrait->trait_type.definition,
                supertrait->equalities, supertrait->equality_count, NULL,
                cm_hir_def_id_none(), item->definition, 1)) {
            return 0;
        }
        {
            uint32_t equality_index;

            for (equality_index = 0u;
                 equality_index < supertrait->equality_count;
                 ++equality_index) {
                if (!cm_hir_predicate_type_in_scope(context,
                        supertrait->equalities[equality_index].value,
                        item->definition, item->parent_definition, NULL,
                        0u)) {
                    return 0;
                }
            }
        }
        for (prior = 0u; prior < index; ++prior) {
            if (cm_hir_named_type_equal(&item->data.trait_item
                        .supertraits[prior].trait_type,
                    &supertrait->trait_type)) {
                return 0;
            }
        }
        definition = cm_hir_lookup_definition(context,
            supertrait->trait_type.definition);
        if (definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ITEM) return 0;
        if (definition->state == CM_HIR_DEFINITION_BOUND) {
            const CmHirItem *target;

            target = cm_hir_bound_definition_item(context,
                supertrait->trait_type.definition);
            if (target == NULL
                || (target->kind != CM_HIR_ITEM_TRAIT
                    && target->kind != CM_HIR_ITEM_TRAIT_ALIAS)
                || (target->kind == CM_HIR_ITEM_TRAIT_ALIAS
                    && supertrait->equality_count != 0u)
                || !cm_hir_named_type_matches_item_parameters(context,
                    &supertrait->trait_type, target)) {
                return 0;
            }
        } else if (definition->has_reserved_item_kind
            && definition->reserved_item_kind != CM_HIR_ITEM_TRAIT
            && definition->reserved_item_kind
                != CM_HIR_ITEM_TRAIT_ALIAS) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_trait_alias_item_payload_valid(
    const CmHirContext *context, const CmHirItem *item)
{
    uint32_t index;

    if ((item->data.trait_alias_item.bound_count != 0u
            && item->data.trait_alias_item.bounds == NULL)
        || (item->data.trait_alias_item.bound_count == 0u
            && item->data.trait_alias_item.bounds != NULL)) {
        return 0;
    }
    for (index = 0u; index < item->data.trait_alias_item.bound_count;
         ++index) {
        const CmHirTraitAliasBound *bound;

        bound = &item->data.trait_alias_item.bounds[index];
        if ((unsigned int)bound->kind
                > (unsigned int)CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME
            || !cm_hir_span_is_ordered(bound->span)
            || bound->span.source != item->span.source
            || bound->span.start < item->span.start
            || bound->span.end > item->span.end) {
            return 0;
        }
        if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME) {
            if (!cm_hir_region_valid(context, &bound->data.lifetime)
                || !cm_hir_predicate_region_in_scope(context,
                    &bound->data.lifetime, item->definition,
                    item->parent_definition, NULL)) {
                return 0;
            }
        } else {
            const CmHirSupertrait *trait_bound;
            const CmHirDefinition *definition;

            trait_bound = &bound->data.trait_bound;
            if ((unsigned int)trait_bound->modifier >
                    (unsigned int)CM_HIR_SUPERTRAIT_CONST_IF_CONST
                || trait_bound->span.source != bound->span.source
                || trait_bound->span.start != bound->span.start
                || trait_bound->span.end != bound->span.end
                || !cm_hir_named_type_valid(context,
                    &trait_bound->trait_type)
                || !cm_hir_named_late_bound_free(context,
                    &trait_bound->trait_type, 0u)
                || !cm_hir_predicate_named_in_scope(context,
                    &trait_bound->trait_type, item->definition,
                    item->parent_definition, NULL, 0u)
                || (!cm_hir_def_id_is_none(item->definition)
                    && cm_hir_def_id_equal(
                        trait_bound->trait_type.definition,
                        item->definition))) {
                return 0;
            }
            definition = cm_hir_lookup_definition(context,
                trait_bound->trait_type.definition);
            if (definition == NULL
                || definition->kind != CM_HIR_DEFINITION_ITEM) {
                return 0;
            }
            if (definition->state == CM_HIR_DEFINITION_BOUND) {
                const CmHirItem *target;

                target = cm_hir_bound_definition_item(context,
                    trait_bound->trait_type.definition);
                if (target == NULL
                    || (target->kind != CM_HIR_ITEM_TRAIT
                        && target->kind != CM_HIR_ITEM_TRAIT_ALIAS)
                    || (target->kind == CM_HIR_ITEM_TRAIT_ALIAS
                        && trait_bound->equality_count != 0u)
                    || !cm_hir_named_type_matches_item_parameters(context,
                        &trait_bound->trait_type, target)) {
                    return 0;
                }
            } else if (definition->has_reserved_item_kind
                && definition->reserved_item_kind != CM_HIR_ITEM_TRAIT
                && definition->reserved_item_kind
                    != CM_HIR_ITEM_TRAIT_ALIAS) {
                return 0;
            }
            if (!cm_hir_associated_equalities_valid(context, item,
                    item->definition,
                    trait_bound->trait_type.definition,
                    trait_bound->equalities, trait_bound->equality_count,
                    NULL, cm_hir_def_id_none(), item->definition, 1)) {
                return 0;
            }
            {
                uint32_t equality_index;

                for (equality_index = 0u;
                     equality_index < trait_bound->equality_count;
                     ++equality_index) {
                    if (!cm_hir_predicate_type_in_scope(context,
                            trait_bound->equalities[equality_index].value,
                            item->definition, item->parent_definition,
                            NULL, 0u)) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

static int cm_hir_item_payload_valid(const CmHirContext *context,
    const CmHirItem *item)
{
    uint32_t index;

    switch (item->kind) {
    case CM_HIR_ITEM_FUNCTION:
        return cm_hir_function_item_payload_valid(context, item);
    case CM_HIR_ITEM_STRUCT:
    case CM_HIR_ITEM_UNION:
        return cm_hir_fields_valid(context,
                item->data.aggregate_item.fields,
                item->data.aggregate_item.field_count,
                item->data.aggregate_item.form)
            && cm_hir_fields_self_roots_valid(context,
                item->data.aggregate_item.fields,
                item->data.aggregate_item.field_count,
                cm_hir_def_id_none());
    case CM_HIR_ITEM_ENUM:
        if (item->data.enum_item.variant_count != 0u
            && item->data.enum_item.variants == NULL) {
            return 0;
        }
        for (index = 0u; index < item->data.enum_item.variant_count;
             ++index) {
            const CmHirVariant *variant;

            variant = &item->data.enum_item.variants[index];
            if (!cm_hir_intern_id_valid(context, variant->name)
                || !cm_hir_fields_valid(context, variant->fields,
                    variant->field_count, variant->form)
                || !cm_hir_fields_self_roots_valid(context,
                    variant->fields, variant->field_count,
                    cm_hir_def_id_none())
                || !cm_hir_span_is_ordered(variant->span)
                || (variant->has_discriminant != 0
                    && variant->has_discriminant != 1)
                || (variant->has_discriminant
                    && (!cm_hir_const_valid(context,
                            &variant->discriminant)
                        || !cm_hir_type_late_bound_free(context,
                            variant->discriminant.type, 0u)
                        || !cm_hir_type_self_owner_valid(context,
                            variant->discriminant.type,
                            cm_hir_def_id_none(), 0u)))) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_ITEM_TYPE_ALIAS:
        return cm_hir_type_alias_payload_valid(context, item);
    case CM_HIR_ITEM_CONST:
    case CM_HIR_ITEM_STATIC:
        return cm_hir_value_item_payload_valid(context, item);
    case CM_HIR_ITEM_MODULE:
        return cm_hir_get_module(context,
            item->data.module_item.module_id) != NULL;
    case CM_HIR_ITEM_TRAIT:
        return cm_hir_trait_item_payload_valid(context, item);
    case CM_HIR_ITEM_IMPL:
        return cm_hir_impl_item_payload_valid(context, item);
    case CM_HIR_ITEM_EXTERN_TYPE:
        return 1;
    case CM_HIR_ITEM_TRAIT_ALIAS:
        return cm_hir_trait_alias_item_payload_valid(context, item);
    }
    return 0;
}

static void cm_hir_copy_item_payload(CmHirContext *context,
    CmHirItem *copy, const CmHirItem *item)
{
    uint32_t index;

    switch (item->kind) {
    case CM_HIR_ITEM_FUNCTION:
        copy->data.function_item.signature.parameters =
            (CmHirFunctionParameter *)cm_hir_copy_array(context,
                item->data.function_item.signature.parameters,
                item->data.function_item.signature.parameter_count,
                sizeof(CmHirFunctionParameter));
        break;
    case CM_HIR_ITEM_STRUCT:
    case CM_HIR_ITEM_UNION:
        copy->data.aggregate_item.fields = (CmHirField *)cm_hir_copy_array(
            context, item->data.aggregate_item.fields,
            item->data.aggregate_item.field_count, sizeof(CmHirField));
        break;
    case CM_HIR_ITEM_ENUM:
        copy->data.enum_item.variants = (CmHirVariant *)cm_hir_copy_array(
            context, item->data.enum_item.variants,
            item->data.enum_item.variant_count, sizeof(CmHirVariant));
        for (index = 0u; index < copy->data.enum_item.variant_count;
             ++index) {
            copy->data.enum_item.variants[index].fields =
                (CmHirField *)cm_hir_copy_array(context,
                    item->data.enum_item.variants[index].fields,
                    item->data.enum_item.variants[index].field_count,
                    sizeof(CmHirField));
        }
        break;
    case CM_HIR_ITEM_TYPE_ALIAS:
        copy->data.type_alias_item.bounds =
            (CmHirAssociatedTypeBound *)cm_hir_copy_array(context,
                item->data.type_alias_item.bounds,
                item->data.type_alias_item.bound_count,
                sizeof(CmHirAssociatedTypeBound));
        for (index = 0u;
             index < copy->data.type_alias_item.bound_count; ++index) {
            copy->data.type_alias_item.bounds[index].trait_type =
                cm_hir_copy_named_type(context,
                    &item->data.type_alias_item.bounds[index].trait_type);
            copy->data.type_alias_item.bounds[index].equalities =
                (CmHirAssociatedTypeEquality *)cm_hir_copy_array(context,
                    item->data.type_alias_item.bounds[index].equalities,
                    item->data.type_alias_item.bounds[index].equality_count,
                    sizeof(CmHirAssociatedTypeEquality));
        }
        break;
    case CM_HIR_ITEM_TRAIT:
        copy->data.trait_item.supertraits =
            (CmHirSupertrait *)cm_hir_copy_array(context,
                item->data.trait_item.supertraits,
                item->data.trait_item.supertrait_count,
                sizeof(CmHirSupertrait));
        for (index = 0u;
             index < copy->data.trait_item.supertrait_count; ++index) {
            copy->data.trait_item.supertraits[index].trait_type =
                cm_hir_copy_named_type(context,
                    &item->data.trait_item.supertraits[index].trait_type);
            copy->data.trait_item.supertraits[index].equalities =
                (CmHirAssociatedTypeEquality *)cm_hir_copy_array(context,
                    item->data.trait_item.supertraits[index].equalities,
                    item->data.trait_item.supertraits[index]
                        .equality_count,
                    sizeof(CmHirAssociatedTypeEquality));
        }
        break;
    case CM_HIR_ITEM_TRAIT_ALIAS:
        copy->data.trait_alias_item.bounds =
            (CmHirTraitAliasBound *)cm_hir_copy_array(context,
                item->data.trait_alias_item.bounds,
                item->data.trait_alias_item.bound_count,
                sizeof(CmHirTraitAliasBound));
        for (index = 0u;
             index < copy->data.trait_alias_item.bound_count; ++index) {
            if (copy->data.trait_alias_item.bounds[index].kind
                    != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                continue;
            }
            copy->data.trait_alias_item.bounds[index].data.trait_bound
                .trait_type = cm_hir_copy_named_type(context,
                    &item->data.trait_alias_item.bounds[index].data
                        .trait_bound.trait_type);
            copy->data.trait_alias_item.bounds[index].data.trait_bound
                .equalities =
                (CmHirAssociatedTypeEquality *)cm_hir_copy_array(context,
                    item->data.trait_alias_item.bounds[index].data
                        .trait_bound.equalities,
                    item->data.trait_alias_item.bounds[index].data
                        .trait_bound.equality_count,
                    sizeof(CmHirAssociatedTypeEquality));
        }
        break;
    case CM_HIR_ITEM_IMPL:
        if (item->data.impl_item.has_trait) {
            copy->data.impl_item.trait_type = cm_hir_copy_named_type(context,
                &item->data.impl_item.trait_type);
        }
        break;
    default:
        break;
    }
}

static void cm_hir_copy_item_predicates(CmHirContext *context,
    CmHirItem *copy, const CmHirItem *item)
{
    uint32_t index;

    copy->predicate_scopes = (CmHirPredicateScope *)cm_hir_copy_array(
        context, item->predicate_scopes, item->predicate_scope_count,
        sizeof(CmHirPredicateScope));
    for (index = 0u; index < copy->predicate_scope_count; ++index) {
        copy->predicate_scopes[index].binder.lifetimes =
            (CmInternId *)cm_hir_copy_array(context,
                item->predicate_scopes[index].binder.lifetimes,
                item->predicate_scopes[index].binder.lifetime_count,
                sizeof(CmInternId));
    }
    copy->predicates = (CmHirTraitPredicate *)cm_hir_copy_array(context,
        item->predicates, item->predicate_count,
        sizeof(CmHirTraitPredicate));
    for (index = 0u; index < copy->predicate_count; ++index) {
        copy->predicates[index].binder.lifetimes =
            (CmInternId *)cm_hir_copy_array(context,
                item->predicates[index].binder.lifetimes,
                item->predicates[index].binder.lifetime_count,
                sizeof(CmInternId));
        copy->predicates[index].trait_type = cm_hir_copy_named_type(context,
            &item->predicates[index].trait_type);
        copy->predicates[index].equalities =
            (CmHirAssociatedTypeEquality *)cm_hir_copy_array(context,
                item->predicates[index].equalities,
                item->predicates[index].equality_count,
                sizeof(CmHirAssociatedTypeEquality));
    }
    copy->outlives_predicates =
        (CmHirOutlivesPredicate *)cm_hir_copy_array(context,
            item->outlives_predicates, item->outlives_predicate_count,
            sizeof(CmHirOutlivesPredicate));
}

static int cm_hir_item_generic_range_valid(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId definition)
{
    size_t index;
    size_t owned_count;
    uint32_t offset;
    int saw_default;
    int saw_non_lifetime;

    if (item->generic_parameter_count == 0u) {
        if (item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE) {
            return 0;
        }
    } else {
        if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            || item->generic_parameter_count - 1u > UINT32_MAX
                - item->generic_parameter_start) {
            return 0;
        }
        saw_default = 0;
        saw_non_lifetime = 0;
        for (offset = 0u; offset < item->generic_parameter_count; ++offset) {
            const CmHirGenericParam *parameter;
            CmHirDefId default_self_owner;

            parameter = cm_hir_get_generic_param(context,
                item->generic_parameter_start + offset);
            if (parameter == NULL
                || !cm_hir_def_id_equal(parameter->owner, definition)
                || parameter->index != offset
                || (parameter->is_relaxed_sized != 0
                    && parameter->is_relaxed_sized != 1)
                || (parameter->is_relaxed_sized
                    && parameter->kind != CM_HIR_GENERIC_TYPE)) {
                return 0;
            }
            default_self_owner = item->kind == CM_HIR_ITEM_TRAIT
                    || item->kind == CM_HIR_ITEM_IMPL
                    || item->kind == CM_HIR_ITEM_TRAIT_ALIAS
                ? definition : cm_hir_def_id_none();
            if (parameter->kind == CM_HIR_GENERIC_CONST
                && !cm_hir_type_self_owner_valid(context,
                    parameter->declared_type, cm_hir_def_id_none(), 0u)) {
                return 0;
            }
            if (parameter->has_default
                && !cm_hir_generic_arg_self_owner_valid(context,
                    &parameter->default_argument, default_self_owner, 0u)) {
                return 0;
            }
            if (item->kind == CM_HIR_ITEM_FUNCTION
                && parameter->has_default) {
                return 0;
            }
            if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
                if (saw_non_lifetime || parameter->has_default) return 0;
            } else {
                saw_non_lifetime = 1;
                if (parameter->has_default) {
                    saw_default = 1;
                } else if (saw_default) {
                    return 0;
                }
            }
        }
    }
    owned_count = 0u;
    for (index = 0u; index < context->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &context->generic_parameters, index);
        if (parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, definition)) {
            ++owned_count;
        }
    }
    return owned_count == (size_t)item->generic_parameter_count;
}

static int cm_hir_trait_bound_reaches(const CmHirContext *context,
    CmHirDefId start, CmHirDefId target)
{
    unsigned char *seen;
    CmVec pending;
    CmHirDefId current;
    int reaches;

    seen = (unsigned char *)cm_alloc_zeroed(
        context->items.len == 0u ? 1u : context->items.len,
        sizeof(unsigned char));
    cm_vec_init(&pending, sizeof(CmHirDefId));
    (void)cm_vec_push(&pending, &start);
    reaches = 0;
    while (cm_vec_pop(&pending, &current)) {
        const CmHirDefinition *definition;
        const CmHirItem *item;
        size_t item_index;
        uint32_t index;

        if (cm_hir_def_id_equal(current, target)) {
            reaches = 1;
            break;
        }
        definition = cm_hir_lookup_definition(context, current);
        if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
            || definition->state != CM_HIR_DEFINITION_BOUND) {
            continue;
        }
        if (definition->entity.item_id == CM_HIR_ITEM_NONE
            || (size_t)definition->entity.item_id > context->items.len) {
            reaches = 1;
            break;
        }
        item_index = (size_t)definition->entity.item_id - 1u;
        if (seen[item_index]) continue;
        seen[item_index] = 1u;
        item = cm_hir_get_item(context, definition->entity.item_id);
        if (item == NULL) {
            reaches = 1;
            break;
        }
        if (item->kind == CM_HIR_ITEM_TRAIT) {
            for (index = 0u;
                 index < item->data.trait_item.supertrait_count;
                 ++index) {
                (void)cm_vec_push(&pending,
                    &item->data.trait_item.supertraits[index]
                        .trait_type.definition);
            }
        } else if (item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
            for (index = 0u;
                 index < item->data.trait_alias_item.bound_count;
                 ++index) {
                const CmHirTraitAliasBound *bound;

                bound = &item->data.trait_alias_item.bounds[index];
                if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) {
                    (void)cm_vec_push(&pending,
                        &bound->data.trait_bound.trait_type.definition);
                }
            }
        }
    }
    cm_vec_destroy(&pending);
    cm_free(seen);
    return reaches;
}

static int cm_hir_item_supertrait_binding_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId definition)
{
    size_t item_index;
    uint32_t supertrait_index;

    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *stored;

        stored = (const CmHirItem *)cm_vec_at_const(&context->items,
            item_index);
        if (stored == NULL || stored->kind != CM_HIR_ITEM_TRAIT) continue;
        for (supertrait_index = 0u;
             supertrait_index
                < stored->data.trait_item.supertrait_count;
             ++supertrait_index) {
            const CmHirSupertrait *supertrait;

            supertrait = &stored->data.trait_item
                .supertraits[supertrait_index];
            if (cm_hir_def_id_equal(stored->data.trait_item
                        .supertraits[supertrait_index]
                        .trait_type.definition,
                    definition)) {
                if ((item->kind != CM_HIR_ITEM_TRAIT
                        && item->kind != CM_HIR_ITEM_TRAIT_ALIAS)
                    || (item->kind == CM_HIR_ITEM_TRAIT_ALIAS
                        && supertrait->equality_count != 0u)
                    || !cm_hir_named_type_matches_item_parameters(context,
                        &stored->data.trait_item
                            .supertraits[supertrait_index].trait_type,
                        item)) {
                    return 0;
                }
            }
            if (!cm_hir_associated_equalities_valid(context, stored,
                    stored->definition,
                    supertrait->trait_type.definition,
                    supertrait->equalities, supertrait->equality_count,
                    item, definition, stored->definition, 1)) {
                return 0;
            }
        }
    }
    if (item->kind != CM_HIR_ITEM_TRAIT) return 1;
    for (supertrait_index = 0u;
         supertrait_index < item->data.trait_item.supertrait_count;
         ++supertrait_index) {
        if (!cm_hir_named_self_owner_valid(context,
                &item->data.trait_item.supertraits[supertrait_index]
                    .trait_type,
                definition, 0u)
            || cm_hir_trait_bound_reaches(context,
                item->data.trait_item.supertraits[supertrait_index]
                    .trait_type.definition,
                definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_item_trait_alias_binding_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId definition)
{
    size_t item_index;
    uint32_t bound_index;

    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *stored;

        stored = (const CmHirItem *)cm_vec_at_const(&context->items,
            item_index);
        if (stored == NULL
            || stored->kind != CM_HIR_ITEM_TRAIT_ALIAS) {
            continue;
        }
        for (bound_index = 0u;
             bound_index < stored->data.trait_alias_item.bound_count;
             ++bound_index) {
            const CmHirTraitAliasBound *bound;
            const CmHirSupertrait *trait_bound;

            bound = &stored->data.trait_alias_item.bounds[bound_index];
            if (bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) continue;
            trait_bound = &bound->data.trait_bound;
            if (cm_hir_def_id_equal(trait_bound->trait_type.definition,
                    definition)
                && ((item->kind != CM_HIR_ITEM_TRAIT
                        && item->kind != CM_HIR_ITEM_TRAIT_ALIAS)
                    || (item->kind == CM_HIR_ITEM_TRAIT_ALIAS
                        && trait_bound->equality_count != 0u)
                    || !cm_hir_named_type_matches_item_parameters(context,
                        &trait_bound->trait_type, item))) {
                return 0;
            }
            if (!cm_hir_associated_equalities_valid(context, stored,
                    stored->definition,
                    trait_bound->trait_type.definition,
                    trait_bound->equalities, trait_bound->equality_count,
                    item, definition, stored->definition, 1)) {
                return 0;
            }
        }
    }
    if (item->kind != CM_HIR_ITEM_TRAIT_ALIAS) return 1;
    for (bound_index = 0u;
         bound_index < item->data.trait_alias_item.bound_count;
         ++bound_index) {
        const CmHirTraitAliasBound *bound;

        bound = &item->data.trait_alias_item.bounds[bound_index];
        if (bound->kind != CM_HIR_TRAIT_ALIAS_BOUND_TRAIT) continue;
        if (!cm_hir_named_self_owner_valid(context,
                &bound->data.trait_bound.trait_type,
                definition, 0u)
            || cm_hir_trait_bound_reaches(context,
                bound->data.trait_bound.trait_type.definition,
                definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_item_associated_bound_binding_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId definition)
{
    size_t item_index;

    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *stored;
        uint32_t bound_index;

        stored = (const CmHirItem *)cm_vec_at_const(&context->items,
            item_index);
        if (stored == NULL || stored->kind != CM_HIR_ITEM_TYPE_ALIAS) {
            continue;
        }
        for (bound_index = 0u;
             bound_index < stored->data.type_alias_item.bound_count;
             ++bound_index) {
            if (!cm_hir_associated_type_bound_valid(context, stored,
                    &stored->data.type_alias_item.bounds[bound_index], item,
                    definition)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_item_predicate_binding_valid(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId definition)
{
    size_t item_index;

    if (!cm_hir_item_predicates_valid(context, item, item, definition,
            definition)
        || !cm_hir_item_outlives_predicates_valid(context, item,
            definition)) {
        return 0;
    }
    for (item_index = 0u; item_index < context->items.len; ++item_index) {
        const CmHirItem *stored;

        stored = (const CmHirItem *)cm_vec_at_const(&context->items,
            item_index);
        if (stored != NULL
            && !cm_hir_item_predicates_valid(context, stored, item,
                definition, stored->definition)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_enum_variant_imports_valid(const CmHirContext *context,
    CmHirDefId definition, CmHirAggregateForm form)
{
    size_t module_index;

    for (module_index = 0u; module_index < context->modules.len;
         ++module_index) {
        const CmHirModule *module;
        uint32_t import_index;

        module = (const CmHirModule *)cm_vec_at_const(&context->modules,
            module_index);
        if (module == NULL) return 0;
        for (import_index = 0u; import_index < module->import_count;
             ++import_index) {
            const CmHirImport *import_value;
            uint32_t binding_index;

            import_value = &module->imports[import_index];
            for (binding_index = 0u;
                 binding_index < import_value->binding_count;
                 ++binding_index) {
                const CmHirImportBinding *binding;

                binding = &import_value->bindings[binding_index];
                if (!cm_hir_def_id_equal(binding->target, definition)) {
                    continue;
                }
                if (binding->namespace_kind != CM_HIR_NAMESPACE_TYPE
                    && (binding->namespace_kind != CM_HIR_NAMESPACE_VALUE
                        || form == CM_HIR_AGGREGATE_NAMED)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int cm_hir_enum_variant_definitions_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirCrateId crate_id)
{
    uint32_t index;

    if (item->kind != CM_HIR_ITEM_ENUM) return 1;
    for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
        const CmHirVariant *variant;
        const CmHirDefinition *definition;
        uint32_t prior;

        variant = &item->data.enum_item.variants[index];
        if (cm_hir_def_id_is_none(variant->definition)) continue;
        definition = cm_hir_lookup_definition(context, variant->definition);
        if (definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ENUM_VARIANT
            || definition->state != CM_HIR_DEFINITION_RESERVED
            || definition->id.crate_id != crate_id
            || definition->span.source != variant->span.source
            || definition->span.start != variant->span.start
            || definition->span.end != variant->span.end
            || cm_hir_def_id_equal(variant->definition,
                item->definition)
            || !cm_hir_enum_variant_imports_valid(context,
                variant->definition, variant->form)) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (cm_hir_def_id_equal(item->data.enum_item.variants[prior]
                        .definition,
                    variant->definition)) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_prebound_trait_children_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId definition)
{
    size_t index;

    if (item->kind != CM_HIR_ITEM_TRAIT) return 1;
    for (index = 0u; index < context->prebound_associated_types.len;
         ++index) {
        const CmHirPreboundAssociatedType *child;

        child = (const CmHirPreboundAssociatedType *)cm_vec_at_const(
            &context->prebound_associated_types, index);
        if (child == NULL || !cm_hir_def_id_equal(
                child->parent_definition, definition)) {
            continue;
        }
        if (child->owner_module != item->owner_module) {
            return 0;
        }
        if (item->data.trait_item.is_auto) return 0;
    }
    return 1;
}

static int cm_hir_prebound_associated_publication_valid(
    const CmHirContext *context, const CmHirItem *item,
    CmHirDefId definition)
{
    size_t index;

    for (index = 0u; index < context->prebound_associated_types.len;
         ++index) {
        const CmHirPreboundAssociatedType *prebound;

        prebound = (const CmHirPreboundAssociatedType *)cm_vec_at_const(
            &context->prebound_associated_types, index);
        if (prebound == NULL
            || !cm_hir_def_id_equal(prebound->definition, definition)) {
            continue;
        }
        return item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && cm_hir_def_id_equal(item->parent_definition,
                prebound->parent_definition)
            && item->owner_module == prebound->owner_module
            && item->name == prebound->name
            && item->span.source == prebound->span.source
            && item->span.start == prebound->span.start
            && item->span.end == prebound->span.end
            && item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            && item->generic_parameter_count == 0u
            && item->data.type_alias_item.target == CM_HIR_TYPE_NONE
            && cm_hir_def_id_is_none(
                item->data.type_alias_item.trait_item_definition);
    }
    return 1;
}

static CmHirStatus cm_hir_add_item_internal(CmHirContext *context,
    const CmHirItem *item, CmHirItemId *out_id)
{
    const CmHirModule *module;
    CmHirDefinition *definition;
    CmHirDefId definition_id;
    CmHirItem copy;
    CmHirStatus status;

    if (context == NULL || item == NULL || out_id == NULL
        || !cm_hir_span_is_ordered(item->span)
        || !cm_hir_visibility_valid(context, &item->visibility)
        || !cm_hir_attributes_valid(context, item->attributes,
            item->attribute_count)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_ITEM_NONE;
    if ((item->kind == CM_HIR_ITEM_IMPL
            && item->name != CM_INTERN_ID_NONE)
        || (item->kind != CM_HIR_ITEM_IMPL
            && !cm_hir_intern_id_valid(context, item->name))) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    module = cm_hir_get_module(context, item->owner_module);
    if (module == NULL) {
        return CM_HIR_INVALID_ID;
    }
    status = cm_hir_item_parent_status(context, module, item);
    if (status != CM_HIR_OK) {
        return status;
    }
    if (!cm_hir_item_predicates_valid(context, item, NULL,
            cm_hir_def_id_none(), item->definition)
        || !cm_hir_item_outlives_predicates_valid(context, item,
            item->definition)) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_item_payload_valid(context, item)) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_enum_variant_definitions_valid(context, item,
            module->crate_id)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (context->items.len >= (size_t)UINT32_MAX) {
        return CM_HIR_ID_EXHAUSTED;
    }
    definition_id = item->definition;
    if (cm_hir_def_id_is_none(definition_id)) {
        status = cm_hir_reserve_definition(context, module->crate_id,
            CM_HIR_DEFINITION_ITEM, item->span, &definition_id);
        if (status != CM_HIR_OK) {
            return status;
        }
        definition = cm_hir_lookup_definition_mut(context, definition_id);
        if (definition == NULL) return CM_HIR_INVARIANT_VIOLATION;
        definition->reserved_item_kind = item->kind;
        definition->has_reserved_item_kind = 1;
    }
    definition = cm_hir_lookup_definition_mut(context, definition_id);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_RESERVED
        || definition_id.crate_id != module->crate_id
        || (definition->has_reserved_item_kind
            && definition->reserved_item_kind != item->kind)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (cm_hir_def_id_equal(definition_id, item->parent_definition)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (!cm_hir_item_generic_range_valid(context, item, definition_id)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (!cm_hir_prebound_trait_children_valid(context, item,
            definition_id)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (!cm_hir_prebound_associated_publication_valid(context, item,
            definition_id)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (!cm_hir_item_supertrait_binding_valid(context, item,
            definition_id)) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_item_trait_alias_binding_valid(context, item,
            definition_id)) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_item_associated_bound_binding_valid(context, item,
            definition_id)) {
        return CM_HIR_INVALID_ID;
    }
    if (!cm_hir_item_predicate_binding_valid(context, item,
            definition_id)) {
        return CM_HIR_INVALID_ID;
    }
    if (item->kind == CM_HIR_ITEM_FUNCTION
        && item->data.function_item.body != CM_HIR_BODY_NONE) {
        const CmHirBody *body;

        body = cm_hir_get_body(context,
            item->data.function_item.body);
        if (!cm_hir_body_origin_valid(body)
            || !cm_hir_def_id_equal(body->owner, definition_id)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    if ((item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC)
        && item->data.value_item.body != CM_HIR_BODY_NONE) {
        const CmHirBody *body;

        body = cm_hir_get_body(context, item->data.value_item.body);
        if (!cm_hir_body_origin_valid(body)
            || !cm_hir_def_id_equal(body->owner, definition_id)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    copy = *item;
    copy.definition = definition_id;
    copy.attributes = (CmHirAttribute *)cm_hir_copy_array(context,
        item->attributes, item->attribute_count, sizeof(CmHirAttribute));
    cm_hir_copy_item_predicates(context, &copy, item);
    cm_hir_copy_item_payload(context, &copy, item);
    if (copy.kind == CM_HIR_ITEM_ENUM) {
        uint32_t variant_index;

        for (variant_index = 0u;
             variant_index < copy.data.enum_item.variant_count;
             ++variant_index) {
            CmHirVariant *variant;

            variant = &copy.data.enum_item.variants[variant_index];
            if (!cm_hir_def_id_is_none(variant->definition)) continue;
            status = cm_hir_reserve_enum_variant_definition(context,
                module->crate_id, variant->span, &variant->definition);
            if (status != CM_HIR_OK) return status;
        }
    }
    status = cm_hir_push(context, &context->items, &copy, out_id);
    if (status != CM_HIR_OK) {
        return status;
    }
    definition = cm_hir_lookup_definition_mut(context, definition_id);
    if (definition == NULL) return CM_HIR_INVARIANT_VIOLATION;
    definition->state = CM_HIR_DEFINITION_BOUND;
    definition->entity.item_id = *out_id;
    if (copy.kind == CM_HIR_ITEM_ENUM) {
        uint32_t variant_index;

        for (variant_index = 0u;
             variant_index < copy.data.enum_item.variant_count;
             ++variant_index) {
            CmHirDefinition *variant_definition;

            variant_definition = cm_hir_lookup_definition_mut(context,
                copy.data.enum_item.variants[variant_index].definition);
            if (variant_definition == NULL
                || variant_definition->kind
                    != CM_HIR_DEFINITION_ENUM_VARIANT
                || variant_definition->state
                    != CM_HIR_DEFINITION_RESERVED) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            variant_definition->state = CM_HIR_DEFINITION_BOUND;
            variant_definition->entity.enum_variant.enum_item_id = *out_id;
            variant_definition->entity.enum_variant.variant_index =
                variant_index;
        }
    }
    return CM_HIR_OK;
}

CmHirStatus cm_hir_add_item(CmHirContext *context, const CmHirItem *item,
    CmHirItemId *out_id)
{
    return cm_hir_add_item_internal(context, item, out_id);
}

CmHirStatus cm_hir_prebind_trait_associated_type_declaration(
    CmHirContext *context, const CmHirItem *item, CmHirItemId *out_id)
{
    const CmHirDefinition *definition;
    const CmHirDefinition *parent;
    const CmHirModule *module;
    CmHirPreboundAssociatedType prebound;
    uint32_t ignored_id;
    size_t index;

    if (context == NULL || item == NULL || out_id == NULL
        || item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_hir_span_is_ordered(item->span)
        || !cm_hir_visibility_valid(context, &item->visibility)
        || !cm_hir_attributes_valid(context, item->attributes,
            item->attribute_count)
        || !cm_hir_intern_id_valid(context, item->name)
        || cm_hir_def_id_is_none(item->parent_definition)
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->data.type_alias_item.target != CM_HIR_TYPE_NONE
        || item->data.type_alias_item.bound_count != 0u
        || item->data.type_alias_item.bounds != NULL
        || item->predicate_scope_count != 0u
        || item->predicate_scopes != NULL
        || item->predicate_count != 0u
        || item->predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->outlives_predicates != NULL
        || !cm_hir_def_id_is_none(
            item->data.type_alias_item.trait_item_definition)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_ITEM_NONE;
    module = cm_hir_get_module(context, item->owner_module);
    if (module == NULL) return CM_HIR_INVALID_ID;
    definition = cm_hir_lookup_definition(context, item->definition);
    parent = cm_hir_lookup_definition(context, item->parent_definition);
    if (definition == NULL || parent == NULL
        || definition->kind != CM_HIR_DEFINITION_ITEM
        || parent->kind != CM_HIR_DEFINITION_ITEM) {
        return CM_HIR_INVALID_ID;
    }
    if (definition->state != CM_HIR_DEFINITION_RESERVED
        || !definition->has_reserved_item_kind
        || definition->reserved_item_kind != CM_HIR_ITEM_TYPE_ALIAS
        || definition->id.crate_id != module->crate_id
        || definition->span.source != item->span.source
        || definition->span.start != item->span.start
        || definition->span.end != item->span.end
        || parent->state != CM_HIR_DEFINITION_RESERVED
        || !parent->has_reserved_item_kind
        || parent->reserved_item_kind != CM_HIR_ITEM_TRAIT
        || parent->id.crate_id != module->crate_id) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    for (index = 0u; index < context->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &context->generic_parameters, index);
        if (parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, item->definition)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    for (index = 0u; index < context->prebound_associated_types.len;
         ++index) {
        const CmHirPreboundAssociatedType *existing;

        existing = (const CmHirPreboundAssociatedType *)cm_vec_at_const(
            &context->prebound_associated_types, index);
        if (existing != NULL
            && cm_hir_def_id_equal(existing->definition,
                item->definition)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    prebound.definition = item->definition;
    prebound.parent_definition = item->parent_definition;
    prebound.owner_module = item->owner_module;
    prebound.name = item->name;
    prebound.span = item->span;
    return cm_hir_push(context, &context->prebound_associated_types,
        &prebound, &ignored_id);
}

static int cm_hir_default_argument_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index,
    const CmHirGenericArg *argument, uint32_t binder_lifetime_count,
    size_t depth, size_t *node_budget);
static int cm_hir_default_type_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index, CmHirTypeId type_id,
    uint32_t binder_lifetime_count, size_t depth, size_t *node_budget);
/* The root type is depth one; permit 256 enclosing type constructors. */
#define CM_HIR_DEFAULT_MAX_DEPTH 257u
#define CM_HIR_DEFAULT_NODE_LIMIT 4096u
static int cm_hir_body_type_equal(const CmHirContext *context,
    CmHirTypeId left_id, CmHirTypeId right_id);
static int cm_hir_expression_body_span_valid(const CmHirBody *body,
    CmSpan span);
static int cm_hir_expression_scope_valid(const CmHirContext *context,
    CmHirExprId expression_id, CmHirBodyId body_id,
    uint32_t visible_local_count, CmHirClosureId active_closure,
    size_t depth);

static int cm_hir_default_parameter_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index,
    CmHirGenericParamId referenced)
{
    const CmHirGenericParam *parameter;

    parameter = cm_hir_get_generic_param(context, referenced);
    return parameter != NULL && cm_hir_def_id_equal(parameter->owner, owner)
        && parameter->index < parameter_index;
}

static int cm_hir_default_region_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index, const CmHirRegion *region,
    uint32_t binder_lifetime_count)
{
    if (region->kind == CM_HIR_REGION_LATE_BOUND) {
        return region->data.binder_index < binder_lifetime_count;
    }
    return region->kind != CM_HIR_REGION_EARLY_BOUND
        || cm_hir_default_parameter_in_scope(context, owner,
            parameter_index, region->data.parameter);
}

static int cm_hir_default_const_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index,
    const CmHirConstArg *constant, uint32_t binder_lifetime_count,
    size_t depth, size_t *node_budget)
{
    if (!cm_hir_default_type_in_scope(context, owner, parameter_index,
            constant->type, binder_lifetime_count, depth, node_budget)) {
        return 0;
    }
    return constant->kind != CM_HIR_CONST_PARAMETER
        || cm_hir_default_parameter_in_scope(context, owner,
            parameter_index, constant->data.parameter);
}

static int cm_hir_default_named_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index,
    const CmHirNamedType *named, uint32_t binder_lifetime_count,
    size_t depth, size_t *node_budget)
{
    uint32_t index;

    for (index = 0u; index < named->argument_count; ++index) {
        if (!cm_hir_default_argument_in_scope(context, owner,
                parameter_index, &named->arguments[index],
                binder_lifetime_count, depth, node_budget)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_default_type_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index, CmHirTypeId type_id,
    uint32_t binder_lifetime_count, size_t depth, size_t *node_budget)
{
    const CmHirType *type;
    uint32_t index;

    if (node_budget == NULL || *node_budget == 0u
        || depth > CM_HIR_DEFAULT_MAX_DEPTH) return 0;
    *node_budget -= 1u;
    type = cm_hir_get_type(context, type_id);
    if (type == NULL) return 0;
    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
        return 1;
    case CM_HIR_TYPE_SELF_KIND:
        return cm_hir_def_id_equal(type->data.self_type.owner, owner);
    case CM_HIR_TYPE_REFERENCE_KIND:
        return cm_hir_default_region_in_scope(context, owner,
                parameter_index, &type->data.reference_type.region,
                binder_lifetime_count)
            && cm_hir_default_type_in_scope(context, owner,
                parameter_index, type->data.reference_type.pointee,
                binder_lifetime_count, depth + 1u, node_budget);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return cm_hir_default_type_in_scope(context, owner,
            parameter_index, type->data.raw_pointer_type.pointee,
            binder_lifetime_count, depth + 1u, node_budget);
    case CM_HIR_TYPE_TUPLE_KIND:
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!cm_hir_default_type_in_scope(context, owner,
                    parameter_index, type->data.tuple_type.elements[index],
                    binder_lifetime_count, depth + 1u, node_budget)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return cm_hir_default_type_in_scope(context, owner,
                parameter_index, type->data.array_type.element,
                binder_lifetime_count, depth + 1u, node_budget)
            && cm_hir_default_const_in_scope(context, owner,
                parameter_index, &type->data.array_type.length,
                binder_lifetime_count, depth + 1u, node_budget);
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_default_type_in_scope(context, owner,
            parameter_index, type->data.slice_type.element,
            binder_lifetime_count, depth + 1u, node_budget);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!cm_hir_default_type_in_scope(context, owner,
                    parameter_index,
                    type->data.fn_pointer_type.parameters[index],
                    type->data.fn_pointer_type.binder.lifetime_count,
                    depth + 1u, node_budget)) {
                return 0;
            }
        }
        return cm_hir_default_type_in_scope(context, owner,
            parameter_index, type->data.fn_pointer_type.return_type,
            type->data.fn_pointer_type.binder.lifetime_count, depth + 1u,
            node_budget);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
        return cm_hir_default_named_in_scope(context, owner,
            parameter_index, &type->data.named_type,
            binder_lifetime_count, depth, node_budget);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return 0;
    case CM_HIR_TYPE_PARAMETER_KIND:
        return cm_hir_default_parameter_in_scope(context, owner,
            parameter_index, type->data.parameter_type.parameter);
    case CM_HIR_TYPE_PROJECTION_KIND:
        return cm_hir_default_type_in_scope(context, owner,
                parameter_index, type->data.projection_type.self_type,
                binder_lifetime_count, depth + 1u, node_budget)
            && cm_hir_default_named_in_scope(context, owner,
                parameter_index,
                &type->data.projection_type.trait_type,
                binder_lifetime_count, depth, node_budget)
            && cm_hir_default_named_in_scope(context, owner,
                parameter_index,
                &type->data.projection_type.associated_type,
                binder_lifetime_count, depth, node_budget);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if ((type->data.dyn_trait_type.has_principal
                && !cm_hir_default_named_in_scope(context, owner,
                    parameter_index,
                    &type->data.dyn_trait_type.principal_trait,
                    binder_lifetime_count, depth, node_budget))
            || !cm_hir_default_region_in_scope(context, owner,
                parameter_index, &type->data.dyn_trait_type.region,
                binder_lifetime_count)) {
            return 0;
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!cm_hir_default_named_in_scope(context, owner,
                    parameter_index,
                    &type->data.dyn_trait_type.auto_traits[index],
                    binder_lifetime_count, depth, node_budget)) {
                return 0;
            }
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index) {
            if (!cm_hir_default_type_in_scope(context, owner,
                    parameter_index,
                    type->data.dyn_trait_type.equalities[index].value,
                    binder_lifetime_count, depth + 1u, node_budget)) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int cm_hir_default_argument_in_scope(const CmHirContext *context,
    CmHirDefId owner, uint32_t parameter_index,
    const CmHirGenericArg *argument, uint32_t binder_lifetime_count,
    size_t depth, size_t *node_budget)
{
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        return cm_hir_default_region_in_scope(context, owner,
            parameter_index, &argument->data.lifetime,
            binder_lifetime_count);
    case CM_HIR_GENERIC_ARG_TYPE:
        return cm_hir_default_type_in_scope(context, owner,
            parameter_index, argument->data.type,
            binder_lifetime_count, depth + 1u, node_budget);
    case CM_HIR_GENERIC_ARG_CONST:
        return cm_hir_default_const_in_scope(context, owner,
            parameter_index, &argument->data.constant,
            binder_lifetime_count, depth + 1u, node_budget);
    }
    return 0;
}

CmHirStatus cm_hir_add_generic_param(CmHirContext *context,
    const CmHirGenericParam *parameter, CmHirGenericParamId *out_id)
{
    size_t index;

    if (context == NULL || parameter == NULL || out_id == NULL
        || !cm_hir_intern_id_valid(context, parameter->name)
        || (unsigned int)parameter->kind >
            (unsigned int)CM_HIR_GENERIC_CONST
        || (parameter->is_relaxed_sized != 0
            && parameter->is_relaxed_sized != 1)
        || (parameter->is_relaxed_sized
            && parameter->kind != CM_HIR_GENERIC_TYPE)
        || parameter->has_default != 0
        || !cm_hir_span_is_ordered(parameter->span)
        || cm_hir_lookup_definition(context, parameter->owner) == NULL
        || cm_hir_lookup_definition(context,
            parameter->owner)->kind != CM_HIR_DEFINITION_ITEM) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_GENERIC_PARAM_NONE;
    if (parameter->kind == CM_HIR_GENERIC_CONST) {
        /*
         * The declared type may arrive after the fact when a decoder
         * restores types following parameters; it must be attached with
         * cm_hir_set_generic_param_declared_type before the parameter is
         * used.
         */
        if (parameter->declared_type != CM_HIR_TYPE_NONE
            && !cm_hir_type_id_valid(context, parameter->declared_type)) {
            return CM_HIR_INVALID_ID;
        }
    } else {
        if (parameter->declared_type != CM_HIR_TYPE_NONE) {
            return CM_HIR_INVALID_ARGUMENT;
        }
    }
    for (index = 0u; index < context->generic_parameters.len; ++index) {
        const CmHirGenericParam *old_parameter;

        old_parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &context->generic_parameters, index);
        if (cm_hir_def_id_equal(old_parameter->owner, parameter->owner)
            && old_parameter->index == parameter->index) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (cm_hir_def_id_equal(old_parameter->owner, parameter->owner)
            && old_parameter->has_default) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    return cm_hir_push(context, &context->generic_parameters, parameter,
        out_id);
}

CmHirStatus cm_hir_set_generic_param_declared_type(
    CmHirContext *context, CmHirGenericParamId parameter_id,
    CmHirTypeId type)
{
    CmHirGenericParam *parameter;

    if (context == NULL || parameter_id == CM_HIR_GENERIC_PARAM_NONE
        || type == CM_HIR_TYPE_NONE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    if (!cm_hir_type_id_valid(context, type)) return CM_HIR_INVALID_ID;
    parameter = (CmHirGenericParam *)cm_vec_at(&context->generic_parameters,
        (size_t)parameter_id - 1u);
    if (parameter == NULL) return CM_HIR_INVALID_ID;
    if (parameter->kind != CM_HIR_GENERIC_CONST) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    if (parameter->declared_type != CM_HIR_TYPE_NONE) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    parameter->declared_type = type;
    return CM_HIR_OK;
}

CmHirStatus cm_hir_set_generic_param_default(CmHirContext *context,
    CmHirGenericParamId parameter_id, const CmHirGenericArg *argument)
{
    CmHirGenericParam *parameter;
    const CmHirDefinition *owner;
    const CmHirGenericParam *referenced_parameter;
    CmHirGenericArgKind expected_kind;
    size_t node_budget;

    if (context == NULL || argument == NULL
        || parameter_id == CM_HIR_GENERIC_PARAM_NONE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    parameter = (CmHirGenericParam *)cm_vec_at(&context->generic_parameters,
        (size_t)parameter_id - 1u);
    if (parameter == NULL) return CM_HIR_INVALID_ID;
    owner = cm_hir_lookup_definition(context, parameter->owner);
    if (owner == NULL || owner->kind != CM_HIR_DEFINITION_ITEM) {
        return CM_HIR_INVALID_ID;
    }
    if (owner->state != CM_HIR_DEFINITION_RESERVED) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    if (parameter->has_default) return CM_HIR_INVARIANT_VIOLATION;
    if (parameter->kind == CM_HIR_GENERIC_LIFETIME) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    expected_kind = parameter->kind == CM_HIR_GENERIC_TYPE
        ? CM_HIR_GENERIC_ARG_TYPE
        : CM_HIR_GENERIC_ARG_CONST;
    if (argument->kind != expected_kind) return CM_HIR_INVALID_ARGUMENT;
    if (!cm_hir_generic_arg_valid(context, argument)) {
        return CM_HIR_INVALID_ID;
    }
    if (argument->kind == CM_HIR_GENERIC_ARG_CONST) {
        if (!cm_hir_body_type_equal(context,
                argument->data.constant.type, parameter->declared_type)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (argument->data.constant.kind == CM_HIR_CONST_PARAMETER) {
            referenced_parameter = cm_hir_get_generic_param(context,
                argument->data.constant.data.parameter);
            if (referenced_parameter == NULL
                || referenced_parameter->kind != CM_HIR_GENERIC_CONST
                || !cm_hir_body_type_equal(context,
                    argument->data.constant.type,
                    referenced_parameter->declared_type)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
        }
    }
    node_budget = CM_HIR_DEFAULT_NODE_LIMIT;
    if (!cm_hir_default_argument_in_scope(context, parameter->owner,
            parameter->index, argument, 0u, 0u, &node_budget)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    parameter->has_default = 1;
    parameter->default_argument = *argument;
    cm_hir_context_record_semantic_mutation(context);
    return CM_HIR_OK;
}

static void cm_hir_claim_expression_tree(CmHirContext *context,
    CmHirExprId expression_id, CmHirBodyId body_id);

CmHirBodyOrigin cm_hir_body_origin_item_source(CmHirDefId definition)
{
    CmHirBodyOrigin origin;

    memset(&origin, 0, sizeof(origin));
    origin.kind = CM_HIR_BODY_ORIGIN_ITEM_SOURCE;
    origin.definition = definition;
    origin.enclosing_definition = definition;
    origin.data.item_source.item_definition = definition;
    return origin;
}

CmHirBodyOrigin cm_hir_body_origin_metadata_recipe(CmHirDefId definition,
    const unsigned char artifact_identity[CM_HIR_ARTIFACT_IDENTITY_SIZE],
    uint32_t recipe_index, uint32_t argument_index)
{
    CmHirBodyOrigin origin;

    memset(&origin, 0, sizeof(origin));
    origin.kind = CM_HIR_BODY_ORIGIN_METADATA_RECIPE;
    origin.definition = definition;
    origin.enclosing_definition = definition;
    origin.data.metadata_recipe.item_definition = definition;
    if (artifact_identity != NULL) {
        memcpy(origin.data.metadata_recipe.artifact_identity,
            artifact_identity, CM_HIR_ARTIFACT_IDENTITY_SIZE);
    }
    origin.data.metadata_recipe.recipe_index = recipe_index;
    origin.data.metadata_recipe.argument_index = argument_index;
    return origin;
}

static int cm_hir_artifact_identity_nonzero(
    const unsigned char identity[CM_HIR_ARTIFACT_IDENTITY_SIZE])
{
    size_t index;

    for (index = 0u; index < CM_HIR_ARTIFACT_IDENTITY_SIZE; ++index) {
        if (identity[index] != 0u) return 1;
    }
    return 0;
}

static int cm_hir_body_origin_valid(const CmHirBody *body)
{
    if (body == NULL
        || !cm_hir_def_id_equal(body->origin.definition, body->owner)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            body->owner)) return 0;
    if (body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE) {
        return cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, body->owner);
    }
    if (body->origin.kind == CM_HIR_BODY_ORIGIN_METADATA_RECIPE) {
        return cm_hir_def_id_equal(
                body->origin.data.metadata_recipe.item_definition,
                body->owner)
            && cm_hir_artifact_identity_nonzero(
                body->origin.data.metadata_recipe.artifact_identity);
    }
    return 0;
}

CmHirStatus cm_hir_add_body(CmHirContext *context, const CmHirBody *body,
    CmHirBodyId *out_id)
{
    CmHirBody copy;
    uint32_t index;
    size_t body_index;
    const CmHirDefinition *owner_definition;
    const CmHirExpr *typed_root;
    CmHirStatus status;

    if (context == NULL || body == NULL || out_id == NULL) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    owner_definition = cm_hir_lookup_definition(context, body->owner);
    if (!cm_hir_span_is_ordered(body->span)
        || !cm_hir_body_origin_valid(body)
        || owner_definition == NULL
        || owner_definition->kind != CM_HIR_DEFINITION_ITEM
        || !cm_hir_type_id_valid(context, body->expected_type)
        || (body->local_count != 0u && body->locals == NULL)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_BODY_NONE;
    typed_root = body->state == CM_HIR_BODY_TYPED
        ? cm_hir_get_expr(context, body->root_expression) : NULL;
    if ((body->state == CM_HIR_BODY_UNLOWERED
            && (body->source == 0u
                || (body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
                    ? body->source_expression_id == 0u
                    : body->source_expression_id != 0u)
                || body->span.source != body->source
                || body->root_expression != CM_HIR_EXPR_NONE
                || body->error_reason != CM_INTERN_ID_NONE))
        || (body->state == CM_HIR_BODY_TYPED
            && (body->source == 0u
                || (body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
                    ? body->source_expression_id == 0u
                    : body->source_expression_id != 0u)
                || body->span.source != body->source
                || typed_root == NULL
                || typed_root->owner_body != CM_HIR_BODY_NONE
                || typed_root->type != body->expected_type
                || typed_root->span.source != body->source
                || typed_root->span.start < body->span.start
                || typed_root->span.end > body->span.end
                || body->error_reason != CM_INTERN_ID_NONE))
        || (body->state == CM_HIR_BODY_ERROR
            && (!cm_hir_intern_id_valid(context, body->error_reason)
                || body->source != 0u
                || body->source_expression_id != 0u
                || body->root_expression != CM_HIR_EXPR_NONE))
        || (unsigned int)body->state >
            (unsigned int)CM_HIR_BODY_ERROR) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    {
        uint32_t previous_parameter;
        uint32_t previous_binding;
        int saw_parameter;
        int saw_non_parameter;

        previous_parameter = 0u;
        previous_binding = 0u;
        saw_parameter = 0;
        saw_non_parameter = 0;
        for (index = 0u; index < body->local_count; ++index) {
            uint32_t parameter_index;
            uint32_t binding_index;

            parameter_index = body->locals[index].parameter_index;
            binding_index = body->locals[index].parameter_binding_index;
            if (parameter_index == CM_HIR_PARAMETER_INDEX_NONE) {
                if (binding_index != 0u) {
                    return CM_HIR_INVARIANT_VIOLATION;
                }
                saw_non_parameter = 1;
            } else if (parameter_index >= body->parameter_count
                || binding_index
                    >= CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                || saw_non_parameter
                || (saw_parameter
                    && (parameter_index < previous_parameter
                        || (parameter_index == previous_parameter
                            && binding_index <= previous_binding)))) {
                return CM_HIR_INVARIANT_VIOLATION;
            } else {
                previous_parameter = parameter_index;
                previous_binding = binding_index;
                saw_parameter = 1;
            }
        }
    }
    for (index = 0u; index < body->local_count; ++index) {
        if (!cm_hir_intern_id_nonempty(context, body->locals[index].name)
            || cm_hir_intern_matches(context, body->locals[index].name, "_")
            || !cm_hir_type_id_valid(context, body->locals[index].type)
            || (unsigned int)body->locals[index].mutability >
                (unsigned int)CM_HIR_MUTABLE
            || !cm_hir_span_is_ordered(body->locals[index].span)) {
            return CM_HIR_INVALID_ID;
        }
    }
    for (body_index = 0u; body_index < context->bodies.len; ++body_index) {
        const CmHirBody *old_body;

        old_body = (const CmHirBody *)cm_vec_at_const(&context->bodies,
            body_index);
        if (cm_hir_def_id_equal(old_body->owner, body->owner)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    copy = *body;
    copy.locals = (CmHirLocal *)cm_hir_copy_array(context, body->locals,
        body->local_count, sizeof(CmHirLocal));
    status = cm_hir_push(context, &context->bodies, &copy, out_id);
    if (status == CM_HIR_OK && body->state == CM_HIR_BODY_TYPED) {
        cm_hir_claim_expression_tree(context, body->root_expression,
            *out_id);
    }
    return status;
}

CmHirStatus cm_hir_reserve_closure(CmHirContext *context,
    CmHirBodyId owner_body, uint32_t source_expression_id,
    const CmHirClosureParam *parameters, uint32_t parameter_count,
    CmHirTypeId return_type, uint32_t visible_local_count, int is_move,
    CmSpan span, CmHirClosureId *out_id)
{
    const CmHirBody *body;
    CmHirClosure copy;
    uint32_t index;
    size_t old_index;

    if (context == NULL || out_id == NULL || owner_body == CM_HIR_BODY_NONE
        || source_expression_id == 0u
        || (parameter_count == 0u) != (parameters == NULL)
        || (is_move != 0 && is_move != 1)
        || !cm_hir_span_is_ordered(span)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_CLOSURE_NONE;
    body = cm_hir_get_body(context, owner_body);
    if (body == NULL || !cm_hir_type_id_valid(context, return_type)) {
        return CM_HIR_INVALID_ID;
    }
    if (body->state != CM_HIR_BODY_UNLOWERED
        || visible_local_count > body->local_count
        || !cm_hir_expression_body_span_valid(body, span)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    for (index = 0u; index < parameter_count; ++index) {
        const CmHirClosureParam *parameter;
        uint32_t previous;
        int binding_valid;

        parameter = &parameters[index];
        binding_valid = parameter->binding_kind == CM_HIR_BINDING_NAMED
            ? cm_hir_intern_id_nonempty(context, parameter->name)
                && !cm_hir_intern_matches(context, parameter->name, "_")
            : parameter->binding_kind == CM_HIR_BINDING_DISCARD
                && parameter->name == CM_INTERN_ID_NONE;
        if (!binding_valid
            || !cm_hir_type_id_valid(context, parameter->type)
            || !cm_hir_span_is_ordered(parameter->span)) {
            return CM_HIR_INVALID_ID;
        }
        if (parameter->span.source != span.source
            || parameter->span.start < span.start
            || parameter->span.end > span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) continue;
        for (previous = 0u; previous < index; ++previous) {
            if (parameters[previous].binding_kind == CM_HIR_BINDING_NAMED
                && parameters[previous].name == parameter->name) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
        }
    }
    for (old_index = 0u; old_index < context->closures.len; ++old_index) {
        const CmHirClosure *old_closure;

        old_closure = (const CmHirClosure *)cm_vec_at_const(
            &context->closures, old_index);
        if (old_closure->owner_body == owner_body
            && old_closure->source_expression_id == source_expression_id) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
    }
    memset(&copy, 0, sizeof(copy));
    copy.state = CM_HIR_CLOSURE_SIGNATURE_RESERVED;
    copy.owner_body = owner_body;
    copy.source_expression_id = source_expression_id;
    copy.parameters = (CmHirClosureParam *)cm_hir_copy_array(context,
        parameters, parameter_count, sizeof(CmHirClosureParam));
    copy.parameter_count = parameter_count;
    copy.return_type = return_type;
    copy.body_expression = CM_HIR_EXPR_NONE;
    copy.visible_local_count = visible_local_count;
    copy.is_move = is_move;
    copy.capture_state = CM_HIR_CLOSURE_CAPTURES_UNMARKED;
    copy.callable_class = CM_HIR_CLOSURE_CLASS_UNKNOWN;
    copy.span = span;
    return cm_hir_push(context, &context->closures, &copy, out_id);
}

static int cm_hir_expression_body_span_valid(const CmHirBody *body,
    CmSpan span)
{
    return body != NULL && span.source == body->source
        && span.start >= body->span.start && span.end <= body->span.end;
}

/* Structural equality for the deliberately admitted body-expression leaves. */
static int cm_hir_body_type_equal(const CmHirContext *context,
    CmHirTypeId left_id, CmHirTypeId right_id)
{
    const CmHirType *left;
    const CmHirType *right;

    if (left_id == right_id) return 1;
    left = cm_hir_get_type(context, left_id);
    right = cm_hir_get_type(context, right_id);
    if (left == NULL || right == NULL || left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return left->data.integer_type.kind == right->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return left->data.float_type.kind == right->data.float_type.kind;
    case CM_HIR_TYPE_PARAMETER_KIND:
        return left->data.parameter_type.parameter
            == right->data.parameter_type.parameter;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return left->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && right->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && left->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            && right->data.reference_type.region.kind
                == CM_HIR_REGION_ERASED
            && cm_hir_body_type_equal(context,
                left->data.reference_type.pointee,
                right->data.reference_type.pointee);
    case CM_HIR_TYPE_ADT_KIND:
        return left->data.named_type.argument_count == 0u
            && left->data.named_type.arguments == NULL
            && right->data.named_type.argument_count == 0u
            && right->data.named_type.arguments == NULL
            && cm_hir_def_id_equal(left->data.named_type.definition,
                right->data.named_type.definition);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return left->data.closure_type.closure
            == right->data.closure_type.closure;
    default:
        return 0;
    }
}

static int cm_hir_expression_locals_visible(const CmHirContext *context,
    CmHirExprId expression_id, CmHirBodyId body_id,
    uint32_t visible_local_count, size_t depth)
{
    const CmHirExpr *expression;
    uint32_t index;

    if (depth >= context->expressions.len) return 0;
    expression = cm_hir_get_expr(context, expression_id);
    if (expression == NULL || expression->owner_body != body_id) return 0;
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
        return 1;
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
    case CM_HIR_EXPR_CLOSURE:
        /* Strict lexical authentication occurs when a closure/body is bound. */
        return 1;
    case CM_HIR_EXPR_LOCAL:
        return expression->data.local.local_index < visible_local_count;
    case CM_HIR_EXPR_CALL:
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_hir_expression_locals_visible(context,
                    expression->data.call.arguments[index], body_id,
                    visible_local_count, depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_EXPR_METHOD_CALL:
        if (!cm_hir_expression_locals_visible(context,
                expression->data.method_call.receiver, body_id,
                visible_local_count, depth + 1u)) return 0;
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            if (!cm_hir_expression_locals_visible(context,
                    expression->data.method_call.arguments[index], body_id,
                    visible_local_count, depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_EXPR_QUALIFIED_CALL:
        for (index = 0u;
             index < expression->data.qualified_call.argument_count;
             ++index) {
            if (!cm_hir_expression_locals_visible(context,
                    expression->data.qualified_call.arguments[index],
                    body_id, visible_local_count, depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_EXPR_BINARY:
        return cm_hir_expression_locals_visible(context,
                expression->data.binary.left, body_id,
                visible_local_count, depth + 1u)
            && cm_hir_expression_locals_visible(context,
                expression->data.binary.right, body_id,
                visible_local_count, depth + 1u);
    case CM_HIR_EXPR_AGGREGATE:
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            if (!cm_hir_expression_locals_visible(context,
                    expression->data.aggregate.fields[index].value,
                    body_id, visible_local_count, depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_EXPR_FIELD:
        return cm_hir_expression_locals_visible(context,
            expression->data.field.base, body_id, visible_local_count,
            depth + 1u);
    case CM_HIR_EXPR_BORROW_SHARED:
        return cm_hir_expression_locals_visible(context,
            expression->data.borrow_shared.operand, body_id,
            visible_local_count, depth + 1u);
    case CM_HIR_EXPR_DEREFERENCE:
        return cm_hir_expression_locals_visible(context,
            expression->data.dereference.operand, body_id,
            visible_local_count, depth + 1u);
    case CM_HIR_EXPR_IF:
        return cm_hir_expression_locals_visible(context,
                expression->data.if_expr.condition, body_id,
                visible_local_count, depth + 1u)
            && cm_hir_expression_locals_visible(context,
                expression->data.if_expr.then_expression, body_id,
                visible_local_count, depth + 1u)
            && cm_hir_expression_locals_visible(context,
                expression->data.if_expr.else_expression, body_id,
                visible_local_count, depth + 1u);
    case CM_HIR_EXPR_BLOCK:
    {
        uint32_t nested_visible;

        nested_visible = visible_local_count;
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;

            statement = &expression->data.block.statements[index];
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.local_index
                    != nested_visible
                || !cm_hir_expression_locals_visible(context,
                    statement->data.let_statement.initializer, body_id,
                    nested_visible, depth + 1u)
                || nested_visible == UINT32_MAX) {
                return 0;
            }
            ++nested_visible;
        }
        return cm_hir_expression_locals_visible(context,
            expression->data.block.tail_expression, body_id,
            nested_visible, depth + 1u);
    }
    }
    return 0;
}

static int cm_hir_expression_scope_valid(const CmHirContext *context,
    CmHirExprId expression_id, CmHirBodyId body_id,
    uint32_t visible_local_count, CmHirClosureId active_closure,
    size_t depth)
{
    const CmHirExpr *expression;
    uint32_t index;

    if (depth >= context->expressions.len) return 0;
    expression = cm_hir_get_expr(context, expression_id);
    if (expression == NULL || expression->owner_body != body_id) return 0;
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
        return 1;
    case CM_HIR_EXPR_LOCAL:
        return expression->data.local.local_index < visible_local_count;
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
        return active_closure != CM_HIR_CLOSURE_NONE
            && expression->data.closure_parameter.closure == active_closure;
    case CM_HIR_EXPR_CLOSURE:
    {
        const CmHirClosure *closure;

        closure = cm_hir_get_closure(context,
            expression->data.closure.closure);
        return closure != NULL && closure->state == CM_HIR_CLOSURE_BODY_BOUND
            && closure->owner_body == body_id
            && cm_hir_expression_scope_valid(context,
                closure->body_expression, body_id,
                closure->visible_local_count,
                expression->data.closure.closure, depth + 1u);
    }
    case CM_HIR_EXPR_CALL:
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (!cm_hir_expression_scope_valid(context,
                    expression->data.call.arguments[index], body_id,
                    visible_local_count, active_closure, depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_EXPR_METHOD_CALL:
        if (!cm_hir_expression_scope_valid(context,
                expression->data.method_call.receiver, body_id,
                visible_local_count, active_closure, depth + 1u)) return 0;
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            if (!cm_hir_expression_scope_valid(context,
                    expression->data.method_call.arguments[index], body_id,
                    visible_local_count, active_closure, depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_EXPR_QUALIFIED_CALL:
        for (index = 0u;
             index < expression->data.qualified_call.argument_count;
             ++index) {
            if (!cm_hir_expression_scope_valid(context,
                    expression->data.qualified_call.arguments[index],
                    body_id, visible_local_count, active_closure,
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_EXPR_BINARY:
        return cm_hir_expression_scope_valid(context,
                expression->data.binary.left, body_id, visible_local_count,
                active_closure, depth + 1u)
            && cm_hir_expression_scope_valid(context,
                expression->data.binary.right, body_id, visible_local_count,
                active_closure, depth + 1u);
    case CM_HIR_EXPR_AGGREGATE:
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            if (!cm_hir_expression_scope_valid(context,
                    expression->data.aggregate.fields[index].value,
                    body_id, visible_local_count, active_closure,
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_EXPR_FIELD:
        return cm_hir_expression_scope_valid(context,
            expression->data.field.base, body_id, visible_local_count,
            active_closure, depth + 1u);
    case CM_HIR_EXPR_BORROW_SHARED:
        return cm_hir_expression_scope_valid(context,
            expression->data.borrow_shared.operand, body_id,
            visible_local_count, active_closure, depth + 1u);
    case CM_HIR_EXPR_DEREFERENCE:
        return cm_hir_expression_scope_valid(context,
            expression->data.dereference.operand, body_id,
            visible_local_count, active_closure, depth + 1u);
    case CM_HIR_EXPR_IF:
        return cm_hir_expression_scope_valid(context,
                expression->data.if_expr.condition, body_id,
                visible_local_count, active_closure, depth + 1u)
            && cm_hir_expression_scope_valid(context,
                expression->data.if_expr.then_expression, body_id,
                visible_local_count, active_closure, depth + 1u)
            && cm_hir_expression_scope_valid(context,
                expression->data.if_expr.else_expression, body_id,
                visible_local_count, active_closure, depth + 1u);
    case CM_HIR_EXPR_BLOCK:
    {
        uint32_t nested_visible;

        nested_visible = visible_local_count;
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;

            statement = &expression->data.block.statements[index];
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.local_index
                    != nested_visible
                || !cm_hir_expression_scope_valid(context,
                    statement->data.let_statement.initializer, body_id,
                    nested_visible, active_closure, depth + 1u)
                || nested_visible == UINT32_MAX) return 0;
            ++nested_visible;
        }
        return cm_hir_expression_scope_valid(context,
            expression->data.block.tail_expression, body_id,
            nested_visible, active_closure, depth + 1u);
    }
    }
    return 0;
}

CmHirStatus cm_hir_bind_closure_body(CmHirContext *context,
    CmHirClosureId closure_id, CmHirExprId body_expression)
{
    CmHirClosure *closure;
    const CmHirExpr *root;

    if (context == NULL || closure_id == CM_HIR_CLOSURE_NONE
        || body_expression == CM_HIR_EXPR_NONE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    closure = (CmHirClosure *)cm_hir_get_id_mut(&context->closures,
        closure_id);
    root = cm_hir_get_expr(context, body_expression);
    if (closure == NULL || root == NULL) return CM_HIR_INVALID_ID;
    if (closure->state != CM_HIR_CLOSURE_SIGNATURE_RESERVED
        || closure->body_expression != CM_HIR_EXPR_NONE
        || closure->capture_state != CM_HIR_CLOSURE_CAPTURES_UNMARKED
        || closure->captures != NULL || closure->capture_count != 0u
        || closure->callable_class != CM_HIR_CLOSURE_CLASS_UNKNOWN
        || closure->is_copy != 0
        || root->owner_body != closure->owner_body
        || !cm_hir_body_type_equal(context, root->type,
            closure->return_type)
        || root->span.source != closure->span.source
        || root->span.start < closure->span.start
        || root->span.end > closure->span.end
        || !cm_hir_expression_scope_valid(context, body_expression,
            closure->owner_body, closure->visible_local_count,
            closure_id, 0u)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    closure->body_expression = body_expression;
    closure->state = CM_HIR_CLOSURE_BODY_BOUND;
    cm_hir_context_record_semantic_mutation(context);
    return CM_HIR_OK;
}

static int cm_hir_call_region_matches(const CmHirRegion *declared,
    const CmHirRegion *actual)
{
    if (declared == NULL || actual == NULL
        || declared->kind != actual->kind) return 0;
    return declared->kind == CM_HIR_REGION_STATIC
        || declared->kind == CM_HIR_REGION_ERASED;
}

static int cm_hir_call_type_matches(const CmHirContext *context,
    const CmHirItem *callee, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirTypeId declared,
    CmHirTypeId actual, size_t depth, int substitute_parameters)
{
    const CmHirType *declared_type;
    const CmHirType *actual_type;
    const CmHirGenericParam *parameter;
    uint32_t index;

    if (context == NULL || callee == NULL
        || depth > context->types.len) return 0;
    declared_type = cm_hir_get_type(context, declared);
    actual_type = cm_hir_get_type(context, actual);
    if (declared_type == NULL || actual_type == NULL) return 0;
    if (declared_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        if (!substitute_parameters) {
            return actual_type->kind == CM_HIR_TYPE_PARAMETER_KIND
                && declared_type->data.parameter_type.parameter
                    == actual_type->data.parameter_type.parameter;
        }
        parameter = cm_hir_get_generic_param(context,
            declared_type->data.parameter_type.parameter);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(parameter->owner, callee->definition)
            || parameter->index >= substitution_count) {
            return 0;
        }
        return cm_hir_call_type_matches(context, callee, substitutions,
            substitution_count, substitutions[parameter->index], actual,
            depth + 1u, 0);
    }
    if (declared_type->kind != actual_type->kind) return 0;
    switch (declared_type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return declared_type->data.integer_type.kind
            == actual_type->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return declared_type->data.float_type.kind
            == actual_type->data.float_type.kind;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return declared_type->data.reference_type.mutability
                == actual_type->data.reference_type.mutability
            && cm_hir_call_region_matches(
                &declared_type->data.reference_type.region,
                &actual_type->data.reference_type.region)
            && cm_hir_call_type_matches(context, callee, substitutions,
                substitution_count,
                declared_type->data.reference_type.pointee,
                actual_type->data.reference_type.pointee, depth + 1u,
                substitute_parameters);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return declared_type->data.raw_pointer_type.mutability
                == actual_type->data.raw_pointer_type.mutability
            && cm_hir_call_type_matches(context, callee, substitutions,
                substitution_count,
                declared_type->data.raw_pointer_type.pointee,
                actual_type->data.raw_pointer_type.pointee, depth + 1u,
                substitute_parameters);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (declared_type->data.tuple_type.element_count
                != actual_type->data.tuple_type.element_count
            || (declared_type->data.tuple_type.element_count == 0u)
                != (declared_type->data.tuple_type.elements == NULL)
            || (actual_type->data.tuple_type.element_count == 0u)
                != (actual_type->data.tuple_type.elements == NULL)) {
            return 0;
        }
        for (index = 0u;
             index < declared_type->data.tuple_type.element_count; ++index) {
            if (!cm_hir_call_type_matches(context, callee, substitutions,
                    substitution_count,
                    declared_type->data.tuple_type.elements[index],
                    actual_type->data.tuple_type.elements[index],
                    depth + 1u, substitute_parameters)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_SLICE_KIND:
        return cm_hir_call_type_matches(context, callee, substitutions,
            substitution_count, declared_type->data.slice_type.element,
            actual_type->data.slice_type.element, depth + 1u,
            substitute_parameters);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (declared_type->data.fn_pointer_type.parameter_count
                != actual_type->data.fn_pointer_type.parameter_count
            || declared_type->data.fn_pointer_type.binder.lifetime_count
                != actual_type->data.fn_pointer_type.binder.lifetime_count
            || declared_type->data.fn_pointer_type.abi
                != actual_type->data.fn_pointer_type.abi
            || declared_type->data.fn_pointer_type.safety
                != actual_type->data.fn_pointer_type.safety
            || declared_type->data.fn_pointer_type.is_variadic
                != actual_type->data.fn_pointer_type.is_variadic
            || (declared_type->data.fn_pointer_type.parameter_count == 0u)
                != (declared_type->data.fn_pointer_type.parameters == NULL)
            || (actual_type->data.fn_pointer_type.parameter_count == 0u)
                != (actual_type->data.fn_pointer_type.parameters == NULL)
            || !cm_hir_call_type_matches(context, callee, substitutions,
                substitution_count,
                declared_type->data.fn_pointer_type.return_type,
                actual_type->data.fn_pointer_type.return_type,
                depth + 1u, substitute_parameters)) return 0;
        for (index = 0u; index
                < declared_type->data.fn_pointer_type.parameter_count;
             ++index) {
            if (!cm_hir_call_type_matches(context, callee, substitutions,
                    substitution_count,
                    declared_type->data.fn_pointer_type.parameters[index],
                    actual_type->data.fn_pointer_type.parameters[index],
                    depth + 1u, substitute_parameters)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ADT_KIND:
        if (!cm_hir_def_id_equal(
                declared_type->data.named_type.definition,
                actual_type->data.named_type.definition)
            || declared_type->data.named_type.argument_count
                != actual_type->data.named_type.argument_count
            || (declared_type->data.named_type.argument_count == 0u)
                != (declared_type->data.named_type.arguments == NULL)
            || (actual_type->data.named_type.argument_count == 0u)
                != (actual_type->data.named_type.arguments == NULL)) {
            return 0;
        }
        for (index = 0u;
             index < declared_type->data.named_type.argument_count; ++index) {
            const CmHirGenericArg *declared_argument;
            const CmHirGenericArg *actual_argument;

            declared_argument = &declared_type->data.named_type.arguments[index];
            actual_argument = &actual_type->data.named_type.arguments[index];
            if (declared_argument->kind != CM_HIR_GENERIC_ARG_TYPE
                || actual_argument->kind != CM_HIR_GENERIC_ARG_TYPE
                || !cm_hir_call_type_matches(context, callee,
                    substitutions, substitution_count,
                    declared_argument->data.type,
                    actual_argument->data.type, depth + 1u,
                    substitute_parameters)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_ARRAY_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
        return 0;
    }
    return 0;
}

static int cm_hir_call_expression_valid(const CmHirContext *context,
    const CmHirExpr *expression)
{
    const CmHirItem *callee;
    const CmHirFunctionSignature *signature;
    uint32_t index;

    callee = cm_hir_bound_definition_item(context,
        expression->data.call.callee);
    if (callee == NULL || callee->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_is_none(callee->parent_definition)
        || !cm_hir_def_id_is_none(
            callee->data.function_item.trait_item_definition)
        || callee->data.function_item.signature.receiver
            != CM_HIR_RECEIVER_NONE
        || callee->generic_parameter_count
            != expression->data.call.type_substitution_count
        || (expression->data.call.type_substitution_count != 0u
            && expression->data.call.type_substitutions == NULL)
        || (expression->data.call.argument_count != 0u
            && expression->data.call.arguments == NULL)) {
        return 0;
    }
    for (index = 0u; index < callee->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            callee->generic_parameter_start + index);
        if (parameter == NULL || parameter->kind != CM_HIR_GENERIC_TYPE
            || parameter->index != index
            || !cm_hir_def_id_equal(parameter->owner,
                callee->definition)
            || !cm_hir_type_id_valid(context,
                expression->data.call.type_substitutions[index])) {
            return 0;
        }
    }
    signature = &callee->data.function_item.signature;
    if (signature->parameter_count != expression->data.call.argument_count
        || !cm_hir_call_type_matches(context, callee,
            expression->data.call.type_substitutions,
            expression->data.call.type_substitution_count,
            signature->return_type, expression->type, 0u, 1)) {
        return 0;
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirExpr *argument;

        argument = cm_hir_get_expr(context,
            expression->data.call.arguments[index]);
        if (argument == NULL
            || argument->owner_body != expression->owner_body
            || argument->span.source != expression->span.source
            || argument->span.start < expression->span.start
            || argument->span.end > expression->span.end
            || !cm_hir_call_type_matches(context, callee,
                expression->data.call.type_substitutions,
                expression->data.call.type_substitution_count,
                signature->parameters[index].type, argument->type, 0u, 1)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_qualified_type_matches(const CmHirContext *context,
    CmHirDefId trait_definition, CmHirTypeId requested_self,
    CmHirTypeId declared, CmHirTypeId actual, size_t depth)
{
    const CmHirType *declared_type;
    const CmHirType *actual_type;
    const CmHirGenericParam *parameter;
    uint32_t index;

    if (context == NULL || depth > context->types.len) return 0;
    declared_type = cm_hir_get_type(context, declared);
    actual_type = cm_hir_get_type(context, actual);
    if (declared_type == NULL || actual_type == NULL) return 0;
    if (declared_type->kind == CM_HIR_TYPE_SELF_KIND) {
        return cm_hir_def_id_equal(declared_type->data.self_type.owner,
                trait_definition)
            && cm_hir_body_type_equal(context, requested_self, actual);
    }
    if (declared_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        /* The concrete trait argument is inferred by semantic checking. */
        parameter = cm_hir_get_generic_param(context,
            declared_type->data.parameter_type.parameter);
        return parameter != NULL
            && cm_hir_def_id_equal(parameter->owner, trait_definition)
            && actual_type->kind != CM_HIR_TYPE_ERROR_KIND;
    }
    if (declared_type->kind != actual_type->kind) return 0;
    switch (declared_type->kind) {
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return declared_type->data.integer_type.kind
            == actual_type->data.integer_type.kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return declared_type->data.float_type.kind
            == actual_type->data.float_type.kind;
    case CM_HIR_TYPE_REFERENCE_KIND:
        return declared_type->data.reference_type.mutability
                == actual_type->data.reference_type.mutability
            && cm_hir_call_region_matches(
                &declared_type->data.reference_type.region,
                &actual_type->data.reference_type.region)
            && cm_hir_qualified_type_matches(context, trait_definition,
                requested_self, declared_type->data.reference_type.pointee,
                actual_type->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return declared_type->data.raw_pointer_type.mutability
                == actual_type->data.raw_pointer_type.mutability
            && cm_hir_qualified_type_matches(context, trait_definition,
                requested_self, declared_type->data.raw_pointer_type.pointee,
                actual_type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (declared_type->data.tuple_type.element_count
                != actual_type->data.tuple_type.element_count
            || (declared_type->data.tuple_type.element_count == 0u)
                != (declared_type->data.tuple_type.elements == NULL)
            || (actual_type->data.tuple_type.element_count == 0u)
                != (actual_type->data.tuple_type.elements == NULL)) {
            return 0;
        }
        for (index = 0u;
             index < declared_type->data.tuple_type.element_count; ++index) {
            if (!cm_hir_qualified_type_matches(context, trait_definition,
                    requested_self,
                    declared_type->data.tuple_type.elements[index],
                    actual_type->data.tuple_type.elements[index],
                    depth + 1u)) return 0;
        }
        return 1;
    case CM_HIR_TYPE_ADT_KIND:
        return declared_type->data.named_type.argument_count == 0u
            && declared_type->data.named_type.arguments == NULL
            && actual_type->data.named_type.argument_count == 0u
            && actual_type->data.named_type.arguments == NULL
            && cm_hir_def_id_equal(
                declared_type->data.named_type.definition,
                actual_type->data.named_type.definition);
    case CM_HIR_TYPE_PARAMETER_KIND:
        return 0;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_ARRAY_KIND:
    case CM_HIR_TYPE_SLICE_KIND:
    case CM_HIR_TYPE_FN_POINTER_KIND:
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PROJECTION_KIND:
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_CLOSURE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return 0;
    }
    return 0;
}

static int cm_hir_qualified_call_expression_valid(
    const CmHirContext *context, const CmHirExpr *expression)
{
    const CmHirBody *body;
    const CmHirItem *trait_item;
    const CmHirItem *declared;
    const CmHirFunctionSignature *signature;
    const CmHirType *self_type;
    uint32_t expected_receiver;
    uint32_t index;

    body = cm_hir_get_body(context, expression->owner_body);
    trait_item = cm_hir_bound_definition_item(context,
        expression->data.qualified_call.requested_trait);
    declared = cm_hir_bound_definition_item(context,
        expression->data.qualified_call.declared_trait_callable);
    self_type = cm_hir_get_type(context,
        expression->data.qualified_call.requested_self_type);
    if (body == NULL
        || expression->data.qualified_call.syntax
            != CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
        || trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
        || trait_item->definition.crate_id != body->owner.crate_id
        || declared == NULL || declared->kind != CM_HIR_ITEM_FUNCTION
        || !cm_hir_def_id_equal(declared->parent_definition,
            trait_item->definition)
        || !cm_hir_def_id_is_none(
            declared->data.function_item.trait_item_definition)
        || declared->generic_parameter_count != 0u
        || declared->predicate_scope_count != 0u
        || declared->predicate_count != 0u
        || declared->outlives_predicate_count != 0u
        || self_type == NULL
        || self_type->kind == CM_HIR_TYPE_ERROR_KIND
        || self_type->kind == CM_HIR_TYPE_INFER_KIND
        || self_type->kind == CM_HIR_TYPE_SELF_KIND
        || self_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        || self_type->kind == CM_HIR_TYPE_PROJECTION_KIND
        || (expression->data.qualified_call.argument_count == 0u)
            != (expression->data.qualified_call.arguments == NULL)) {
        return 0;
    }
    signature = &declared->data.function_item.signature;
    expected_receiver = signature->receiver == CM_HIR_RECEIVER_NONE
        ? CM_HIR_CALLABLE_RECEIVER_NONE : 0u;
    if (signature->parameter_count
            != expression->data.qualified_call.argument_count
        || (signature->parameter_count == 0u)
            != (signature->parameters == NULL)
        || expression->data.qualified_call.receiver_argument
            != expected_receiver
        || !cm_hir_qualified_type_matches(context, trait_item->definition,
            expression->data.qualified_call.requested_self_type,
            signature->return_type, expression->type, 0u)) {
        return 0;
    }
    for (index = 0u; index < signature->parameter_count; ++index) {
        const CmHirExpr *argument;

        argument = cm_hir_get_expr(context,
            expression->data.qualified_call.arguments[index]);
        if (argument == NULL
            || argument->owner_body != expression->owner_body
            || argument->span.source != expression->span.source
            || argument->span.start < expression->span.start
            || argument->span.end > expression->span.end
            || !cm_hir_qualified_type_matches(context,
                trait_item->definition,
                expression->data.qualified_call.requested_self_type,
                signature->parameters[index].type, argument->type, 0u)) {
            return 0;
        }
    }
    return 1;
}

static int cm_hir_method_call_expression_valid(
    const CmHirContext *context, const CmHirExpr *expression)
{
    const CmHirExpr *receiver;
    uint32_t index;
    uint32_t prior_end;

    if (expression->data.method_call.syntax != CM_HIR_CALLABLE_DOT_METHOD
        || !cm_hir_intern_id_nonempty(context,
            expression->data.method_call.method_name)
        || (expression->data.method_call.argument_count == 0u)
            != (expression->data.method_call.arguments == NULL)
        || (expression->data.method_call.in_scope_trait_count == 0u)
            != (expression->data.method_call.in_scope_traits == NULL)) {
        return 0;
    }
    receiver = cm_hir_get_expr(context,
        expression->data.method_call.receiver);
    if (receiver == NULL
        || receiver->owner_body != expression->owner_body
        || receiver->span.source != expression->span.source
        || receiver->span.start != expression->span.start
        || receiver->span.end > expression->span.end) {
        return 0;
    }
    prior_end = receiver->span.end;
    for (index = 0u; index < expression->data.method_call.argument_count;
         ++index) {
        const CmHirExpr *argument;

        argument = cm_hir_get_expr(context,
            expression->data.method_call.arguments[index]);
        if (argument == NULL
            || argument->owner_body != expression->owner_body
            || argument->span.source != expression->span.source
            || argument->span.start < prior_end
            || argument->span.end > expression->span.end) {
            return 0;
        }
        prior_end = argument->span.end;
    }
    for (index = 0u;
         index < expression->data.method_call.in_scope_trait_count;
         ++index) {
        const CmHirItem *trait_item;
        uint32_t prior;

        trait_item = cm_hir_bound_definition_item(context,
            expression->data.method_call.in_scope_traits[index]);
        if (trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (cm_hir_def_id_equal(
                    expression->data.method_call.in_scope_traits[prior],
                    expression->data.method_call.in_scope_traits[index])) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_aggregate_expression_valid(const CmHirContext *context,
    const CmHirExpr *expression, const CmHirType *type,
    const CmHirBody *body)
{
    const CmHirDefinition *body_owner;
    const CmHirItem *aggregate;
    const CmHirModule *module;
    uint32_t index;

    if (body == NULL || type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.argument_count != 0u
        || type->data.named_type.arguments != NULL
        || !cm_hir_def_id_equal(type->data.named_type.definition,
            expression->data.aggregate.definition)
        || (expression->data.aggregate.field_count == 0u)
            != (expression->data.aggregate.fields == NULL)) {
        return 0;
    }
    body_owner = cm_hir_lookup_definition(context, body->owner);
    aggregate = cm_hir_bound_definition_item(context,
        expression->data.aggregate.definition);
    if (body_owner == NULL
        || body_owner->kind != CM_HIR_DEFINITION_ITEM
        || aggregate == NULL || aggregate->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_is_none(aggregate->parent_definition)
        || aggregate->generic_parameter_count != 0u
        || aggregate->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || aggregate->data.aggregate_item.field_count
            != expression->data.aggregate.field_count
        || expression->data.aggregate.definition.crate_id
            != body->owner.crate_id) {
        return 0;
    }
    module = cm_hir_get_module(context, aggregate->owner_module);
    if (module == NULL
        || module->crate_id != expression->data.aggregate.definition.crate_id) {
        return 0;
    }
    for (index = 0u; index < expression->data.aggregate.field_count;
         ++index) {
        const CmHirAggregateFieldValue *field_value;
        const CmHirExpr *value;
        uint32_t prior;

        field_value = &expression->data.aggregate.fields[index];
        value = field_value->field_index
                < aggregate->data.aggregate_item.field_count
            ? cm_hir_get_expr(context, field_value->value) : NULL;
        if (value == NULL || !cm_hir_span_is_ordered(field_value->span)
            || field_value->span.source != expression->span.source
            || field_value->span.start < expression->span.start
            || field_value->span.end > expression->span.end
            || value->owner_body != expression->owner_body
            || value->span.source != field_value->span.source
            || value->span.start < field_value->span.start
            || value->span.end > field_value->span.end
            || !cm_hir_body_type_equal(context, value->type,
                aggregate->data.aggregate_item
                    .fields[field_value->field_index].type)
            || (index != 0u
                && expression->data.aggregate.fields[index - 1u].span.end
                    > field_value->span.start)) {
            return 0;
        }
        for (prior = 0u; prior < index; ++prior) {
            if (expression->data.aggregate.fields[prior].field_index
                    == field_value->field_index) {
                return 0;
            }
        }
    }
    return 1;
}

static int cm_hir_field_expression_valid(const CmHirContext *context,
    const CmHirExpr *expression, const CmHirBody *body)
{
    const CmHirDefinition *body_owner;
    const CmHirExpr *base;
    const CmHirType *base_type;
    const CmHirItem *aggregate;
    const CmHirModule *module;

    base = cm_hir_get_expr(context, expression->data.field.base);
    base_type = base == NULL ? NULL : cm_hir_get_type(context, base->type);
    body_owner = body == NULL ? NULL
        : cm_hir_lookup_definition(context, body->owner);
    aggregate = cm_hir_bound_definition_item(context,
        expression->data.field.definition);
    if (body == NULL || body_owner == NULL
        || body_owner->kind != CM_HIR_DEFINITION_ITEM
        || base == NULL || base->owner_body != expression->owner_body
        || base->span.source != expression->span.source
        || base->span.start != expression->span.start
        || base->span.end >= expression->span.end
        || base_type == NULL || base_type->kind != CM_HIR_TYPE_ADT_KIND
        || base_type->data.named_type.argument_count != 0u
        || base_type->data.named_type.arguments != NULL
        || !cm_hir_def_id_equal(base_type->data.named_type.definition,
            expression->data.field.definition)
        || aggregate == NULL || aggregate->kind != CM_HIR_ITEM_STRUCT
        || !cm_hir_def_id_is_none(aggregate->parent_definition)
        || aggregate->generic_parameter_count != 0u
        || aggregate->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
        || expression->data.field.field_index
            >= aggregate->data.aggregate_item.field_count
        || expression->data.field.definition.crate_id
            != body->owner.crate_id
        || !cm_hir_body_type_equal(context, expression->type,
            aggregate->data.aggregate_item
                .fields[expression->data.field.field_index].type)) {
        return 0;
    }
    module = cm_hir_get_module(context, aggregate->owner_module);
    return module != NULL
        && module->crate_id == expression->data.field.definition.crate_id;
}

static void cm_hir_claim_expression_tree(CmHirContext *context,
    CmHirExprId expression_id, CmHirBodyId body_id)
{
    CmHirExpr *expression;
    uint32_t index;

    expression = (CmHirExpr *)cm_hir_get_id_mut(&context->expressions,
        expression_id);
    if (expression == NULL || expression->owner_body == body_id) return;
    if (expression->kind == CM_HIR_EXPR_BLOCK) {
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            cm_hir_claim_expression_tree(context,
                expression->data.block.statements[index].data.let_statement
                    .initializer,
                body_id);
        }
        cm_hir_claim_expression_tree(context,
            expression->data.block.tail_expression, body_id);
    } else if (expression->kind == CM_HIR_EXPR_CALL) {
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            cm_hir_claim_expression_tree(context,
                expression->data.call.arguments[index], body_id);
        }
    } else if (expression->kind == CM_HIR_EXPR_METHOD_CALL) {
        cm_hir_claim_expression_tree(context,
            expression->data.method_call.receiver, body_id);
        for (index = 0u;
             index < expression->data.method_call.argument_count; ++index) {
            cm_hir_claim_expression_tree(context,
                expression->data.method_call.arguments[index], body_id);
        }
    } else if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        for (index = 0u;
             index < expression->data.qualified_call.argument_count;
             ++index) {
            cm_hir_claim_expression_tree(context,
                expression->data.qualified_call.arguments[index], body_id);
        }
    } else if (expression->kind == CM_HIR_EXPR_BINARY) {
        cm_hir_claim_expression_tree(context, expression->data.binary.left,
            body_id);
        cm_hir_claim_expression_tree(context, expression->data.binary.right,
            body_id);
    } else if (expression->kind == CM_HIR_EXPR_AGGREGATE) {
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            cm_hir_claim_expression_tree(context,
                expression->data.aggregate.fields[index].value, body_id);
        }
    } else if (expression->kind == CM_HIR_EXPR_FIELD) {
        cm_hir_claim_expression_tree(context, expression->data.field.base,
            body_id);
    } else if (expression->kind == CM_HIR_EXPR_BORROW_SHARED) {
        cm_hir_claim_expression_tree(context,
            expression->data.borrow_shared.operand, body_id);
    } else if (expression->kind == CM_HIR_EXPR_DEREFERENCE) {
        cm_hir_claim_expression_tree(context,
            expression->data.dereference.operand, body_id);
    } else if (expression->kind == CM_HIR_EXPR_IF) {
        cm_hir_claim_expression_tree(context,
            expression->data.if_expr.condition, body_id);
        cm_hir_claim_expression_tree(context,
            expression->data.if_expr.then_expression, body_id);
        cm_hir_claim_expression_tree(context,
            expression->data.if_expr.else_expression, body_id);
    } else if (expression->kind == CM_HIR_EXPR_CLOSURE) {
        const CmHirClosure *closure;

        closure = cm_hir_get_closure(context,
            expression->data.closure.closure);
        if (closure != NULL && closure->state == CM_HIR_CLOSURE_BODY_BOUND) {
            cm_hir_claim_expression_tree(context, closure->body_expression,
                body_id);
        }
    }
    expression->owner_body = body_id;
}

CmHirStatus cm_hir_add_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id)
{
    const CmHirType *type;
    const CmHirBody *body;
    CmHirExpr copy;
    size_t reserved_count;
    size_t owned_count;
    size_t owned_bytes;
    uint32_t *owned_ids;

    if (context == NULL || expression == NULL || out_id == NULL) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_EXPR_NONE;
    type = cm_hir_get_type(context, expression->type);
    if (type == NULL || !cm_hir_span_is_ordered(expression->span)
        || expression->usage != CM_HIR_USAGE_UNKNOWN
        || expression->static_borrow_state != CM_HIR_STATIC_BORROW_UNKNOWN
        || (unsigned int)expression->kind >
            (unsigned int)CM_HIR_EXPR_CLOSURE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    body = expression->owner_body == CM_HIR_BODY_NONE ? NULL
        : cm_hir_get_body(context, expression->owner_body);
    if (expression->owner_body != CM_HIR_BODY_NONE
        && (body == NULL || body->state != CM_HIR_BODY_UNLOWERED
            || !cm_hir_expression_body_span_valid(body,
                expression->span))) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    copy = *expression;
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
        if (type->kind != CM_HIR_TYPE_INTEGER_KIND) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    case CM_HIR_EXPR_BLOCK:
    {
        const CmHirExpr *tail;
        uint32_t first_statement_local;
        uint32_t index;

        tail = cm_hir_get_expr(context,
            expression->data.block.tail_expression);
        if ((body == NULL
                && expression->data.block.statement_count != 0u)
            || (expression->data.block.statement_count == 0u)
                != (expression->data.block.statements == NULL)
            || (body != NULL && expression->data.block.statement_count
                > body->local_count)
            || tail == NULL || !cm_hir_body_type_equal(context, tail->type,
                expression->type)
            || tail->owner_body != expression->owner_body
            || tail->span.source != expression->span.source
            || tail->span.start < expression->span.start
            || tail->span.end > expression->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        first_statement_local = body == NULL ? 0u : body->local_count
            - expression->data.block.statement_count;
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;
            const CmHirExpr *initializer;
            uint32_t local_index;
            uint32_t prior_index;

            statement = &expression->data.block.statements[index];
            local_index = first_statement_local + index;
            initializer = statement->kind == CM_HIR_STATEMENT_LET
                ? cm_hir_get_expr(context,
                    statement->data.let_statement.initializer)
                : NULL;
            if (statement->kind != CM_HIR_STATEMENT_LET
                || !cm_hir_span_is_ordered(statement->span)
                || statement->span.source != expression->span.source
                || statement->span.start < expression->span.start
                || statement->span.end > expression->span.end
                || statement->data.let_statement.local_index != local_index
                || body->locals[local_index].parameter_index
                    != CM_HIR_PARAMETER_INDEX_NONE
                || initializer == NULL
                || initializer->owner_body != expression->owner_body
                || !cm_hir_body_type_equal(context, initializer->type,
                    body->locals[local_index].type)
                || initializer->span.source != statement->span.source
                || initializer->span.start < statement->span.start
                || initializer->span.end > statement->span.end
                || !cm_hir_expression_locals_visible(context,
                    statement->data.let_statement.initializer,
                    expression->owner_body, local_index, 0u)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            for (prior_index = 0u; prior_index < local_index;
                 ++prior_index) {
                if (body->locals[prior_index].name
                        == body->locals[local_index].name) {
                    return CM_HIR_INVARIANT_VIOLATION;
                }
            }
        }
        if (body != NULL && !cm_hir_expression_locals_visible(context,
                expression->data.block.tail_expression,
                expression->owner_body, body->local_count, 0u)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        copy.data.block.statements = (CmHirStatement *)cm_hir_copy_array(
            context, expression->data.block.statements,
            expression->data.block.statement_count,
            sizeof(CmHirStatement));
        break;
    }
    case CM_HIR_EXPR_LOCAL:
        if (body == NULL || expression->data.local.local_index
                >= body->local_count
            || !cm_hir_body_type_equal(context,
                body->locals[expression->data.local.local_index].type,
                expression->type)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    case CM_HIR_EXPR_CLOSURE_PARAMETER:
    {
        const CmHirClosure *closure;
        const CmHirClosureParam *parameter;

        closure = cm_hir_get_closure(context,
            expression->data.closure_parameter.closure);
        parameter = closure != NULL
                && expression->data.closure_parameter.parameter_index
                    < closure->parameter_count
            ? &closure->parameters[
                expression->data.closure_parameter.parameter_index]
            : NULL;
        if (body == NULL || closure == NULL || parameter == NULL
            || closure->owner_body != expression->owner_body
            || parameter->binding_kind != CM_HIR_BINDING_NAMED
            || !cm_hir_body_type_equal(context, parameter->type,
                expression->type)
            || expression->span.source != parameter->span.source
            || expression->span.start < parameter->span.start
            || expression->span.end > closure->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    case CM_HIR_EXPR_CLOSURE:
    {
        const CmHirClosure *closure;

        closure = cm_hir_get_closure(context, expression->data.closure.closure);
        if (body == NULL || closure == NULL
            || closure->state != CM_HIR_CLOSURE_BODY_BOUND
            || closure->capture_state
                != CM_HIR_CLOSURE_CAPTURES_UNMARKED
            || closure->captures != NULL || closure->capture_count != 0u
            || closure->callable_class
                != CM_HIR_CLOSURE_CLASS_UNKNOWN
            || closure->is_copy != 0
            || closure->owner_body != expression->owner_body
            || type->kind != CM_HIR_TYPE_CLOSURE_KIND
            || type->data.closure_type.closure
                != expression->data.closure.closure
            || expression->span.source != closure->span.source
            || expression->span.start != closure->span.start
            || expression->span.end != closure->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    case CM_HIR_EXPR_CALL:
        if (body == NULL || !cm_hir_call_expression_valid(context,
                expression)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (context->expressions.len >= (size_t)UINT32_MAX) {
            return CM_HIR_ID_EXHAUSTED;
        }
        if (!cm_size_add(
                (size_t)expression->data.call.type_substitution_count,
                (size_t)expression->data.call.argument_count,
                &owned_count)
            || !cm_size_mul(owned_count, sizeof(uint32_t), &owned_bytes)
            || !cm_size_add(context->expressions.len, 1u,
                &reserved_count)) {
            return CM_HIR_ID_EXHAUSTED;
        }
        cm_vec_reserve(&context->expressions, reserved_count);
        owned_ids = owned_count == 0u ? NULL
            : (uint32_t *)cm_alloc(owned_bytes);
        if (expression->data.call.type_substitution_count != 0u) {
            memcpy(owned_ids, expression->data.call.type_substitutions,
                (size_t)expression->data.call.type_substitution_count
                    * sizeof(uint32_t));
        }
        if (expression->data.call.argument_count != 0u) {
            memcpy(owned_ids
                    + expression->data.call.type_substitution_count,
                expression->data.call.arguments,
                (size_t)expression->data.call.argument_count
                    * sizeof(uint32_t));
        }
        copy.data.call.type_substitutions =
            (CmHirTypeId *)owned_ids;
        copy.data.call.arguments = owned_ids == NULL ? NULL
            : (CmHirExprId *)(owned_ids
                + expression->data.call.type_substitution_count);
        copy.data.call.owned_storage = owned_ids;
        break;
    case CM_HIR_EXPR_METHOD_CALL:
        if (body == NULL
            || !cm_hir_method_call_expression_valid(context, expression)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        copy.data.method_call.arguments =
            (CmHirExprId *)cm_hir_copy_array(context,
                expression->data.method_call.arguments,
                expression->data.method_call.argument_count,
                sizeof(CmHirExprId));
        copy.data.method_call.in_scope_traits =
            (CmHirDefId *)cm_hir_copy_array(context,
                expression->data.method_call.in_scope_traits,
                expression->data.method_call.in_scope_trait_count,
                sizeof(CmHirDefId));
        break;
    case CM_HIR_EXPR_QUALIFIED_CALL:
        if (body == NULL
            || !cm_hir_qualified_call_expression_valid(context,
                expression)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (context->expressions.len >= (size_t)UINT32_MAX
            || !cm_size_mul(
                (size_t)expression->data.qualified_call.argument_count,
                sizeof(uint32_t), &owned_bytes)
            || !cm_size_add(context->expressions.len, 1u,
                &reserved_count)) {
            return CM_HIR_ID_EXHAUSTED;
        }
        cm_vec_reserve(&context->expressions, reserved_count);
        owned_ids = expression->data.qualified_call.argument_count == 0u
            ? NULL : (uint32_t *)cm_alloc(owned_bytes);
        if (expression->data.qualified_call.argument_count != 0u) {
            memcpy(owned_ids, expression->data.qualified_call.arguments,
                (size_t)expression->data.qualified_call.argument_count
                    * sizeof(uint32_t));
        }
        copy.data.qualified_call.arguments = (CmHirExprId *)owned_ids;
        copy.data.qualified_call.owned_storage = owned_ids;
        break;
    case CM_HIR_EXPR_BINARY:
    {
        const CmHirExpr *left;
        const CmHirExpr *right;
        const CmHirType *left_type;
        const CmHirType *right_type;
        int is_equality;
        int is_less;
        int is_arithmetic;
        int operands_are_u32;
        int operands_are_usize;

        left = cm_hir_get_expr(context, expression->data.binary.left);
        right = cm_hir_get_expr(context, expression->data.binary.right);
        left_type = left == NULL ? NULL
            : cm_hir_get_type(context, left->type);
        right_type = right == NULL ? NULL
            : cm_hir_get_type(context, right->type);
        is_equality = expression->data.binary.operator_kind
            == CM_HIR_BINARY_EQUAL;
        is_less = expression->data.binary.operator_kind
            == CM_HIR_BINARY_LESS;
        is_arithmetic = expression->data.binary.operator_kind
                == CM_HIR_BINARY_ADD
            || expression->data.binary.operator_kind
                == CM_HIR_BINARY_SUBTRACT;
        operands_are_u32 = left_type != NULL && right_type != NULL
            && left_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && right_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && left_type->data.integer_type.kind == CM_HIR_INT_U32
            && right_type->data.integer_type.kind == CM_HIR_INT_U32;
        operands_are_usize = left_type != NULL && right_type != NULL
            && left_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && right_type->kind == CM_HIR_TYPE_INTEGER_KIND
            && left_type->data.integer_type.kind == CM_HIR_INT_USIZE
            && right_type->data.integer_type.kind == CM_HIR_INT_USIZE;
        if (body == NULL
            || (!is_arithmetic && !is_equality && !is_less)
            || left == NULL || right == NULL
            || left->owner_body != expression->owner_body
            || right->owner_body != expression->owner_body
            || (is_equality && !operands_are_u32)
            || (is_less && !operands_are_usize)
            || (is_arithmetic && !operands_are_u32
                && !operands_are_usize)
            || ((is_equality || is_less)
                ? type->kind != CM_HIR_TYPE_BOOL_KIND
                : (type->kind != CM_HIR_TYPE_INTEGER_KIND
                    || type->data.integer_type.kind
                        != left_type->data.integer_type.kind
                    || !cm_hir_body_type_equal(context, left->type,
                        expression->type)
                    || !cm_hir_body_type_equal(context, right->type,
                        expression->type)))
            || left->span.source != expression->span.source
            || right->span.source != expression->span.source
            || left->span.start < expression->span.start
            || left->span.end > expression->span.end
            || right->span.start < expression->span.start
            || right->span.end > expression->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    case CM_HIR_EXPR_IF:
    {
        const CmHirExpr *condition;
        const CmHirExpr *then_expression;
        const CmHirExpr *else_expression;
        const CmHirType *condition_type;

        condition = cm_hir_get_expr(context,
            expression->data.if_expr.condition);
        then_expression = cm_hir_get_expr(context,
            expression->data.if_expr.then_expression);
        else_expression = cm_hir_get_expr(context,
            expression->data.if_expr.else_expression);
        condition_type = condition == NULL ? NULL
            : cm_hir_get_type(context, condition->type);
        if (body == NULL || type->kind != CM_HIR_TYPE_INTEGER_KIND
            || (type->data.integer_type.kind != CM_HIR_INT_U32
                && type->data.integer_type.kind != CM_HIR_INT_USIZE)
            || condition == NULL || then_expression == NULL
            || else_expression == NULL || condition_type == NULL
            || condition_type->kind != CM_HIR_TYPE_BOOL_KIND
            || condition->kind != CM_HIR_EXPR_BINARY
            || condition->data.binary.operator_kind
                != (type->data.integer_type.kind == CM_HIR_INT_U32
                    ? CM_HIR_BINARY_EQUAL : CM_HIR_BINARY_LESS)
            || condition->owner_body != expression->owner_body
            || then_expression->owner_body != expression->owner_body
            || else_expression->owner_body != expression->owner_body
            || then_expression->kind != CM_HIR_EXPR_BLOCK
            || else_expression->kind != CM_HIR_EXPR_BLOCK
            || then_expression->data.block.statement_count != 0u
            || then_expression->data.block.statements != NULL
            || else_expression->data.block.statement_count != 0u
            || else_expression->data.block.statements != NULL
            || !cm_hir_body_type_equal(context, then_expression->type,
                expression->type)
            || !cm_hir_body_type_equal(context, else_expression->type,
                expression->type)
            || condition->span.source != expression->span.source
            || then_expression->span.source != expression->span.source
            || else_expression->span.source != expression->span.source
            || condition->span.start < expression->span.start
            || condition->span.end > then_expression->span.start
            || then_expression->span.end > else_expression->span.start
            || else_expression->span.end > expression->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    case CM_HIR_EXPR_AGGREGATE:
    {
        const CmHirAggregateFieldValue *fields;

        if (!cm_hir_aggregate_expression_valid(context, expression, type,
                body)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if (context->expressions.len >= (size_t)UINT32_MAX
            || !cm_size_add(context->expressions.len, 1u,
                &reserved_count)) {
            return CM_HIR_ID_EXHAUSTED;
        }
        fields = expression->data.aggregate.fields;
        cm_vec_reserve(&context->expressions, reserved_count);
        copy.data.aggregate.fields =
            (CmHirAggregateFieldValue *)cm_hir_copy_array(context, fields,
                expression->data.aggregate.field_count,
                sizeof(CmHirAggregateFieldValue));
        copy.data.aggregate.owned_storage = NULL;
        break;
    }
    case CM_HIR_EXPR_FIELD:
        if (!cm_hir_field_expression_valid(context, expression, body)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    case CM_HIR_EXPR_BORROW_SHARED:
    {
        const CmHirExpr *operand;

        operand = cm_hir_get_expr(context,
            expression->data.borrow_shared.operand);
        if (body == NULL || operand == NULL
            || operand->owner_body != expression->owner_body
            || (operand->kind != CM_HIR_EXPR_LOCAL
                && operand->kind != CM_HIR_EXPR_FIELD
                && operand->kind != CM_HIR_EXPR_DEREFERENCE)
            || type->kind != CM_HIR_TYPE_REFERENCE_KIND
            || type->data.reference_type.mutability != CM_HIR_IMMUTABLE
            || type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
            || !cm_hir_body_type_equal(context,
                type->data.reference_type.pointee, operand->type)
            || operand->span.source != expression->span.source
            || operand->span.start <= expression->span.start
            || operand->span.end != expression->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    case CM_HIR_EXPR_DEREFERENCE:
    {
        const CmHirExpr *operand;
        const CmHirType *operand_type;

        operand = cm_hir_get_expr(context,
            expression->data.dereference.operand);
        operand_type = operand == NULL ? NULL
            : cm_hir_get_type(context, operand->type);
        if (body == NULL || operand == NULL
            || operand->owner_body != expression->owner_body
            || operand_type == NULL
            || operand_type->kind != CM_HIR_TYPE_REFERENCE_KIND
            || operand_type->data.reference_type.mutability
                != CM_HIR_IMMUTABLE
            || operand_type->data.reference_type.region.kind
                != CM_HIR_REGION_ERASED
            || !cm_hir_body_type_equal(context,
                operand_type->data.reference_type.pointee,
                expression->type)
            || operand->span.source != expression->span.source
            || operand->span.start <= expression->span.start
            || operand->span.end != expression->span.end) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        break;
    }
    }
    return cm_hir_push(context, &context->expressions, &copy, out_id);
}

CmHirStatus cm_hir_add_owned_call_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id)
{
    const CmHirType *type;
    const CmHirBody *body;
    const uint32_t *expected_arguments;

    if (context == NULL || expression == NULL || out_id == NULL) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_EXPR_NONE;
    if (expression->kind != CM_HIR_EXPR_CALL
        || expression->usage != CM_HIR_USAGE_UNKNOWN
        || expression->static_borrow_state != CM_HIR_STATIC_BORROW_UNKNOWN
        || expression->data.call.type_substitutions == NULL
        || expression->data.call.argument_count == 0u
        || expression->data.call.arguments == NULL
        || context->expressions.len >= context->expressions.cap
        || context->expressions.len >= (size_t)UINT32_MAX) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    expected_arguments = (const uint32_t *)
        expression->data.call.type_substitutions
        + expression->data.call.type_substitution_count;
    if ((const uint32_t *)expression->data.call.arguments
            != expected_arguments) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    if (expression->data.call.owned_storage != NULL
        && expression->data.call.owned_storage
            != (uint32_t *)expression->data.call.type_substitutions) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    type = cm_hir_get_type(context, expression->type);
    body = cm_hir_get_body(context, expression->owner_body);
    if (type == NULL || body == NULL
        || body->state != CM_HIR_BODY_UNLOWERED
        || !cm_hir_span_is_ordered(expression->span)
        || !cm_hir_expression_body_span_valid(body, expression->span)
        || !cm_hir_call_expression_valid(context, expression)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    return cm_hir_push(context, &context->expressions, expression, out_id);
}

CmHirStatus cm_hir_add_owned_qualified_call_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id)
{
    const CmHirType *type;
    const CmHirBody *body;

    if (context == NULL || expression == NULL || out_id == NULL) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_EXPR_NONE;
    if (expression->kind != CM_HIR_EXPR_QUALIFIED_CALL
        || expression->usage != CM_HIR_USAGE_UNKNOWN
        || expression->static_borrow_state != CM_HIR_STATIC_BORROW_UNKNOWN
        || (expression->data.qualified_call.argument_count == 0u)
            != (expression->data.qualified_call.arguments == NULL)
        || context->expressions.len >= context->expressions.cap
        || context->expressions.len >= (size_t)UINT32_MAX
        || (expression->data.qualified_call.owned_storage != NULL
            && expression->data.qualified_call.owned_storage
                != (uint32_t *)expression->data.qualified_call.arguments)) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    type = cm_hir_get_type(context, expression->type);
    body = cm_hir_get_body(context, expression->owner_body);
    if (type == NULL || body == NULL
        || body->state != CM_HIR_BODY_UNLOWERED
        || !cm_hir_span_is_ordered(expression->span)
        || !cm_hir_expression_body_span_valid(body, expression->span)
        || !cm_hir_qualified_call_expression_valid(context, expression)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    return cm_hir_push(context, &context->expressions, expression, out_id);
}

CmHirStatus cm_hir_add_owned_aggregate_expr(CmHirContext *context,
    const CmHirExpr *expression, CmHirExprId *out_id)
{
    const CmHirType *type;
    const CmHirBody *body;

    if (context == NULL || expression == NULL || out_id == NULL) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    *out_id = CM_HIR_EXPR_NONE;
    if (expression->kind != CM_HIR_EXPR_AGGREGATE
        || expression->usage != CM_HIR_USAGE_UNKNOWN
        || expression->static_borrow_state != CM_HIR_STATIC_BORROW_UNKNOWN
        || (expression->data.aggregate.field_count == 0u
            && (expression->data.aggregate.fields != NULL
                || expression->data.aggregate.owned_storage != NULL))
        || (expression->data.aggregate.field_count != 0u
            && (expression->data.aggregate.fields == NULL
                || (expression->data.aggregate.owned_storage != NULL
                    && expression->data.aggregate.owned_storage
                        != expression->data.aggregate.fields)))
        || context->expressions.len >= context->expressions.cap
        || context->expressions.len >= (size_t)UINT32_MAX) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    type = cm_hir_get_type(context, expression->type);
    body = cm_hir_get_body(context, expression->owner_body);
    if (type == NULL || body == NULL
        || body->state != CM_HIR_BODY_UNLOWERED
        || !cm_hir_span_is_ordered(expression->span)
        || !cm_hir_expression_body_span_valid(body, expression->span)
        || !cm_hir_aggregate_expression_valid(context, expression, type,
            body)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    return cm_hir_push(context, &context->expressions, expression, out_id);
}

CmHirStatus cm_hir_set_body_root_expression(CmHirContext *context,
    CmHirBodyId body_id, CmHirExprId root_expression)
{
    CmHirBody *body;
    const CmHirExpr *root;
    uint32_t initial_visible_local_count;

    if (context == NULL || body_id == CM_HIR_BODY_NONE
        || root_expression == CM_HIR_EXPR_NONE) {
        return CM_HIR_INVALID_ARGUMENT;
    }
    body = (CmHirBody *)cm_vec_at(&context->bodies, (size_t)body_id - 1u);
    root = cm_hir_get_expr(context, root_expression);
    if (body == NULL || root == NULL) return CM_HIR_INVALID_ID;
    initial_visible_local_count = 0u;
    while (initial_visible_local_count < body->local_count
        && body->locals[initial_visible_local_count].parameter_index
            != CM_HIR_PARAMETER_INDEX_NONE) {
        ++initial_visible_local_count;
    }
    if (body->state != CM_HIR_BODY_UNLOWERED
        || body->root_expression != CM_HIR_EXPR_NONE
        || body->source == 0u || body->source_expression_id == 0u
        || body->error_reason != CM_INTERN_ID_NONE
        || !cm_hir_body_type_equal(context, root->type,
            body->expected_type)
        || (root->owner_body != CM_HIR_BODY_NONE
            && root->owner_body != body_id)
        || root->span.source != body->source
        || root->span.start < body->span.start
        || root->span.end > body->span.end
        || !cm_hir_expression_scope_valid(context, root_expression,
            body_id, initial_visible_local_count,
            CM_HIR_CLOSURE_NONE, 0u)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    cm_hir_claim_expression_tree(context, root_expression, body_id);
    body->root_expression = root_expression;
    body->state = CM_HIR_BODY_TYPED;
    cm_hir_context_record_semantic_mutation(context);
    return CM_HIR_OK;
}

const char *cm_hir_status_name(CmHirStatus status)
{
    switch (status) {
    case CM_HIR_OK:
        return "ok";
    case CM_HIR_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_INVALID_ID:
        return "invalid id";
    case CM_HIR_ID_EXHAUSTED:
        return "id exhausted";
    case CM_HIR_INVARIANT_VIOLATION:
        return "invariant violation";
    }
    return "unknown HIR status";
}
