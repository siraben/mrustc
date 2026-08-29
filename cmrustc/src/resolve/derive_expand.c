#include "cm/resolve/derive_expand.h"
#include "cm/alloc.h"
#include "cm/buf.h"
#include "cm/vec.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void cm_str_buf_append_ulong(CmStrBuf *out, unsigned long value)
{
    char digits[24];
    size_t n = 0u;
    do {
        digits[n++] = (char)('0' + (value % 10ul));
        value /= 10ul;
    } while (value != 0ul && n < sizeof(digits));
    while (n != 0u) cm_str_buf_push(out, digits[--n]);
}

/* Text of an interned AST string, or "" . */
static void cm_derive_append_intern(CmStrBuf *out, const CmAst *ast,
    CmInternId id)
{
    const CmInternedString *text = id == CM_INTERN_ID_NONE ? NULL
        : cm_ast_get_string(ast, id);
    if (text != NULL) cm_str_buf_append_n(out, (const char *)text->bytes,
        text->len);
}

/* Whether the attribute text `derive(...)` lists `name`. */
static int cm_derive_lists(const CmInternedString *text, const char *name)
{
    size_t at;
    size_t length = strlen(name);
    size_t skip = 0u;
    if (text == NULL) return 0;
    /* Attribute text keeps its `#[` / `#![` wrapper. */
    if (text->len > 2u && text->bytes[0] == '#' && text->bytes[1] == '[')
        skip = 2u;
    else if (text->len > 3u && text->bytes[0] == '#' && text->bytes[1] == '!'
        && text->bytes[2] == '[')
        skip = 3u;
    if (text->len < skip + 9u
        || memcmp(text->bytes + skip, "derive(", 7u) != 0) return 0;
    at = skip + 7u; /* after "derive(" */
    while (at < text->len) {
        size_t start;
        while (at < text->len && (text->bytes[at] == ' '
                || text->bytes[at] == ',' || text->bytes[at] == '\n'
                || text->bytes[at] == '\t')) at += 1u;
        start = at;
        while (at < text->len && text->bytes[at] != ','
                && text->bytes[at] != ')' && text->bytes[at] != ' ') at += 1u;
        /* A path such as `core::fmt::Debug`: compare the last segment. */
        {
            size_t seg = start;
            size_t last = start;
            for (seg = start; seg < at; ++seg)
                if (text->bytes[seg] == ':') last = seg + 1u;
            if (at - last == length
                && memcmp(text->bytes + last, name, length) == 0) return 1;
        }
        if (at < text->len && text->bytes[at] == ')') break;
        if (at == start) at += 1u;
    }
    return 0;
}

static int cm_derive_item_lists(const CmAst *ast, const CmAstItem *item,
    const char *name)
{
    uint32_t index;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmAstAttribute *attribute = cm_ast_get_attribute(ast,
            item->attributes[index]);
        if (attribute == NULL) continue;
        if (cm_derive_lists(cm_ast_get_string(ast, attribute->text), name))
            return 1;
    }
    return 0;
}

/* `#[cfg(..)]` / `#[cfg_attr(..)]` attributes of `item`, so a derived
 * impl is stripped together with a cfg-gated definition (core's
 * `AlignmentEnum` exists once per `target_pointer_width`). */
static void cm_derive_copy_cfg(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item)
{
    uint32_t index;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmAstAttribute *attribute = cm_ast_get_attribute(ast,
            item->attributes[index]);
        const CmInternedString *text = attribute == NULL ? NULL
            : cm_ast_get_string(ast, attribute->text);
        size_t skip;
        if (text == NULL || text->len < 8u) continue;
        skip = text->bytes[0] == '#' && text->bytes[1] == '[' ? 2u : 0u;
        if (text->len > skip + 4u
            && memcmp(text->bytes + skip, "cfg(", 4u) == 0) {
            cm_str_buf_append_n(out, (const char *)text->bytes, text->len);
            cm_str_buf_push(out, '\n');
        }
    }
}

/* `impl<decls> TRAIT for Name<uses> where T: BOUND, ... {` */
static void cm_derive_open_impl(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item, const char *trait_path, const char *bound_path)
{
    uint32_t index;
    int first_pred = 1;
    cm_derive_copy_cfg(out, ast, item);
    cm_str_buf_append(out, "impl");
    if (item->generic_parameter_count != 0u) {
        cm_str_buf_push(out, '<');
        for (index = 0u; index < item->generic_parameter_count; ++index) {
            const CmAstGenericParam *param = &item->generic_parameters[index];
            if (index != 0u) cm_str_buf_append(out, ", ");
            if (param->declaration != CM_INTERN_ID_NONE) {
                cm_derive_append_intern(out, ast, param->declaration);
            } else {
                if (param->kind == CM_AST_PARAM_LIFETIME)
                    cm_str_buf_push(out, '\'');
                else if (param->kind == CM_AST_PARAM_CONST)
                    cm_str_buf_append(out, "const ");
                cm_derive_append_intern(out, ast, param->name);
                if (param->kind == CM_AST_PARAM_CONST)
                    cm_str_buf_append(out, ": usize");
            }
        }
        cm_str_buf_push(out, '>');
    }
    cm_str_buf_push(out, ' ');
    cm_str_buf_append(out, trait_path);
    cm_str_buf_append(out, " for ");
    cm_derive_append_intern(out, ast, item->name);
    if (item->generic_parameter_count != 0u) {
        cm_str_buf_push(out, '<');
        for (index = 0u; index < item->generic_parameter_count; ++index) {
            const CmAstGenericParam *param = &item->generic_parameters[index];
            if (index != 0u) cm_str_buf_append(out, ", ");
            if (param->kind == CM_AST_PARAM_LIFETIME) cm_str_buf_push(out, '\'');
            cm_derive_append_intern(out, ast, param->name);
        }
        cm_str_buf_push(out, '>');
    }
    if (bound_path != NULL) {
        for (index = 0u; index < item->generic_parameter_count; ++index) {
            const CmAstGenericParam *param = &item->generic_parameters[index];
            if (param->kind != CM_AST_PARAM_TYPE) continue;
            cm_str_buf_append(out, first_pred ? " where " : ", ");
            first_pred = 0;
            cm_derive_append_intern(out, ast, param->name);
            cm_str_buf_append(out, ": ");
            cm_str_buf_append(out, bound_path);
        }
    }
    if (item->where_clause != CM_INTERN_ID_NONE) {
        const CmInternedString *clause = cm_ast_get_string(ast,
            item->where_clause);
        if (clause != NULL && clause->len != 0u) {
            cm_str_buf_append(out, first_pred ? " where " : ", ");
            first_pred = 0;
            cm_str_buf_append_n(out, (const char *)clause->bytes, clause->len);
        }
    }
    cm_str_buf_append(out, " {\n");
}

/* The fields of one struct or variant: `form`, `fields`, `count`. */
typedef struct CmDeriveShape {
    CmAstFieldForm form;
    const CmAstField *fields;
    uint32_t count;
} CmDeriveShape;

static void cm_derive_shape_of_item(const CmAstItem *item,
    CmDeriveShape *shape)
{
    shape->form = item->data.aggregate_item.form;
    shape->fields = item->data.aggregate_item.fields;
    shape->count = item->data.aggregate_item.field_count;
}

static void cm_derive_shape_of_variant(const CmAstVariant *variant,
    CmDeriveShape *shape)
{
    shape->form = variant->form;
    shape->fields = variant->fields;
    shape->count = variant->field_count;
}

/* `Name` / `Name::Variant` */
static void cm_derive_path(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item, const CmAstVariant *variant)
{
    cm_derive_append_intern(out, ast, item->name);
    if (variant != NULL) {
        cm_str_buf_append(out, "::");
        cm_derive_append_intern(out, ast, variant->name);
    }
}

/* A by-value pattern binding each field to `__pN` / `__sN`. */
static void cm_derive_pattern(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item, const CmAstVariant *variant,
    const CmDeriveShape *shape, const char *prefix)
{
    uint32_t index;
    cm_derive_path(out, ast, item, variant);
    if (shape->form == CM_AST_FIELDS_TUPLE) {
        cm_str_buf_push(out, '(');
        for (index = 0u; index < shape->count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_str_buf_append(out, prefix);
            cm_str_buf_append_ulong(out, (unsigned long)index);
        }
        cm_str_buf_push(out, ')');
    } else if (shape->form == CM_AST_FIELDS_NAMED) {
        cm_str_buf_append(out, " { ");
        for (index = 0u; index < shape->count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_derive_append_intern(out, ast, shape->fields[index].name);
            cm_str_buf_append(out, ": ");
            cm_str_buf_append(out, prefix);
            cm_str_buf_append_ulong(out, (unsigned long)index);
        }
        cm_str_buf_append(out, " }");
    }
}

/* Debug arm body for one shape: builder calls on `f`. */
static void cm_derive_debug_body(CmStrBuf *out, const CmAst *ast,
    const CmInternedString *name, const CmDeriveShape *shape)
{
    uint32_t index;
    if (shape->form == CM_AST_FIELDS_UNIT || shape->count == 0u) {
        cm_str_buf_append(out, "f.write_str(\"");
        cm_str_buf_append_n(out, (const char *)name->bytes, name->len);
        cm_str_buf_append(out, "\")");
        return;
    }
    if (shape->form == CM_AST_FIELDS_TUPLE) {
        cm_str_buf_append(out, "f.debug_tuple(\"");
        cm_str_buf_append_n(out, (const char *)name->bytes, name->len);
        cm_str_buf_append(out, "\")");
        for (index = 0u; index < shape->count; ++index) {
            cm_str_buf_append(out, ".field(&__p");
            cm_str_buf_append_ulong(out, (unsigned long)index);
            cm_str_buf_push(out, ')');
        }
        cm_str_buf_append(out, ".finish()");
        return;
    }
    cm_str_buf_append(out, "f.debug_struct(\"");
    cm_str_buf_append_n(out, (const char *)name->bytes, name->len);
    cm_str_buf_append(out, "\")");
    for (index = 0u; index < shape->count; ++index) {
        const CmInternedString *field = cm_ast_get_string(ast,
            shape->fields[index].name);
        cm_str_buf_append(out, ".field(\"");
        if (field != NULL)
            cm_str_buf_append_n(out, (const char *)field->bytes, field->len);
        cm_str_buf_append(out, "\", &__p");
        cm_str_buf_append_ulong(out, (unsigned long)index);
        cm_str_buf_push(out, ')');
    }
    cm_str_buf_append(out, ".finish()");
}

static void cm_derive_debug(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item)
{
    cm_derive_open_impl(out, ast, item, "::core::fmt::Debug",
        "::core::fmt::Debug");
    cm_str_buf_append(out, "    fn fmt(&self, f: &mut ::core::fmt::Formatter"
        "<'_>) -> ::core::fmt::Result {\n        match *self {\n");
    if (item->kind == CM_AST_ITEM_STRUCT) {
        CmDeriveShape shape;
        cm_derive_shape_of_item(item, &shape);
        cm_str_buf_append(out, "            ");
        cm_derive_pattern(out, ast, item, NULL, &shape, "__p");
        cm_str_buf_append(out, " => ");
        cm_derive_debug_body(out, ast, cm_ast_get_string(ast, item->name),
            &shape);
        cm_str_buf_append(out, ",\n");
    } else {
        uint32_t index;
        for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
            const CmAstVariant *variant = &item->data.enum_item.variants[index];
            CmDeriveShape shape;
            cm_derive_shape_of_variant(variant, &shape);
            cm_str_buf_append(out, "            ");
            cm_derive_pattern(out, ast, item, variant, &shape, "__p");
            cm_str_buf_append(out, " => ");
            cm_derive_debug_body(out, ast,
                cm_ast_get_string(ast, variant->name), &shape);
            cm_str_buf_append(out, ",\n");
        }
        if (item->data.enum_item.variant_count == 0u)
            cm_str_buf_append(out, "            _ => Ok(()),\n");
    }
    cm_str_buf_append(out, "        }\n    }\n}\n");
}

/* Construction of one shape from `__pN` clones. */
static void cm_derive_clone_construct(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item, const CmAstVariant *variant,
    const CmDeriveShape *shape)
{
    uint32_t index;
    cm_derive_path(out, ast, item, variant);
    if (shape->form == CM_AST_FIELDS_TUPLE) {
        cm_str_buf_push(out, '(');
        for (index = 0u; index < shape->count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_str_buf_append(out, "::core::clone::Clone::clone(&__p");
            cm_str_buf_append_ulong(out, (unsigned long)index);
            cm_str_buf_push(out, ')');
        }
        cm_str_buf_push(out, ')');
    } else if (shape->form == CM_AST_FIELDS_NAMED) {
        cm_str_buf_append(out, " { ");
        for (index = 0u; index < shape->count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_derive_append_intern(out, ast, shape->fields[index].name);
            cm_str_buf_append(out, ": ::core::clone::Clone::clone(&__p");
            cm_str_buf_append_ulong(out, (unsigned long)index);
            cm_str_buf_push(out, ')');
        }
        cm_str_buf_append(out, " }");
    }
}

static void cm_derive_clone(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item)
{
    cm_derive_open_impl(out, ast, item, "::core::clone::Clone",
        "::core::clone::Clone");
    cm_str_buf_append(out, "    fn clone(&self) -> Self {\n"
        "        match *self {\n");
    if (item->kind == CM_AST_ITEM_STRUCT) {
        CmDeriveShape shape;
        cm_derive_shape_of_item(item, &shape);
        cm_str_buf_append(out, "            ");
        cm_derive_pattern(out, ast, item, NULL, &shape, "__p");
        cm_str_buf_append(out, " => ");
        cm_derive_clone_construct(out, ast, item, NULL, &shape);
        cm_str_buf_append(out, ",\n");
    } else {
        uint32_t index;
        for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
            const CmAstVariant *variant = &item->data.enum_item.variants[index];
            CmDeriveShape shape;
            cm_derive_shape_of_variant(variant, &shape);
            cm_str_buf_append(out, "            ");
            cm_derive_pattern(out, ast, item, variant, &shape, "__p");
            cm_str_buf_append(out, " => ");
            cm_derive_clone_construct(out, ast, item, variant, &shape);
            cm_str_buf_append(out, ",\n");
        }
        if (item->data.enum_item.variant_count == 0u)
            cm_str_buf_append(out, "            _ => loop {},\n");
    }
    cm_str_buf_append(out, "        }\n    }\n}\n");
}

static void cm_derive_marker(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item, const char *trait_path)
{
    cm_derive_open_impl(out, ast, item, trait_path, trait_path);
    cm_str_buf_append(out, "}\n");
}

static void cm_derive_partial_eq(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item)
{
    cm_derive_open_impl(out, ast, item, "::core::cmp::PartialEq",
        "::core::cmp::PartialEq");
    cm_str_buf_append(out, "    fn eq(&self, other: &Self) -> bool {\n");
    if (item->kind == CM_AST_ITEM_STRUCT) {
        CmDeriveShape shape;
        uint32_t index;
        cm_derive_shape_of_item(item, &shape);
        cm_str_buf_append(out, "        match (*self, *other) {\n            (");
        cm_derive_pattern(out, ast, item, NULL, &shape, "__p");
        cm_str_buf_append(out, ", ");
        cm_derive_pattern(out, ast, item, NULL, &shape, "__q");
        cm_str_buf_append(out, ") => true");
        for (index = 0u; index < shape.count; ++index) {
            cm_str_buf_append(out, " && __p");
            cm_str_buf_append_ulong(out, (unsigned long)index);
            cm_str_buf_append(out, " == __q");
            cm_str_buf_append_ulong(out, (unsigned long)index);
        }
        cm_str_buf_append(out, ",\n        }\n");
    } else {
        uint32_t index;
        cm_str_buf_append(out, "        match (*self, *other) {\n");
        for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
            const CmAstVariant *variant = &item->data.enum_item.variants[index];
            CmDeriveShape shape;
            uint32_t field;
            cm_derive_shape_of_variant(variant, &shape);
            cm_str_buf_append(out, "            (");
            cm_derive_pattern(out, ast, item, variant, &shape, "__p");
            cm_str_buf_append(out, ", ");
            cm_derive_pattern(out, ast, item, variant, &shape, "__q");
            cm_str_buf_append(out, ") => true");
            for (field = 0u; field < shape.count; ++field) {
                cm_str_buf_append(out, " && __p");
                cm_str_buf_append_ulong(out, (unsigned long)field);
                cm_str_buf_append(out, " == __q");
                cm_str_buf_append_ulong(out, (unsigned long)field);
            }
            cm_str_buf_append(out, ",\n");
        }
        cm_str_buf_append(out, "            _ => false,\n        }\n");
    }
    cm_str_buf_append(out, "    }\n}\n");
}

static void cm_derive_default(CmStrBuf *out, const CmAst *ast,
    const CmAstItem *item)
{
    CmDeriveShape shape;
    uint32_t index;
    if (item->kind != CM_AST_ITEM_STRUCT) return; /* enums need #[default] */
    cm_derive_shape_of_item(item, &shape);
    cm_derive_open_impl(out, ast, item, "::core::default::Default",
        "::core::default::Default");
    cm_str_buf_append(out, "    fn default() -> Self {\n        ");
    cm_derive_append_intern(out, ast, item->name);
    if (shape.form == CM_AST_FIELDS_TUPLE) {
        cm_str_buf_push(out, '(');
        for (index = 0u; index < shape.count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_str_buf_append(out, "::core::default::Default::default()");
        }
        cm_str_buf_push(out, ')');
    } else if (shape.form == CM_AST_FIELDS_NAMED) {
        cm_str_buf_append(out, " { ");
        for (index = 0u; index < shape.count; ++index) {
            if (index != 0u) cm_str_buf_append(out, ", ");
            cm_derive_append_intern(out, ast, shape.fields[index].name);
            cm_str_buf_append(out, ": ::core::default::Default::default()");
        }
        cm_str_buf_append(out, " }");
    }
    cm_str_buf_append(out, "\n    }\n}\n");
}

/* Appends `ids` to the module item list that contains `target`; root
 * items live in the vec, inline modules in an arena-owned array. */
static void cm_derive_append_to_container(CmAst *ast, CmAstItemId target,
    const CmAstItemId *ids, uint32_t count)
{
    size_t index;
    const CmAstItemId *roots = (const CmAstItemId *)ast->root_items.data;
    for (index = 0u; index < ast->root_items.len; ++index)
        if (roots[index] == target) {
            cm_vec_append(&ast->root_items, ids, count);
            return;
        }
    for (index = 0u; index < ast->items.len; ++index) {
        CmAstItem *container = (CmAstItem *)cm_vec_at(&ast->items, index);
        uint32_t child;
        if (container == NULL || container->kind != CM_AST_ITEM_MODULE
            || !container->data.module_item.is_inline) continue;
        for (child = 0u; child < container->data.module_item.item_count;
                ++child) {
            if (container->data.module_item.items[child] != target) continue;
            {
                uint32_t old_count = container->data.module_item.item_count;
                size_t size = (size_t)(old_count + count) * sizeof(CmAstItemId);
                CmAstItemId *copy = (CmAstItemId *)cm_arena_alloc(
                    &ast->storage, size, sizeof(CmAstItemId));
                memcpy(copy, container->data.module_item.items,
                    (size_t)old_count * sizeof(CmAstItemId));
                memcpy(copy + old_count, ids,
                    (size_t)count * sizeof(CmAstItemId));
                container->data.module_item.items = copy;
                container->data.module_item.item_count = old_count + count;
            }
            return;
        }
    }
}

int cm_derive_unit_aliases_core(const CmAst *ast)
{
    size_t index;
    for (index = 0u; index < ast->items.len; ++index) {
        const CmAstItem *item = cm_ast_get_item(ast, (CmAstItemId)index);
        const CmInternedString *alias;
        if (item == NULL || item->kind != CM_AST_ITEM_EXTERN_CRATE) continue;
        alias = item->data.extern_crate_item.alias == CM_INTERN_ID_NONE
            ? cm_ast_get_string(ast, item->name)
            : cm_ast_get_string(ast, item->data.extern_crate_item.alias);
        if (alias != NULL && alias->len == 4u
            && memcmp(alias->bytes, "core", 4u) == 0) return 1;
    }
    return 0;
}

size_t cm_derive_expand(CmAst *ast, enum cm_edition edition,
    int core_reachable)
{
    size_t appended = 0u;
    size_t original = ast->items.len;
    size_t index;
    if (!core_reachable && !cm_derive_unit_aliases_core(ast)) return 0u;
    for (index = 0u; index < original; ++index) {
        const CmAstItem *item = cm_ast_get_item(ast, (CmAstItemId)index);
        CmStrBuf text;
        int any = 0;
        if (item == NULL || (item->kind != CM_AST_ITEM_STRUCT
                && item->kind != CM_AST_ITEM_ENUM)
            || item->attribute_count == 0u) continue;
        cm_str_buf_init(&text);
        if (getenv("CM_DERIVE_DEBUG") != NULL) {
            uint32_t a;
            for (a = 0u; a < item->attribute_count; ++a) {
                const CmAstAttribute *attribute = cm_ast_get_attribute(ast,
                    item->attributes[a]);
                const CmInternedString *t = attribute == NULL ? NULL
                    : cm_ast_get_string(ast, attribute->text);
                fprintf(stderr, "DERIVE attr item=%lu [%.*s]\n",
                    (unsigned long)index, t == NULL ? 1 : (int)t->len,
                    t == NULL ? "?" : (const char *)t->bytes);
            }
        }
        if (cm_derive_item_lists(ast, item, "Debug")) {
            cm_derive_debug(&text, ast, item); any = 1;
        }
        if (cm_derive_item_lists(ast, item, "Clone")) {
            cm_derive_clone(&text, ast, item); any = 1;
        }
        if (cm_derive_item_lists(ast, item, "Copy")) {
            cm_derive_marker(&text, ast, item, "::core::marker::Copy"); any = 1;
        }
        if (cm_derive_item_lists(ast, item, "PartialEq")) {
            cm_derive_partial_eq(&text, ast, item); any = 1;
        }
        if (cm_derive_item_lists(ast, item, "Eq")) {
            cm_derive_marker(&text, ast, item, "::core::cmp::Eq"); any = 1;
        }
        if (cm_derive_item_lists(ast, item, "Default")
            && item->kind == CM_AST_ITEM_STRUCT) {
            cm_derive_default(&text, ast, item); any = 1;
        }
        if (any && text.len != 0u) {
            CmItemListFragment fragment = cm_parse_item_list_fragment(ast,
                text.data, text.len, edition);
            if (getenv("CM_DERIVE_DEBUG") != NULL)
                fprintf(stderr, "DERIVE item=%lu items=%u errors=%u %s\n%.*s\n",
                    (unsigned long)index, (unsigned)fragment.item_count,
                    (unsigned)fragment.parse.error_count,
                    fragment.parse.error_count != 0u
                        ? fragment.parse.first_error.message : "",
                    (int)text.len, text.data);
            if (fragment.parse.error_count == 0u && fragment.item_count != 0u) {
                /* `item` may have moved: the vec grew. */
                cm_derive_append_to_container(ast, (CmAstItemId)index,
                    fragment.items, fragment.item_count);
                appended += fragment.item_count;
            }
        }
        cm_str_buf_destroy(&text);
    }
    return appended;
}
