#include "cm/hir/model.h"
#include "cm/hir/semantic_mark.h"

#include "cm/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirTypeId add_simple_type(CmHirContext *context,
    CmHirTypeKind kind, CmSpan span)
{
    CmHirType type;
    CmHirTypeId id;

    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.span = span;
    assert(cm_hir_add_type(context, &type, &id) == CM_HIR_OK);
    return id;
}

static char *read_dump(FILE *file)
{
    long length;
    size_t bytes_read;
    char *text;

    assert(fflush(file) == 0);
    assert(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    assert(length >= 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    text = (char *)malloc((size_t)length + 1u);
    assert(text != NULL);
    bytes_read = fread(text, 1u, (size_t)length, file);
    assert(bytes_read == (size_t)length);
    text[bytes_read] = '\0';
    return text;
}

static void test_structural_import_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirModuleId child_id;
    CmHirModuleId invalid_module_id;
    CmHirModuleId primitive_module_id;
    CmHirModuleId extern_module_id;
    CmHirDefId item_definition;
    CmHirImport imports[2];
    CmHirImportBinding bindings[4];
    CmHirAttribute attributes[1];
    const CmHirModule *root;
    const CmHirModule *child;
    size_t item_count;
    size_t definition_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "import_model"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &crate_id, &root_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "child"), test_span(10u, 20u),
        &child_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "invalid"), test_span(21u, 30u),
        &invalid_module_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "primitive"), test_span(31u, 40u),
        &primitive_module_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "externs"), test_span(41u, 50u),
        &extern_module_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(51u, 60u), &item_definition) == CM_HIR_OK);
    child = cm_hir_get_module(&context, child_id);
    root = cm_hir_get_module(&context, root_id);
    assert(child != NULL && root != NULL);

    memset(attributes, 0, sizeof(attributes));
    attributes[0].metadata = cm_hir_intern(&context,
        "stable(feature = \"rust1\", since = \"1.0.0\")");
    attributes[0].span = test_span(1u, 9u);
    attributes[0].source_attribute = 7u;
    memset(bindings, 0, sizeof(bindings));
    bindings[0].name = cm_hir_intern(&context, "Child");
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[0].target = child->definition;
    bindings[0].is_public = 1;
    bindings[0].is_crate_visible = 1;
    bindings[1].name = cm_hir_intern(&context, "_");
    bindings[1].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[1].target = child->definition;
    bindings[1].is_anonymous = 1;
    bindings[1].is_public = 1;
    bindings[1].is_crate_visible = 1;
    bindings[2].name = bindings[1].name;
    bindings[2].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[2].target = item_definition;
    bindings[2].is_anonymous = 1;
    bindings[2].is_public = 1;
    bindings[2].is_crate_visible = 1;
    bindings[3].name = cm_hir_intern(&context, "value");
    bindings[3].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    bindings[3].target = item_definition;
    bindings[3].is_crate_visible = 1;
    memset(imports, 0, sizeof(imports));
    imports[0].tree = cm_hir_intern(&context, "self::child as Child");
    imports[0].visibility.kind = CM_HIR_VIS_PUBLIC;
    imports[0].visibility.restriction = cm_hir_def_id_none();
    imports[0].span = test_span(1u, 20u);
    imports[0].source_item = 3u;
    imports[0].attributes = attributes;
    imports[0].attribute_count = 1u;
    imports[0].bindings = &bindings[0];
    imports[0].binding_count = 3u;
    imports[1].tree = cm_hir_intern(&context, "self::value");
    imports[1].visibility.kind = CM_HIR_VIS_RESTRICTED;
    imports[1].visibility.restriction = root->definition;
    imports[1].span = test_span(41u, 50u);
    imports[1].source_item = 4u;
    imports[1].bindings = &bindings[3];
    imports[1].binding_count = 1u;
    item_count = context.items.len;
    definition_count = context.definitions.len;
    assert(cm_hir_set_module_imports(&context, root_id, NULL, 0u)
        == CM_HIR_OK);
    assert(cm_hir_set_module_imports(&context, root_id, imports, 2u)
        == CM_HIR_OK);
    assert(context.items.len == item_count
        && context.definitions.len == definition_count);
    attributes[0].source_attribute = 99u;
    bindings[0].name = CM_INTERN_ID_NONE;
    imports[0].source_item = 99u;
    root = cm_hir_get_module(&context, root_id);
    assert(root != NULL && root->import_count == 2u
        && root->imports != imports
        && root->imports[0].attributes != attributes
        && root->imports[0].bindings != bindings
        && root->imports[0].source_item == 3u
        && root->imports[0].attributes[0].source_attribute == 7u
        && root->imports[0].bindings[0].name != CM_INTERN_ID_NONE
        && root->imports[0].bindings[1].is_anonymous
        && root->imports[0].bindings[2].is_anonymous
        && root->imports[0].bindings[1].name
            == root->imports[0].bindings[2].name
        && cm_hir_def_id_equal(root->imports[1].bindings[0].target,
            item_definition));
    assert(cm_hir_set_module_imports(&context, root_id, imports, 2u)
        == CM_HIR_INVARIANT_VIOLATION);

    imports[0] = root->imports[0];
    imports[0].tree = cm_hir_intern(&context, "u8");
    imports[0].bindings = bindings;
    imports[0].binding_count = 1u;
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].name = cm_hir_intern(&context, "u8");
    bindings[0].target = cm_hir_def_id_none();
    bindings[0].primitive_kind = CM_HIR_PRIMITIVE_U8;
    assert(cm_hir_set_module_imports(&context, primitive_module_id,
        imports, 1u) == CM_HIR_OK);
    child = cm_hir_get_module(&context, primitive_module_id);
    assert(child != NULL && child->import_count == 1u
        && child->imports[0].binding_count == 1u
        && child->imports[0].bindings[0].primitive_kind
            == CM_HIR_PRIMITIVE_U8
        && cm_hir_def_id_is_none(child->imports[0].bindings[0].target));

    imports[0] = root->imports[0];
    imports[0].kind = CM_HIR_IMPORT_EXTERN_CRATE;
    imports[0].tree = cm_hir_intern(&context, "self");
    imports[0].bindings = bindings;
    imports[0].binding_count = 1u;
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].name = cm_hir_intern(&context, "local_core");
    bindings[0].target = root->definition;
    assert(cm_hir_set_module_imports(&context, extern_module_id,
        imports, 1u) == CM_HIR_OK);
    child = cm_hir_get_module(&context, extern_module_id);
    assert(child != NULL && child->import_count == 1u
        && child->imports[0].kind == CM_HIR_IMPORT_EXTERN_CRATE
        && child->imports[0].binding_count == 1u
        && cm_hir_def_id_equal(child->imports[0].bindings[0].target,
            root->definition));

    imports[0] = root->imports[0];
    imports[0].tree = CM_INTERN_ID_NONE;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].kind = (CmHirImportKind)(
        (unsigned int)CM_HIR_IMPORT_EXTERN_CRATE + 1u);
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].kind = CM_HIR_IMPORT_EXTERN_CRATE;
    imports[0].binding_count = 0u;
    imports[0].bindings = NULL;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].source_item = 0u;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].span.source = 0u;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].visibility.kind = CM_HIR_VIS_RESTRICTED;
    imports[0].visibility.restriction = child->definition;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].bindings = bindings;
    imports[0].binding_count = 1u;
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].is_public = 2;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].is_public = 1;
    bindings[0].is_crate_visible = 0;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].is_crate_visible = 2;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].is_anonymous = 1;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0] = root->imports[0].bindings[1];
    bindings[0].is_anonymous = 0;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    imports[0] = root->imports[0];
    imports[0].bindings = bindings;
    bindings[0] = root->imports[0].bindings[0];
    bindings[0].target = cm_hir_def_id_none();
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ID);
    bindings[0].primitive_kind = CM_HIR_PRIMITIVE_U8;
    bindings[0].target = child->definition;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0].target = cm_hir_def_id_none();
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_MACRO;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[0].primitive_kind =
        (CmHirPrimitiveKind)((unsigned int)CM_HIR_PRIMITIVE_F128 + 1u);
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0].name = cm_hir_intern(&context, "_");
    bindings[0].primitive_kind = CM_HIR_PRIMITIVE_U8;
    bindings[0].is_anonymous = 1;
    assert(cm_hir_set_module_imports(&context, invalid_module_id,
        imports, 1u) == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_set_module_imports(&context, UINT32_MAX,
        imports, 1u) == CM_HIR_INVALID_ID);
    child = cm_hir_get_module(&context, invalid_module_id);
    assert(child != NULL && child->import_count == 0u
        && child->imports == NULL);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "import module#1 index=0 source-item=3 visibility=public")
        != NULL);
    assert(strstr(dump,
        "import-attr module#1 index=0 source-attr=7 depth=0")
        != NULL);
    assert(strstr(dump,
        "import-binding module#1 index=1 binding=0 namespace=value")
        != NULL);
    assert(strstr(dump,
        "anonymous=0 public=0 crate-visible=1") != NULL);
    assert(strstr(dump, "kind=extern-crate tree=\"self\"") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void init_test_item(CmHirItem *item, CmHirItemKind kind,
    CmHirDefId definition, CmHirModuleId module,
    CmHirDefId parent_definition, CmInternId name, CmSpan span)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = parent_definition;
    item->name = name;
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = span;
}

static void test_enum_variant_definition_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirModuleId invalid_value_id;
    CmHirModuleId invalid_macro_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirDefId enum_definition;
    CmHirDefId variant_definitions[3];
    CmHirField tuple_field;
    CmHirField named_field;
    CmHirVariant variants[3];
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirItem *stored;
    CmHirImportBinding bindings[5];
    CmHirImport import_value;
    const CmHirDefinition *definition;
    FILE *dump_file;
    char *dump;
    uint32_t index;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "variant_model"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &crate_id, &root_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "invalid_value"), test_span(1u, 2u),
        &invalid_value_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "invalid_macro"), test_span(3u, 4u),
        &invalid_macro_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(5u, 6u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_ENUM, test_span(10u, 60u), &enum_definition)
        == CM_HIR_OK);
    for (index = 0u; index < 3u; ++index) {
        assert(cm_hir_reserve_enum_variant_definition(&context, crate_id,
            test_span(20u + index * 10u, 29u + index * 10u),
            &variant_definitions[index]) == CM_HIR_OK);
    }
    memset(&tuple_field, 0, sizeof(tuple_field));
    tuple_field.type = u8_type;
    tuple_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    tuple_field.span = test_span(30u, 39u);
    memset(&named_field, 0, sizeof(named_field));
    named_field.name = cm_hir_intern(&context, "value");
    named_field.type = u8_type;
    named_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    named_field.span = test_span(40u, 49u);
    memset(variants, 0, sizeof(variants));
    variants[0].definition = variant_definitions[0];
    variants[0].name = cm_hir_intern(&context, "Unit");
    variants[0].form = CM_HIR_AGGREGATE_UNIT;
    variants[0].span = test_span(20u, 29u);
    variants[1].definition = variant_definitions[1];
    variants[1].name = cm_hir_intern(&context, "Tuple");
    variants[1].form = CM_HIR_AGGREGATE_TUPLE;
    variants[1].fields = &tuple_field;
    variants[1].field_count = 1u;
    variants[1].span = test_span(30u, 39u);
    variants[2].definition = variant_definitions[2];
    variants[2].name = cm_hir_intern(&context, "Named");
    variants[2].form = CM_HIR_AGGREGATE_NAMED;
    variants[2].fields = &named_field;
    variants[2].field_count = 1u;
    variants[2].span = test_span(40u, 49u);
    init_test_item(&item, CM_HIR_ITEM_ENUM, enum_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Choice"),
        test_span(10u, 60u));
    item.data.enum_item.variants = variants;
    item.data.enum_item.variant_count = 3u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL && stored->kind == CM_HIR_ITEM_ENUM
        && stored->data.enum_item.variant_count == 3u);
    for (index = 0u; index < 3u; ++index) {
        definition = cm_hir_lookup_definition(&context,
            stored->data.enum_item.variants[index].definition);
        assert(definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && definition->state == CM_HIR_DEFINITION_BOUND
            && definition->entity.enum_variant.enum_item_id == item_id
            && definition->entity.enum_variant.variant_index == index);
    }
    memset(bindings, 0, sizeof(bindings));
    bindings[0].name = cm_hir_intern(&context, "Unit");
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[0].target = variant_definitions[0];
    bindings[1] = bindings[0];
    bindings[1].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    bindings[2].name = cm_hir_intern(&context, "Tuple");
    bindings[2].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    bindings[2].target = variant_definitions[1];
    bindings[3] = bindings[2];
    bindings[3].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[4].name = cm_hir_intern(&context, "Named");
    bindings[4].namespace_kind = CM_HIR_NAMESPACE_TYPE;
    bindings[4].target = variant_definitions[2];
    memset(&import_value, 0, sizeof(import_value));
    import_value.tree = cm_hir_intern(&context, "Choice::*");
    import_value.visibility.kind = CM_HIR_VIS_PRIVATE;
    import_value.span = test_span(61u, 70u);
    import_value.source_item = 1u;
    import_value.bindings = bindings;
    import_value.binding_count = 5u;
    assert(cm_hir_set_module_imports(&context, root_id, &import_value, 1u)
        == CM_HIR_OK);
    bindings[0].target = variant_definitions[2];
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_VALUE;
    import_value.bindings = &bindings[0];
    import_value.binding_count = 1u;
    assert(cm_hir_set_module_imports(&context, invalid_value_id,
        &import_value, 1u) == CM_HIR_INVALID_ARGUMENT);
    bindings[0].target = variant_definitions[1];
    bindings[0].namespace_kind = CM_HIR_NAMESPACE_MACRO;
    assert(cm_hir_set_module_imports(&context, invalid_macro_id,
        &import_value, 1u) == CM_HIR_INVALID_ARGUMENT);
    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump, "enum-variant bound enum-item#1 variant=0")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_macro_definition_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirModuleId invalid_namespace_id;
    CmHirDefId macro_definition;
    CmHirImportBinding binding;
    CmHirImport import_value;
    const CmHirDefinition *definition;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "macro_model"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &crate_id, &root_id) == CM_HIR_OK);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "invalid_namespace"),
        test_span(1u, 2u), &invalid_namespace_id) == CM_HIR_OK);
    assert(cm_hir_add_macro_definition(&context, root_id,
        cm_hir_intern(&context, "Copy"),
        CM_HIR_MACRO_RULES_DEFINITION, test_span(10u, 30u),
        &macro_definition) == CM_HIR_OK);
    definition = cm_hir_lookup_definition(&context, macro_definition);
    assert(definition != NULL
        && definition->kind == CM_HIR_DEFINITION_MACRO
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->entity.macro_definition.owner_module == root_id
        && definition->entity.macro_definition.form
            == CM_HIR_MACRO_RULES_DEFINITION
        && definition->entity.macro_definition.name
            == cm_hir_intern(&context, "Copy"));
    assert(cm_hir_add_macro_definition(&context, CM_HIR_MODULE_NONE,
        cm_hir_intern(&context, "Bad"),
        CM_HIR_MACRO_RULES_DEFINITION, test_span(31u, 32u),
        &binding.target) == CM_HIR_INVALID_ID);
    assert(cm_hir_add_macro_definition(&context, root_id,
        cm_hir_intern(&context, "Bad"),
        (CmHirMacroDefinitionForm)99, test_span(31u, 32u),
        &binding.target) == CM_HIR_INVALID_ARGUMENT);
    memset(&binding, 0, sizeof(binding));
    binding.name = cm_hir_intern(&context, "Copy");
    binding.namespace_kind = CM_HIR_NAMESPACE_MACRO;
    binding.target = macro_definition;
    memset(&import_value, 0, sizeof(import_value));
    import_value.tree = cm_hir_intern(&context, "crate::Copy");
    import_value.visibility.kind = CM_HIR_VIS_PRIVATE;
    import_value.span = test_span(40u, 50u);
    import_value.source_item = 1u;
    import_value.bindings = &binding;
    import_value.binding_count = 1u;
    assert(cm_hir_set_module_imports(&context, root_id, &import_value, 1u)
        == CM_HIR_OK);
    binding.namespace_kind = CM_HIR_NAMESPACE_TYPE;
    assert(cm_hir_set_module_imports(&context, invalid_namespace_id,
        &import_value, 1u) == CM_HIR_INVALID_ARGUMENT);
    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "macro bound module#1 form=macro-rules name=\"Copy\"")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static CmHirStatus add_test_trait_method(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module,
    CmHirDefId trait_definition, const char *item_name,
    CmHirReceiverKind receiver, const char *parameter_name,
    CmHirTypeId parameter_type, CmHirTypeId return_type, CmInternId abi)
{
    CmHirDefId definition;
    CmHirFunctionParameter parameter;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(1u, 2u), &definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(context, parameter_name);
    parameter.type = parameter_type;
    parameter.span = test_span(1u, 2u);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, module,
        trait_definition, cm_hir_intern(context, item_name),
        test_span(1u, 2u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = receiver;
    item.data.function_item.signature.return_type = return_type;
    item.data.function_item.signature.abi = abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    return cm_hir_add_item(context, &item, &item_id);
}

static void test_known_trait_projection_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId u8_type;
    CmHirTypeId usize_type;
    CmHirTypeId projection_id;
    CmHirTypeId rejected_type_id;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId other_trait_definition;
    CmHirDefId other_associated_definition;
    CmHirDefId struct_definition;
    CmHirDefId arbitrary_child_definition;
    CmHirDefId self_parent_definition;
    CmHirDefId impl_definition;
    CmHirDefId negative_impl_definition;
    CmHirDefId inherent_impl_definition;
    CmHirDefId unsafe_impl_definition;
    CmHirDefId impl_alias_definition;
    CmHirDefId targetless_impl_alias_definition;
    CmHirDefId duplicate_impl_alias_definition;
    CmHirDefId trait_target_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId trait_parameter;
    CmHirGenericParamId associated_parameter;
    CmHirGenericParamId impl_associated_parameter;
    CmHirGenericParamId duplicate_impl_associated_parameter;
    CmHirGenericArg trait_arguments[1];
    CmHirGenericArg associated_arguments[1];
    CmHirType type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId trait_item_id;
    CmHirItemId associated_item_id;
    const CmHirType *stored_projection;
    const CmHirItem *stored_associated;
    const CmHirItem *stored_impl;
    const CmHirDefinition *stored_definition;
    size_t arena_bytes;
    size_t item_count;
    size_t type_count;
    size_t definition_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "projection_test"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &crate_id, &root_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(4u, 9u);
    type.data.integer_type.kind = CM_HIR_INT_USIZE;
    assert(cm_hir_add_type(&context, &type, &usize_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 80u), &trait_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = trait_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(20u, 21u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &trait_parameter) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Trait"),
        test_span(10u, 80u));
    item.generic_parameter_start = trait_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &trait_item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(30u, 40u), &associated_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = associated_definition;
    parameter.name = cm_hir_intern(&context, "U");
    parameter.span = test_span(34u, 35u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &associated_parameter) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_id, trait_definition, cm_hir_intern(&context, "Assoc"),
        test_span(30u, 40u));
    item.generic_parameter_start = associated_parameter;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &associated_item_id) == CM_HIR_OK);
    stored_associated = cm_hir_get_item(&context, associated_item_id);
    assert(stored_associated != NULL
        && stored_associated->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_is_none(stored_associated->data.type_alias_item
                .trait_item_definition)
        && cm_hir_def_id_equal(stored_associated->parent_definition,
            trait_definition));

    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = u8_type;
    memset(associated_arguments, 0, sizeof(associated_arguments));
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    associated_arguments[0].data.type = usize_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(90u, 120u);
    type.data.projection_type.self_type = u8_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    type.data.projection_type.associated_type.arguments = associated_arguments;
    type.data.projection_type.associated_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &projection_id) == CM_HIR_OK);
    trait_arguments[0].data.type = CM_HIR_TYPE_NONE;
    associated_arguments[0].data.type = CM_HIR_TYPE_NONE;
    stored_projection = cm_hir_get_type(&context, projection_id);
    assert(stored_projection != NULL
        && stored_projection->data.projection_type.trait_type.arguments
            != trait_arguments
        && stored_projection->data.projection_type.associated_type.arguments
            != associated_arguments
        && stored_projection->data.projection_type.trait_type.arguments[0]
            .data.type == u8_type
        && stored_projection->data.projection_type.associated_type.arguments[0]
            .data.type == usize_type);

    trait_arguments[0].data.type = u8_type;
    associated_arguments[0].data.type = usize_type;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    type_count = context.types.len;
    type.data.projection_type.trait_type.argument_count = 0u;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.argument_count = 0u;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    type.data.projection_type.associated_type.argument_count = 1u;
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    trait_arguments[0].data.lifetime.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = u8_type;
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    associated_arguments[0].data.lifetime.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    associated_arguments[0].data.type = usize_type;
    assert(rejected_type_id == CM_HIR_TYPE_NONE
        && context.types.len == type_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 150u), &other_trait_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, other_trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "OtherTrait"),
        test_span(121u, 150u));
    item.data.trait_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(130u, 140u), &other_associated_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        other_associated_definition, root_id, other_trait_definition,
        cm_hir_intern(&context, "Assoc"), test_span(130u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    trait_arguments[0].data.type = u8_type;
    associated_arguments[0].data.type = usize_type;
    type.data.projection_type.associated_type.definition =
        other_associated_definition;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    type_count = context.types.len;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    assert(rejected_type_id == CM_HIR_TYPE_NONE);
    assert(context.types.len == type_count);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);

    type.data.projection_type.associated_type.definition =
        associated_definition;
    type.data.projection_type.trait_type.definition = associated_definition;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.associated_type.definition = trait_definition;
    assert(cm_hir_add_type(&context, &type, &rejected_type_id)
        == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(151u, 170u), &struct_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Container"),
        test_span(151u, 170u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(155u, 160u), &arbitrary_child_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        arbitrary_child_definition, root_id, struct_definition,
        cm_hir_intern(&context, "ArbitraryChild"), test_span(155u, 160u));
    item.data.type_alias_item.target = u8_type;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count);
    assert(cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        arbitrary_child_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(171u, 180u), &self_parent_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, self_parent_definition,
        root_id, self_parent_definition, cm_hir_intern(&context, "SelfParent"),
        test_span(171u, 180u));
    item.data.type_alias_item.target = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);

    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, cm_hir_def_id_none(),
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Targetless"),
        test_span(181u, 190u));
    item.is_specializable = 1;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    definition_count = context.definitions.len;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE
        && context.definitions.len == definition_count);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(185u, 190u), &trait_target_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, trait_target_definition,
        root_id, trait_definition, cm_hir_intern(&context, "Defaulted"),
        test_span(185u, 190u));
    item.is_specializable = 1;
    item.data.type_alias_item.target = u8_type;
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count);

    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, cm_hir_def_id_none(),
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "LinkedFree"),
        test_span(186u, 190u));
    item.data.type_alias_item.target = u8_type;
    item.data.type_alias_item.trait_item_definition = associated_definition;
    definition_count = context.definitions.len;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE
        && context.definitions.len == definition_count);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(191u, 230u), &impl_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(231u, 260u), &negative_impl_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, impl_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NamedImpl"),
        test_span(191u, 230u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.trait_type.arguments = trait_arguments;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVALID_ARGUMENT);
    item.name = CM_INTERN_ID_NONE;
    item.data.impl_item.has_trait = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.polarity = CM_HIR_IMPL_NEGATIVE;
    item.definition = negative_impl_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item.definition = impl_definition;
    item.data.impl_item.polarity = CM_HIR_IMPL_POSITIVE;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    item.data.impl_item.trait_type.definition = struct_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context, impl_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.trait_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.trait_type.argument_count = 1u;
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    trait_arguments[0].data.lifetime.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored_impl = cm_hir_get_item(&context, item_id);
    assert(stored_impl != NULL && stored_impl->name == CM_INTERN_ID_NONE
        && stored_impl->data.impl_item.has_trait == 1
        && stored_impl->data.impl_item.polarity == CM_HIR_IMPL_POSITIVE
        && cm_hir_def_id_equal(
            stored_impl->data.impl_item.trait_type.definition,
            trait_definition)
        && stored_impl->data.impl_item.trait_type.arguments
            != trait_arguments
        && stored_impl->data.impl_item.trait_type.argument_count == 1u
        && stored_impl->data.impl_item.trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && stored_impl->data.impl_item.trait_type.arguments[0].data.type
            == u8_type);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(191u, 225u), &inherent_impl_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, inherent_impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(191u, 225u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 0;
    item.data.impl_item.is_const = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.is_const = 0;
    item.data.impl_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.trait_type.definition = trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.trait_type.definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored_impl = cm_hir_get_item(&context, item_id);
    assert(stored_impl != NULL
        && stored_impl->data.impl_item.has_trait == 0
        && stored_impl->data.impl_item.safety == CM_HIR_SAFE
        && cm_hir_def_id_is_none(
            stored_impl->data.impl_item.trait_type.definition)
        && stored_impl->data.impl_item.trait_type.arguments == NULL
        && stored_impl->data.impl_item.trait_type.argument_count == 0u);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(192u, 230u), &unsafe_impl_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, unsafe_impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(192u, 230u));
    item.data.impl_item.self_type = usize_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = other_trait_definition;
    item.data.impl_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(200u, 210u), &targetless_impl_alias_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        targetless_impl_alias_definition, root_id, impl_definition,
        cm_hir_intern(&context, "Assoc"), test_span(200u, 210u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.trait_item_definition = associated_definition;
    item.parent_definition = negative_impl_definition;
    item.is_specializable = 1;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    item.parent_definition = inherent_impl_definition;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    item.parent_definition = impl_definition;
    item.is_specializable = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(211u, 220u), &impl_alias_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, impl_alias_definition,
        root_id, impl_definition, cm_hir_intern(&context, "Assoc"),
        test_span(211u, 220u));
    item.data.type_alias_item.target = usize_type;
    item.data.type_alias_item.trait_item_definition =
        other_associated_definition;
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.type_alias_item.trait_item_definition = associated_definition;
    item.name = cm_hir_intern(&context, "WrongName");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count);
    item.name = cm_hir_intern(&context, "Assoc");
    item.is_specializable = 2;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVALID_ARGUMENT);
    item.is_specializable = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = impl_alias_definition;
    parameter.name = cm_hir_intern(&context, "V");
    parameter.span = test_span(214u, 215u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &impl_associated_parameter) == CM_HIR_OK);
    item.generic_parameter_start = impl_associated_parameter;
    item.generic_parameter_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored_associated = cm_hir_get_item(&context, item_id);
    assert(stored_associated != NULL
        && stored_associated->is_specializable == 1);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(221u, 225u), &duplicate_impl_alias_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        duplicate_impl_alias_definition, root_id, impl_definition,
        cm_hir_intern(&context, "Assoc"), test_span(221u, 225u));
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = duplicate_impl_alias_definition;
    parameter.name = cm_hir_intern(&context, "V");
    parameter.span = test_span(222u, 223u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &duplicate_impl_associated_parameter) == CM_HIR_OK);
    item.generic_parameter_start = duplicate_impl_associated_parameter;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = u8_type;
    item.data.type_alias_item.trait_item_definition = trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.type_alias_item.trait_item_definition = associated_definition;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        duplicate_impl_alias_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "projection <ty#1 as 1:2<ty#1>>::1:3<ty#2>") != NULL);
    assert(strstr(dump, "impl def=") != NULL
        && strstr(dump, "name=none") != NULL
        && strstr(dump, "name=\"Assoc\" trait-item=1:3") != NULL
        && strstr(dump, "specializable=1") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_method_and_item_attribute_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId trait_self_type;
    CmHirTypeId trait_self_ref_type;
    CmHirTypeId impl_self_type;
    CmHirTypeId impl_self_ref_type;
    CmHirTypeId rejected_type;
    CmHirDefId trait_definition;
    CmHirDefId trait_method_definition;
    CmHirDefId impl_definition;
    CmHirDefId impl_method_definition;
    CmHirDefId duplicate_method_definition;
    CmHirDefId free_function_definition;
    CmHirDefId invalid_attribute_definition;
    CmHirDefId struct_definition;
    CmHirDefId cross_owner_default_definition;
    CmHirDefId mismatched_kind_definition;
    CmHirGenericParam cross_owner_parameter;
    CmHirGenericParamId cross_owner_parameter_id;
    CmHirGenericArg cross_owner_default;
    CmHirGenericParam self_default_parameter;
    CmHirGenericParamId self_default_parameter_id;
    CmHirGenericArg self_default;
    CmHirGenericArg impl_trait_argument;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId trait_method_item_id;
    CmHirItemId impl_method_item_id;
    CmHirBody body;
    CmHirBodyId impl_method_body_id;
    CmHirLocal body_locals[1];
    CmHirFunctionParameter parameters[1];
    CmHirAttribute attributes[2];
    const CmHirItem *stored;
    const CmHirDefinition *definition;
    CmInternId rust_abi;
    CmInternId method_name;
    size_t arena_bytes;
    size_t item_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "method_model"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &crate_id, &root_id) == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    rust_abi = cm_hir_intern(&context, "Rust");
    method_name = cm_hir_intern(&context, "visit");

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        (CmHirItemKind)99, test_span(10u, 80u), &trait_definition)
        == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_def_id_is_none(trait_definition));
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 80u), &trait_definition)
        == CM_HIR_OK);
    definition = cm_hir_lookup_definition(&context, trait_definition);
    assert(definition != NULL && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_TRAIT);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(20u, 24u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&context, &type, &trait_self_type) == CM_HIR_OK);
    memset(&self_default_parameter, 0, sizeof(self_default_parameter));
    self_default_parameter.kind = CM_HIR_GENERIC_TYPE;
    self_default_parameter.owner = trait_definition;
    self_default_parameter.name = cm_hir_intern(&context, "Rhs");
    self_default_parameter.span = test_span(15u, 18u);
    assert(cm_hir_add_generic_param(&context, &self_default_parameter,
        &self_default_parameter_id) == CM_HIR_OK);
    memset(&self_default, 0, sizeof(self_default));
    self_default.kind = CM_HIR_GENERIC_ARG_TYPE;
    self_default.data.type = trait_self_type;
    assert(cm_hir_set_generic_param_default(&context,
        self_default_parameter_id, &self_default) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Visitor"),
        test_span(10u, 80u));
    item.generic_parameter_start = self_default_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_get_generic_param(&context, self_default_parameter_id)
            ->has_default
        && cm_hir_get_generic_param(&context, self_default_parameter_id)
            ->default_argument.data.type == trait_self_type);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(81u, 89u), &cross_owner_default_definition)
        == CM_HIR_OK);
    memset(&cross_owner_parameter, 0, sizeof(cross_owner_parameter));
    cross_owner_parameter.kind = CM_HIR_GENERIC_TYPE;
    cross_owner_parameter.owner = cross_owner_default_definition;
    cross_owner_parameter.name = cm_hir_intern(&context, "T");
    cross_owner_parameter.span = test_span(82u, 83u);
    assert(cm_hir_add_generic_param(&context, &cross_owner_parameter,
        &cross_owner_parameter_id) == CM_HIR_OK);
    memset(&cross_owner_default, 0, sizeof(cross_owner_default));
    cross_owner_default.kind = CM_HIR_GENERIC_ARG_TYPE;
    cross_owner_default.data.type = trait_self_type;
    assert(cm_hir_set_generic_param_default(&context,
        cross_owner_parameter_id, &cross_owner_default)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(!cm_hir_get_generic_param(&context,
        cross_owner_parameter_id)->has_default);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(20u, 25u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = trait_self_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &trait_self_ref_type)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(30u, 50u),
        &trait_method_definition) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(30u, 31u);
    type.data.self_type.owner = trait_method_definition;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    assert(rejected_type == CM_HIR_TYPE_NONE);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(51u, 59u),
        &mismatched_kind_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION,
        mismatched_kind_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "wrong_reserved_kind"),
        test_span(51u, 59u));
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    definition = cm_hir_lookup_definition(&context,
        mismatched_kind_definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED);

    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&context, "self");
    parameters[0].type = trait_self_ref_type;
    parameters[0].span = test_span(32u, 37u);
    memset(attributes, 0, sizeof(attributes));
    attributes[0].metadata = cm_hir_intern(&context, "inline");
    attributes[0].span = test_span(30u, 31u);
    attributes[0].source_attribute = 11u;
    attributes[0].expansion_depth = 0u;
    attributes[1].metadata = cm_hir_intern(&context, "track_caller");
    attributes[1].span = test_span(31u, 32u);
    attributes[1].source_attribute = 12u;
    attributes[1].expansion_depth = 1u;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, trait_method_definition,
        root_id, trait_definition, method_name, test_span(30u, 50u));
    item.attributes = attributes;
    item.attribute_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_REF_SHARED;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.signature.is_async = 1;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = (CmHirReceiverKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_REF_SHARED;
    item.data.function_item.trait_item_definition = trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    item.data.function_item.has_default_body = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &trait_method_item_id)
        == CM_HIR_OK);
    stored = cm_hir_get_item(&context, trait_method_item_id);
    assert(stored != NULL
        && stored->attributes != attributes
        && stored->attribute_count == 2u
        && stored->attributes[0].metadata == attributes[0].metadata
        && stored->data.function_item.signature.parameters != parameters
        && stored->data.function_item.signature.parameters[0].type
            == trait_self_ref_type
        && stored->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_SHARED
        && stored->data.function_item.signature.is_async
        && stored->data.function_item.has_default_body == 1
        && stored->data.function_item.body == CM_HIR_BODY_NONE
        && cm_hir_def_id_is_none(
            stored->data.function_item.trait_item_definition));
    attributes[0].metadata = CM_INTERN_ID_NONE;
    parameters[0].type = CM_HIR_TYPE_NONE;
    assert(stored->attributes[0].metadata != CM_INTERN_ID_NONE
        && stored->data.function_item.signature.parameters[0].type
            == trait_self_ref_type);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(90u, 150u), &impl_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(90u, 150u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    memset(&impl_trait_argument, 0, sizeof(impl_trait_argument));
    impl_trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    impl_trait_argument.data.type = u8_type;
    item.data.impl_item.trait_type.arguments = &impl_trait_argument;
    item.data.impl_item.trait_type.argument_count = 1u;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(100u, 104u);
    type.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&context, &type, &impl_self_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(100u, 105u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = impl_self_type;
    type.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &impl_self_ref_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(110u, 130u), &impl_method_definition) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&context, "self");
    parameters[0].type = impl_self_ref_type;
    parameters[0].span = test_span(112u, 117u);
    memset(body_locals, 0, sizeof(body_locals));
    body_locals[0].name = cm_hir_intern(&context, "self");
    body_locals[0].type = impl_self_ref_type;
    body_locals[0].span = test_span(112u, 117u);
    memset(&body, 0, sizeof(body));
    body.owner = impl_method_definition;
    body.origin = cm_hir_body_origin_item_source(impl_method_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u8_type;
    body.locals = body_locals;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(117u, 129u);
    assert(cm_hir_add_body(&context, &body, &impl_method_body_id)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, impl_method_definition,
        root_id, impl_definition, method_name, test_span(110u, 130u));
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_REF_MUTABLE;
    item.data.function_item.signature.return_type = u8_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.trait_item_definition = trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.trait_item_definition = trait_method_definition;
    item.name = cm_hir_intern(&context, "wrong_name");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.name = method_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.body = impl_method_body_id;
    parameters[0].type = trait_self_ref_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].type = impl_self_ref_type;
    item.data.function_item.signature.return_type = trait_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.return_type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.is_async = 1;
    item.data.function_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.has_default_body = 0;
    assert(cm_hir_add_item(&context, &item, &impl_method_item_id)
        == CM_HIR_OK);
    stored = cm_hir_get_item(&context, impl_method_item_id);
    assert(stored != NULL
        && stored->data.function_item.signature.receiver
            == CM_HIR_RECEIVER_REF_MUTABLE
        && stored->data.function_item.signature.return_type == u8_type
        && cm_hir_def_id_equal(
            stored->data.function_item.trait_item_definition,
            trait_method_definition));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(131u, 140u), &duplicate_method_definition) == CM_HIR_OK);
    item.definition = duplicate_method_definition;
    item.span = test_span(131u, 140u);
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_CUSTOM;
    item.data.function_item.signature.return_type = unit_type;
    item_count = context.items.len;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(160u, 170u), &free_function_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, free_function_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "free"),
        test_span(160u, 170u));
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].type = u8_type;
    item.data.function_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.has_default_body = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(171u, 180u), &invalid_attribute_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, invalid_attribute_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "bad_attrs"),
        test_span(171u, 180u));
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.attribute_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVALID_ARGUMENT);
    assert(context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    item.attributes = attributes;
    attributes[0].metadata = cm_hir_intern(&context, "cold");
    attributes[0].span = test_span(171u, 172u);
    attributes[0].source_attribute = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVALID_ARGUMENT);
    definition = cm_hir_lookup_definition(&context,
        invalid_attribute_definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(181u, 190u), &struct_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NotSelf"),
        test_span(181u, 190u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(182u, 186u);
    type.data.self_type.owner = struct_definition;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strncmp(dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(strstr(dump, "Self(owner=") != NULL);
    assert(strstr(dump, "receiver=ref-shared") != NULL);
    assert(strstr(dump, "receiver=ref-mutable") != NULL);
    assert(strstr(dump, "default-body=1") != NULL);
    assert(strstr(dump, "item-attr item#") != NULL);
    assert(strstr(dump, "meta=\"inline\"") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_scoped_self_and_receiver_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId impl_definition;
    CmHirDefId rejected_definition;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId trait_self_type;
    CmHirTypeId shared_self_type;
    CmHirTypeId mutable_self_type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirField field;
    CmInternId rust_abi;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "public_method_invariants"),
        CM_HIR_EDITION_2024, test_span(0u, 300u), &crate_id,
        &root_id) == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    rust_abi = cm_hir_intern(&context, "Rust");

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 100u), &trait_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "ReceiverTrait"),
        test_span(10u, 100u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(11u, 15u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&context, &type, &trait_self_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(11u, 16u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = trait_self_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &shared_self_type) == CM_HIR_OK);
    type.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &mutable_self_type) == CM_HIR_OK);

    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_value_type", CM_HIR_RECEIVER_VALUE,
        "self", u8_type, unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_value_name", CM_HIR_RECEIVER_VALUE,
        "other", trait_self_type, unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "value", CM_HIR_RECEIVER_VALUE,
        "self", trait_self_type, unit_type, rust_abi) == CM_HIR_OK);

    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_shared_shape", CM_HIR_RECEIVER_REF_SHARED,
        "self", trait_self_type, unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_shared_mutability",
        CM_HIR_RECEIVER_REF_SHARED, "self", mutable_self_type,
        unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_shared_name", CM_HIR_RECEIVER_REF_SHARED,
        "other", shared_self_type, unit_type, rust_abi)
        == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "shared", CM_HIR_RECEIVER_REF_SHARED,
        "self", shared_self_type, unit_type, rust_abi) == CM_HIR_OK);

    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_mutable_shape", CM_HIR_RECEIVER_REF_MUTABLE,
        "self", trait_self_type, unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_mutable_mutability",
        CM_HIR_RECEIVER_REF_MUTABLE, "self", shared_self_type,
        unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_mutable_name", CM_HIR_RECEIVER_REF_MUTABLE,
        "other", mutable_self_type, unit_type, rust_abi)
        == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "mutable", CM_HIR_RECEIVER_REF_MUTABLE,
        "self", mutable_self_type, unit_type, rust_abi) == CM_HIR_OK);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "bad_custom", CM_HIR_RECEIVER_CUSTOM,
        "self", u8_type, unit_type, rust_abi) == CM_HIR_INVALID_ID);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "custom_value", CM_HIR_RECEIVER_CUSTOM,
        "self", trait_self_type, unit_type, rust_abi) == CM_HIR_OK);
    assert(add_test_trait_method(&context, crate_id, root_id,
        trait_definition, "custom_reference", CM_HIR_RECEIVER_CUSTOM,
        "self", mutable_self_type, unit_type, rust_abi) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(110u, 120u), &rejected_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "BadAlias"),
        test_span(110u, 120u));
    item.data.type_alias_item.target = trait_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 140u), &rejected_definition) == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.name = cm_hir_intern(&context, "bad");
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.type = trait_self_type;
    field.span = test_span(125u, 130u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, rejected_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "BadAggregate"),
        test_span(121u, 140u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(141u, 150u), &associated_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_id, trait_definition, cm_hir_intern(&context, "Assoc"),
        test_span(141u, 150u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(151u, 200u), &impl_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(151u, 200u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(160u, 170u), &rejected_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, impl_definition, cm_hir_intern(&context, "Assoc"),
        test_span(160u, 170u));
    item.data.type_alias_item.target = trait_self_type;
    item.data.type_alias_item.trait_item_definition = associated_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    cm_hir_context_destroy(&context);
}

static void test_function_pointer_lifetime_binder_model(void)
{
    CmHirContext context;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId late_zero_type;
    CmHirTypeId late_one_type;
    CmHirTypeId inner_type;
    CmHirTypeId outer_type;
    CmHirTypeId rejected;
    CmHirTypeId parameters[2];
    CmInternId names[2];
    const CmHirType *stored;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(4u, 8u);
    type.data.reference_type.region.kind = CM_HIR_REGION_LATE_BOUND;
    type.data.reference_type.region.data.binder_index = 0u;
    type.data.reference_type.pointee = unit_type;
    type.late_bound_requirement = 99u;
    assert(cm_hir_add_type(&context, &type, &late_zero_type) == CM_HIR_OK);
    stored = cm_hir_get_type(&context, late_zero_type);
    assert(stored != NULL && stored->late_bound_requirement == 1u);
    type.data.reference_type.region.data.binder_index = 1u;
    assert(cm_hir_add_type(&context, &type, &late_one_type) == CM_HIR_OK);

    names[0] = cm_hir_intern(&context, "'a");
    parameters[0] = late_zero_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_FN_POINTER_KIND;
    type.span = test_span(10u, 40u);
    type.data.fn_pointer_type.parameters = parameters;
    type.data.fn_pointer_type.parameter_count = 1u;
    type.data.fn_pointer_type.return_type = late_zero_type;
    type.data.fn_pointer_type.binder.lifetimes = names;
    type.data.fn_pointer_type.binder.lifetime_count = 1u;
    type.data.fn_pointer_type.binder.span = test_span(10u, 17u);
    type.data.fn_pointer_type.abi = cm_hir_intern(&context, "Rust");
    assert(cm_hir_add_type(&context, &type, &inner_type) == CM_HIR_OK);
    names[0] = CM_INTERN_ID_NONE;
    stored = cm_hir_get_type(&context, inner_type);
    assert(stored != NULL
        && stored->late_bound_requirement == 0u
        && stored->data.fn_pointer_type.binder.lifetimes[0]
            != CM_INTERN_ID_NONE);

    memset(&type.data.fn_pointer_type.binder, 0,
        sizeof(type.data.fn_pointer_type.binder));
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);
    names[0] = cm_hir_intern(&context, "'a");
    type.data.fn_pointer_type.binder.lifetimes = NULL;
    type.data.fn_pointer_type.binder.lifetime_count = 1u;
    type.data.fn_pointer_type.binder.span = test_span(10u, 17u);
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);
    type.data.fn_pointer_type.binder.lifetimes = names;
    type.data.fn_pointer_type.parameters = &late_one_type;
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);
    type.data.fn_pointer_type.parameters = parameters;
    names[1] = names[0];
    type.data.fn_pointer_type.binder.lifetime_count = 2u;
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);
    type.data.fn_pointer_type.binder.lifetime_count
        = CM_HIR_LIFETIME_BINDER_LIMIT + 1u;
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);

    names[1] = cm_hir_intern(&context, "'outer");
    parameters[0] = inner_type;
    parameters[1] = late_zero_type;
    type.data.fn_pointer_type.parameters = parameters;
    type.data.fn_pointer_type.parameter_count = 2u;
    type.data.fn_pointer_type.return_type = unit_type;
    type.data.fn_pointer_type.binder.lifetimes = &names[1];
    type.data.fn_pointer_type.binder.lifetime_count = 1u;
    type.data.fn_pointer_type.binder.span = test_span(20u, 30u);
    assert(cm_hir_add_type(&context, &type, &outer_type) == CM_HIR_OK);
    assert(outer_type != CM_HIR_TYPE_NONE);
    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strncmp(dump, "hir-v35\n", strlen("hir-v35\n")) == 0
        && strstr(dump,
            "type#4 for<\"'a\"> fn[\"Rust\"](ty#2)->ty#2 ") != NULL
        && strstr(dump,
            "type#5 for<\"'outer\"> fn[\"Rust\"](ty#4,ty#2)->ty#1 ")
            != NULL);
    cm_free(dump);
    fclose(dump_file);

    type.data.fn_pointer_type.parameters = parameters;
    type.data.fn_pointer_type.parameter_count = 0u;
    memset(&type.data.fn_pointer_type.binder, 0,
        sizeof(type.data.fn_pointer_type.binder));
    assert(cm_hir_add_type(&context, &type, &rejected)
        == CM_HIR_INVALID_ID);
    cm_hir_context_destroy(&context);
}

static void test_body_public_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirDefId definition;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirLocal local;
    CmHirFunctionParameter parameter;
    CmHirItem item;
    CmHirItemId item_id;
    CmInternId rust_abi;
    CmSpan span;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "body_invariants"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &crate_id, &root_id) == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    rust_abi = cm_hir_intern(&context, "Rust");

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 20u), &definition) == CM_HIR_OK);
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(10u, 20u);
    body_id = CM_HIR_BODY_NONE;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT
        && body_id == CM_HIR_BODY_NONE && context.bodies.len == 0u);
    body.origin = cm_hir_body_origin_item_source(definition);
    body.origin.kind = (CmHirBodyOriginKind)99;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT && context.bodies.len == 0u);
    body.origin = cm_hir_body_origin_item_source(definition);
    body.origin.definition = cm_hir_def_id_none();
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT && context.bodies.len == 0u);
    body.origin = cm_hir_body_origin_item_source(definition);
    body.origin.enclosing_definition = cm_hir_def_id_none();
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT && context.bodies.len == 0u);
    body.origin = cm_hir_body_origin_item_source(definition);
    body.origin.data.item_source.item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT && context.bodies.len == 0u);
    body.origin = cm_hir_body_origin_item_source(definition);
    span = test_span(10u, 20u);
    span.source = 2u;
    body.span = span;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(body_id == CM_HIR_BODY_NONE);

    body.state = CM_HIR_BODY_ERROR;
    body.error_reason = cm_hir_intern(&context, "body-error");
    body.source = 1u;
    body.source_expression_id = 0u;
    body.span = test_span(10u, 20u);
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    body.source = 0u;
    body.source_expression_id = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    body.source_expression_id = 0u;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(21u, 40u), &definition) == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = cm_hir_intern(&context, "x");
    local.type = u8_type;
    local.mutability = (CmHirMutability)99;
    local.span = test_span(22u, 23u);
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 2u;
    body.span = test_span(21u, 40u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_INVALID_ID);
    local.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    memset(&parameter, 0, sizeof(parameter));
    parameter.name = cm_hir_intern(&context, "different");
    parameter.type = u8_type;
    parameter.span = local.span;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "bad_local_name"),
        test_span(21u, 40u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(41u, 60u), &definition) == CM_HIR_OK);
    local.name = cm_hir_intern(&context, "x");
    local.type = u8_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = test_span(42u, 43u);
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 3u;
    body.span = test_span(41u, 60u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    parameter.name = local.name;
    parameter.type = unit_type;
    parameter.span = local.span;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "bad_local_type"),
        test_span(41u, 60u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    cm_hir_context_destroy(&context);
}

static void test_metadata_recipe_body_origin(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId unit_type;
    CmHirDefId definition;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirLocal local;
    CmHirExpr expression;
    CmHirExprId root_expression;
    unsigned char identity[CM_HIR_ARTIFACT_IDENTITY_SIZE];
    const CmHirBody *stored;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "metadata_body"), CM_HIR_EDITION_2021,
        test_span(0u, 100u), &crate_id, &root_id) == CM_HIR_OK);
    (void)root_id;
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 40u), &definition) == CM_HIR_OK);
    memset(identity, 0, sizeof(identity));
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_metadata_recipe(definition,
        identity, 7u, 0u);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.source = 1u;
    body.source_expression_id = 0u;
    body.span = test_span(10u, 40u);
    body_id = CM_HIR_BODY_NONE;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVALID_ARGUMENT);
    assert(body_id == CM_HIR_BODY_NONE && context.bodies.len == 0u);

    identity[0] = 0x42u;
    identity[CM_HIR_ARTIFACT_IDENTITY_SIZE - 1u] = 0xa5u;
    body.origin = cm_hir_body_origin_metadata_recipe(definition,
        identity, 7u, 0u);
    memset(&local, 0, sizeof(local));
    local.name = cm_hir_intern(&context, "value");
    local.type = unit_type;
    local.span = test_span(12u, 17u);
    local.parameter_index = 0u;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source_expression_id = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    body.source_expression_id = 0u;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    stored = cm_hir_get_body(&context, body_id);
    assert(stored != NULL
        && stored->origin.kind == CM_HIR_BODY_ORIGIN_METADATA_RECIPE
        && stored->origin.data.metadata_recipe.recipe_index == 7u
        && stored->origin.data.metadata_recipe.argument_index == 0u
        && memcmp(stored->origin.data.metadata_recipe.artifact_identity,
            identity, sizeof(identity)) == 0);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = unit_type;
    expression.span = test_span(20u, 25u);
    expression.data.local.local_index = 0u;
    root_expression = CM_HIR_EXPR_NONE;
    assert(cm_hir_add_expr(&context, &expression, &root_expression)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&context, body_id,
        root_expression) == CM_HIR_OK);
    stored = cm_hir_get_body(&context, body_id);
    assert(stored != NULL && stored->state == CM_HIR_BODY_TYPED
        && stored->root_expression == root_expression
        && stored->source_expression_id == 0u);
    cm_hir_context_destroy(&context);
}

static void test_discard_parameter_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirDefId definition;
    CmHirDefId mismatch_definition;
    CmHirDefId receiver_definition;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirFunctionParameter parameters[2];
    CmHirLocal locals[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmInternId rust_abi;
    size_t arena_bytes;
    size_t body_count;
    size_t item_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "discard_parameters"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &crate_id, &root_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(2u, 3u));
    rust_abi = cm_hir_intern(&context, "Rust");

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 80u), &definition) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].type = u8_type;
    parameters[0].span = test_span(20u, 21u);
    parameters[0].binding_kind = CM_HIR_BINDING_DISCARD;
    parameters[1].name = cm_hir_intern(&context, "value");
    parameters[1].type = u8_type;
    parameters[1].span = test_span(22u, 27u);
    parameters[1].binding_kind = CM_HIR_BINDING_NAMED;
    memset(locals, 0, sizeof(locals));
    locals[0].name = parameters[1].name;
    locals[0].type = u8_type;
    locals[0].span = parameters[1].span;
    locals[0].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = locals;
    body.local_count = 1u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 4u;
    body.span = test_span(10u, 80u);
    body_count = context.bodies.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    locals[0].name = CM_INTERN_ID_NONE;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_INVALID_ID);
    assert(body_id == CM_HIR_BODY_NONE && context.bodies.len == body_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    locals[0].name = cm_hir_intern(&context, "");
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_INVALID_ID);
    locals[0].name = cm_hir_intern(&context, "_");
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_INVALID_ID);
    locals[0].mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_INVALID_ID);
    locals[0].mutability = CM_HIR_IMMUTABLE;
    locals[0].name = parameters[1].name;
    locals[0].parameter_index = 2u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    locals[0].parameter_index = 1u;
    locals[1] = locals[0];
    locals[1].name = cm_hir_intern(&context, "earlier");
    locals[1].parameter_index = 0u;
    body.local_count = 2u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    body.local_count = 1u;
    locals[0].parameter_index = 1u;
    assert(context.bodies.len == body_count);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    assert(context.bodies.len == body_count + 1u);

    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "discard_then_value"),
        test_span(10u, 80u));
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    parameters[0].name = cm_hir_intern(&context, "first");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].name = CM_INTERN_ID_NONE;
    parameters[0].binding_kind = (CmHirBindingKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[0].name = cm_hir_intern(&context, "_");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].name = cm_hir_intern(&context, "");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].binding_kind = CM_HIR_BINDING_DISCARD;
    parameters[0].name = cm_hir_intern(&context, "discard");
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].name = CM_INTERN_ID_NONE;
    parameters[0].binding_mode = CM_HIR_PARAMETER_BINDING_REF_SHARED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[0].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameters[1].type = unit_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].type = u8_type;
    parameters[1].binding_mode = (CmHirParameterBindingMode)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].binding_mode = CM_HIR_PARAMETER_BINDING_REF_MUTABLE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(81u, 120u), &mismatch_definition) == CM_HIR_OK);
    memset(&body, 0, sizeof(body));
    body.owner = mismatch_definition;
    body.origin = cm_hir_body_origin_item_source(mismatch_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 5u;
    body.span = test_span(81u, 120u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, mismatch_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "count_mismatch"), test_span(81u, 120u));
    item.data.function_item.signature.parameters = &parameters[1];
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 160u), &receiver_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, receiver_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "receiver"),
        test_span(121u, 160u));
    item.data.function_item.signature.parameters = &parameters[0];
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "function-param item#1 index=0 binding=discard name=none")
        != NULL);
    assert(strstr(dump,
        "function-param item#1 index=1 binding=named name=\"value\"")
        != NULL);
    assert(strstr(dump,
        "body-local body#1 index=0 origin=parameter[1].binding[0] "
        "name=\"value\"")
        != NULL);
    assert(strstr(dump, "body#1 owner=1:2 origin=item-source "
        "definition=1:2 enclosing=1:2 item=1:2 state=unlowered "
        "expected=ty#1 locals=1 params=2") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_tuple_parameter_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId tuple_type;
    CmHirType type;
    CmHirTypeId tuple_elements[2];
    CmHirDefId definition;
    CmHirDefId declaration_definition;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirFunctionParameter parameter;
    CmHirLocal locals[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmInternId rust_abi;
    CmInternId left_name;
    CmInternId right_name;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "tuple_parameters"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &crate_id, &root_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(2u, 3u));
    tuple_elements[0] = u8_type;
    tuple_elements[1] = u8_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(3u, 8u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&context, &type, &tuple_type) == CM_HIR_OK);
    rust_abi = cm_hir_intern(&context, "Rust");
    left_name = cm_hir_intern(&context, "left");
    right_name = cm_hir_intern(&context, "right");
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 80u), &definition) == CM_HIR_OK);

    memset(locals, 0, sizeof(locals));
    locals[0].name = left_name;
    locals[0].type = u8_type;
    locals[0].span = test_span(21u, 25u);
    locals[0].parameter_index = 0u;
    locals[0].parameter_binding_index = 0u;
    locals[1].name = right_name;
    locals[1].type = u8_type;
    locals[1].span = test_span(27u, 32u);
    locals[1].parameter_index = 0u;
    locals[1].parameter_binding_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 4u;
    body.span = test_span(10u, 80u);

    locals[1].parameter_binding_index = 0u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    locals[0].parameter_binding_index = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    locals[0].parameter_binding_index = 0u;
    locals[1].parameter_binding_index = 2u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    locals[1].parameter_index = CM_HIR_PARAMETER_INDEX_NONE;
    locals[1].parameter_binding_index = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    locals[1].parameter_index = 0u;
    locals[1].parameter_binding_index = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    memset(&parameter, 0, sizeof(parameter));
    parameter.type = tuple_type;
    parameter.span = test_span(20u, 34u);
    parameter.binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.tuple_bindings[0].name = left_name;
    parameter.tuple_bindings[0].span = locals[0].span;
    parameter.tuple_bindings[1].name = right_name;
    parameter.tuple_bindings[1].span = locals[1].span;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "split"),
        test_span(10u, 80u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;

    parameter.type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.type = tuple_type;
    parameter.name = left_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.name = CM_INTERN_ID_NONE;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_REF_SHARED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.tuple_bindings[1].name = left_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.tuple_bindings[1].name = right_name;
    parameter.tuple_bindings[1].span = test_span(9u, 12u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.tuple_bindings[1].span = locals[1].span;

    stored_body = (CmHirBody *)cm_vec_at(&context.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    stored_body->locals[1].parameter_binding_index = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].parameter_binding_index = 1u;
    stored_body->locals[1].name = left_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].name = right_name;
    stored_body->locals[1].type = unit_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].type = u8_type;
    stored_body->locals[1].span = test_span(28u, 32u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].span = locals[1].span;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(81u, 120u), &declaration_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, declaration_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "declaration"), test_span(81u, 120u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "function-param item#1 index=0 binding=tuple-pattern name=none")
        != NULL);
    assert(strstr(dump,
        "function-param-binding item#1 parameter=0 index=0 name=\"left\"")
        != NULL);
    assert(strstr(dump,
        "function-param-binding item#1 parameter=0 index=1 name=\"right\"")
        != NULL);
    assert(strstr(dump,
        "body-local body#1 index=1 origin=parameter[0].binding[1]")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_unary_rust_call_tuple_parameter_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirType type;
    CmHirType *stored_tuple;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId u8_ref_type;
    CmHirTypeId self_type;
    CmHirTypeId unary_tuple_type;
    CmHirTypeId empty_tuple_type;
    CmHirTypeId ternary_tuple_type;
    CmHirTypeId binary_tuple_type;
    CmHirTypeId unary_elements[1];
    CmHirTypeId binary_elements[2];
    CmHirTypeId ternary_elements[3];
    CmHirDefId impl_definition;
    CmHirDefId method_definition;
    CmHirDefId free_definition;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirBodyId free_body_id;
    CmHirLocal locals[3];
    CmHirLocal free_local;
    CmHirFunctionParameter parameters[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmInternId rust_call_abi;
    CmInternId rust_abi;
    CmInternId byte_name;
    CmInternId extra_name;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "unary_rust_call_tuple_parameter"),
        CM_HIR_EDITION_2024, test_span(0u, 240u), &crate_id, &root_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(3u, 7u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = u8_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &u8_ref_type) == CM_HIR_OK);

    unary_elements[0] = u8_ref_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(7u, 12u);
    type.data.tuple_type.elements = unary_elements;
    type.data.tuple_type.element_count = 1u;
    assert(cm_hir_add_type(&context, &type, &unary_tuple_type)
        == CM_HIR_OK);
    type.data.tuple_type.elements = NULL;
    type.data.tuple_type.element_count = 0u;
    assert(cm_hir_add_type(&context, &type, &empty_tuple_type)
        == CM_HIR_OK);
    ternary_elements[0] = u8_ref_type;
    ternary_elements[1] = u8_ref_type;
    ternary_elements[2] = u8_ref_type;
    type.data.tuple_type.elements = ternary_elements;
    type.data.tuple_type.element_count = 3u;
    assert(cm_hir_add_type(&context, &type, &ternary_tuple_type)
        == CM_HIR_OK);
    binary_elements[0] = u8_ref_type;
    binary_elements[1] = u8_ref_type;
    type.data.tuple_type.elements = binary_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&context, &type, &binary_tuple_type)
        == CM_HIR_OK);

    rust_call_abi = cm_hir_intern(&context, "rust-call");
    rust_abi = cm_hir_intern(&context, "Rust");
    byte_name = cm_hir_intern(&context, "byte");
    extra_name = cm_hir_intern(&context, "extra");
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(20u, 180u), &impl_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(20u, 180u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(30u, 34u);
    type.data.self_type.owner = impl_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(40u, 120u), &method_definition)
        == CM_HIR_OK);

    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "self");
    locals[0].type = self_type;
    locals[0].span = test_span(45u, 49u);
    locals[0].parameter_index = 0u;
    locals[0].parameter_binding_index = 0u;
    locals[1].name = byte_name;
    locals[1].type = u8_ref_type;
    locals[1].span = test_span(53u, 57u);
    locals[1].parameter_index = 1u;
    locals[1].parameter_binding_index = 0u;
    locals[2].name = extra_name;
    locals[2].type = u8_ref_type;
    locals[2].span = test_span(59u, 64u);
    locals[2].parameter_index = 1u;
    locals[2].parameter_binding_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = method_definition;
    body.origin = cm_hir_body_origin_item_source(method_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = locals;
    body.local_count = 3u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(40u, 120u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = locals[0].name;
    parameters[0].type = self_type;
    parameters[0].span = locals[0].span;
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[0].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameters[1].type = unary_tuple_type;
    parameters[1].span = test_span(50u, 70u);
    parameters[1].binding_kind = CM_HIR_BINDING_TUPLE_PATTERN;
    parameters[1].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameters[1].tuple_bindings[0].name = byte_name;
    parameters[1].tuple_bindings[0].span = locals[1].span;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, method_definition, root_id,
        impl_definition, cm_hir_intern(&context, "call_once"),
        test_span(40u, 120u));
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = 2u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_call_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;

    stored_body = (CmHirBody *)cm_vec_at(&context.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->local_count = 2u;
    parameters[1].tuple_bindings[1].name = extra_name;
    parameters[1].tuple_bindings[1].span = locals[2].span;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    memset(&parameters[1].tuple_bindings[1], 0,
        sizeof(parameters[1].tuple_bindings[1]));
    parameters[1].tuple_bindings[1].span = locals[2].span;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    memset(&parameters[1].tuple_bindings[1], 0,
        sizeof(parameters[1].tuple_bindings[1]));
    parameters[1].newtype_binding.name = extra_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    memset(&parameters[1].newtype_binding, 0,
        sizeof(parameters[1].newtype_binding));
    stored_body->locals[1].parameter_index = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].parameter_index = 1u;
    stored_body->locals[1].parameter_binding_index = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].parameter_binding_index = 0u;
    stored_body->locals[1].name = extra_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].name = byte_name;
    stored_body->locals[1].type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].type = u8_ref_type;
    stored_body->locals[1].mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].mutability = CM_HIR_IMMUTABLE;
    stored_body->locals[1].span = test_span(54u, 57u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[1].span = locals[1].span;

    parameters[1].type = empty_tuple_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].type = ternary_tuple_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].type = binary_tuple_type;
    parameters[1].tuple_bindings[1].name = extra_name;
    parameters[1].tuple_bindings[1].span = locals[2].span;
    stored_body->local_count = 3u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameters[1].type = unary_tuple_type;
    memset(&parameters[1].tuple_bindings[1], 0,
        sizeof(parameters[1].tuple_bindings[1]));
    stored_body->local_count = 2u;
    item.data.function_item.signature.abi = rust_abi;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.abi = rust_call_abi;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;

    stored_tuple = (CmHirType *)cm_vec_at(&context.types,
        (size_t)unary_tuple_type - 1u);
    assert(stored_tuple != NULL);
    stored_tuple->data.tuple_type.element_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_tuple->data.tuple_type.element_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(130u, 170u), &free_definition)
        == CM_HIR_OK);
    memset(&free_local, 0, sizeof(free_local));
    free_local.name = byte_name;
    free_local.type = u8_ref_type;
    free_local.span = test_span(140u, 144u);
    free_local.parameter_index = 0u;
    free_local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = free_definition;
    body.origin = cm_hir_body_origin_item_source(free_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = &free_local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(130u, 170u);
    assert(cm_hir_add_body(&context, &body, &free_body_id) == CM_HIR_OK);
    parameters[1].tuple_bindings[0].span = free_local.span;
    parameters[1].span = test_span(138u, 150u);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, free_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "free_unary"),
        test_span(130u, 170u));
    item.data.function_item.signature.parameters = &parameters[1];
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_call_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = free_body_id;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "function-param-binding item#2 parameter=1 index=0 name=\"byte\"")
        != NULL);
    assert(strstr(dump,
        "function-param-binding item#2 parameter=1 index=1") == NULL);
    assert(strstr(dump,
        "body-local body#1 index=1 origin=parameter[1].binding[0]")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_newtype_parameter_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId parameter_field_type;
    CmHirTypeId applied_type;
    CmHirType type;
    CmHirType *stored_applied_type;
    CmHirDefId newtype_definition;
    CmHirDefId function_definition;
    CmHirDefId declaration_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirGenericArg argument;
    CmHirField field;
    CmHirItem item;
    CmHirItem *stored_newtype;
    CmHirItemId newtype_item_id;
    CmHirItemId item_id;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirLocal local;
    CmHirFunctionParameter parameter;
    CmInternId binding_name;
    CmInternId rust_abi;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "newtype_parameters"),
        CM_HIR_EDITION_2024, test_span(0u, 240u), &crate_id, &root_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(2u, 3u));
    rust_abi = cm_hir_intern(&context, "Rust");
    binding_name = cm_hir_intern(&context, "value");

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(10u, 50u), &newtype_definition)
        == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = newtype_definition;
    generic.name = cm_hir_intern(&context, "T");
    generic.span = test_span(20u, 21u);
    assert(cm_hir_add_generic_param(&context, &generic, &generic_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(25u, 26u);
    type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&context, &type, &parameter_field_type)
        == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.type = parameter_field_type;
    field.visibility.kind = CM_HIR_VIS_PUBLIC;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(25u, 35u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, newtype_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Wrapper"),
        test_span(10u, 50u));
    item.generic_parameter_start = generic_id;
    item.generic_parameter_count = 1u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &newtype_item_id) == CM_HIR_OK);

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(55u, 65u);
    type.data.named_type.definition = newtype_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &applied_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(70u, 140u), &function_definition)
        == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = binding_name;
    local.type = u8_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = test_span(85u, 90u);
    local.parameter_index = 0u;
    local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = function_definition;
    body.origin = cm_hir_body_origin_item_source(function_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(70u, 140u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    memset(&parameter, 0, sizeof(parameter));
    parameter.type = applied_type;
    parameter.span = test_span(80u, 105u);
    parameter.binding_kind = CM_HIR_BINDING_NEWTYPE_PATTERN;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.newtype_binding.name = binding_name;
    parameter.newtype_binding.span = local.span;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, function_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "unwrap"),
        test_span(70u, 140u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;

    parameter.name = binding_name;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.name = CM_INTERN_ID_NONE;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_REF_SHARED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    parameter.newtype_binding.name = CM_INTERN_ID_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.newtype_binding.name = binding_name;
    parameter.newtype_binding.span = test_span(60u, 65u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.newtype_binding.span = local.span;
    parameter.tuple_bindings[0].name = binding_name;
    parameter.tuple_bindings[0].span = local.span;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    memset(parameter.tuple_bindings, 0, sizeof(parameter.tuple_bindings));
    parameter.type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.type = applied_type;

    stored_newtype = (CmHirItem *)cm_vec_at(&context.items,
        (size_t)newtype_item_id - 1u);
    assert(stored_newtype != NULL);
    stored_newtype->data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_newtype->data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    stored_newtype->data.aggregate_item.fields[0].type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_newtype->data.aggregate_item.fields[0].type = parameter_field_type;

    stored_applied_type = (CmHirType *)cm_vec_at(&context.types,
        (size_t)applied_type - 1u);
    assert(stored_applied_type != NULL);
    stored_applied_type->data.named_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_applied_type->data.named_type.argument_count = 1u;

    stored_body = (CmHirBody *)cm_vec_at(&context.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL);
    stored_body->locals[0].type = unit_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[0].type = u8_type;
    stored_body->locals[0].parameter_binding_index = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[0].parameter_binding_index = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(150u, 200u),
        &declaration_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, declaration_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "declaration"),
        test_span(150u, 200u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "function-param item#2 index=0 binding=newtype-pattern name=none")
        != NULL);
    assert(strstr(dump,
        "function-param-binding item#2 parameter=0 index=0 name=\"value\"")
        != NULL);
    assert(strstr(dump,
        "body-local body#1 index=0 origin=parameter[0].binding[0]")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_deref_shared_parameter_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId shared_u8_type;
    CmHirTypeId mutable_u8_type;
    CmHirType type;
    CmHirDefId definition;
    CmHirBody body;
    CmHirBody *stored_body;
    CmHirBodyId body_id;
    CmHirLocal local;
    CmHirFunctionParameter parameter;
    CmHirItem item;
    CmHirItemId item_id;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "deref_shared_parameter"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id, &root_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(2u, 3u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(4u, 7u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = u8_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &shared_u8_type) == CM_HIR_OK);
    type.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &mutable_u8_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(10u, 60u), &definition)
        == CM_HIR_OK);
    memset(&local, 0, sizeof(local));
    local.name = cm_hir_intern(&context, "value");
    local.type = u8_type;
    local.mutability = CM_HIR_IMMUTABLE;
    local.span = test_span(20u, 26u);
    local.parameter_index = 0u;
    local.parameter_binding_index = 0u;
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = &local;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(10u, 60u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    memset(&parameter, 0, sizeof(parameter));
    parameter.name = local.name;
    parameter.type = shared_u8_type;
    parameter.span = local.span;
    parameter.binding_kind = CM_HIR_BINDING_NAMED;
    parameter.binding_mode = CM_HIR_PARAMETER_BINDING_DEREF_SHARED;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "first"),
        test_span(10u, 60u));
    item.data.function_item.signature.parameters = &parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = cm_hir_intern(&context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;

    parameter.type = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.type = mutable_u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    parameter.type = shared_u8_type;
    stored_body = (CmHirBody *)cm_vec_at(&context.bodies,
        (size_t)body_id - 1u);
    assert(stored_body != NULL && stored_body->local_count == 1u);
    stored_body->locals[0].type = shared_u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[0].type = u8_type;
    stored_body->locals[0].mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_body->locals[0].mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "function-param item#1 index=0 binding=named name=\"value\" "
        "mode=deref-shared") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_supertrait_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId other_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId other_root_id;
    CmHirTypeId u8_type;
    CmHirDefId required_definition;
    CmHirDefId const_definition;
    CmHirDefId forward_definition;
    CmHirDefId struct_definition;
    CmHirDefId consumer_definition;
    CmHirDefId forward_consumer_definition;
    CmHirDefId forward_generic_definition;
    CmHirDefId forward_generic_consumer_definition;
    CmHirDefId rejected_definition;
    CmHirDefId other_trait_definition;
    CmHirGenericParam required_generic;
    CmHirGenericParam forward_generic;
    CmHirGenericParamId required_parameter;
    CmHirGenericParamId forward_generic_parameter;
    CmHirGenericArg arguments[1];
    CmHirSupertrait supertraits[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId consumer_item_id;
    CmInternId forward_name;
    const CmHirItem *stored;
    const CmHirDefinition *stored_definition;
    size_t arena_bytes;
    size_t item_count;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    const char *first_supertrait;
    const char *second_supertrait;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "supertrait_test"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &crate_id, &root_id) == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 30u), &required_definition) == CM_HIR_OK);
    memset(&required_generic, 0, sizeof(required_generic));
    required_generic.kind = CM_HIR_GENERIC_TYPE;
    required_generic.owner = required_definition;
    required_generic.name = cm_hir_intern(&context, "T");
    required_generic.span = test_span(18u, 19u);
    assert(cm_hir_add_generic_param(&context, &required_generic,
        &required_parameter) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, required_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Required"),
        test_span(10u, 30u));
    item.generic_parameter_start = required_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(31u, 50u), &const_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, const_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "ConstIfConst"),
        test_span(31u, 50u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(51u, 70u), &forward_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(71u, 90u), &struct_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NotATrait"),
        test_span(71u, 90u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = u8_type;
    memset(supertraits, 0, sizeof(supertraits));
    supertraits[0].trait_type.definition = required_definition;
    supertraits[0].trait_type.arguments = arguments;
    supertraits[0].trait_type.argument_count = 1u;
    supertraits[0].span = test_span(101u, 102u);
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    supertraits[1].trait_type.definition = const_definition;
    supertraits[1].span = test_span(103u, 104u);
    supertraits[1].modifier = CM_HIR_SUPERTRAIT_CONST_IF_CONST;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(100u, 130u), &consumer_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, consumer_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Consumer"),
        test_span(100u, 130u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 2u;
    assert(cm_hir_add_item(&context, &item, &consumer_item_id)
        == CM_HIR_OK);
    assert(consumer_item_id == 4u);

    arguments[0].data.type = CM_HIR_TYPE_NONE;
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_CONST_IF_CONST;
    supertraits[1].trait_type.definition = struct_definition;
    stored = cm_hir_get_item(&context, consumer_item_id);
    assert(stored != NULL
        && stored->data.trait_item.supertraits != supertraits
        && stored->data.trait_item.supertrait_count == 2u
        && stored->data.trait_item.supertraits[0].modifier
            == CM_HIR_SUPERTRAIT_REQUIRED
        && stored->data.trait_item.supertraits[1].modifier
            == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(stored->data.trait_item.supertraits[0]
                .trait_type.definition, required_definition)
        && cm_hir_def_id_equal(stored->data.trait_item.supertraits[1]
                .trait_type.definition, const_definition)
        && stored->data.trait_item.supertraits[0].trait_type.arguments
            != arguments
        && stored->data.trait_item.supertraits[0].trait_type.arguments[0]
            .data.type == u8_type);

    supertraits[0].trait_type.definition = forward_definition;
    supertraits[0].trait_type.arguments = NULL;
    supertraits[0].trait_type.argument_count = 0u;
    supertraits[0].span = test_span(141u, 142u);
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(140u, 160u), &forward_consumer_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_consumer_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "ForwardConsumer"), test_span(140u, 160u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    forward_name = cm_hir_intern(&context, "ForwardTarget");
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, forward_definition, root_id,
        cm_hir_def_id_none(), forward_name, test_span(51u, 70u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    supertraits[0].trait_type.definition = forward_consumer_definition;
    supertraits[0].span = test_span(61u, 62u);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_definition, root_id,
        cm_hir_def_id_none(), forward_name, test_span(51u, 70u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_definition, root_id,
        cm_hir_def_id_none(), forward_name, test_span(51u, 70u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(221u, 230u), &forward_generic_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(231u, 250u), &forward_generic_consumer_definition)
        == CM_HIR_OK);
    supertraits[0].trait_type.definition = forward_generic_definition;
    supertraits[0].trait_type.arguments = NULL;
    supertraits[0].trait_type.argument_count = 0u;
    supertraits[0].span = test_span(240u, 241u);
    init_test_item(&item, CM_HIR_ITEM_TRAIT,
        forward_generic_consumer_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "ForwardGenericConsumer"),
        test_span(231u, 250u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&forward_generic, 0, sizeof(forward_generic));
    forward_generic.kind = CM_HIR_GENERIC_TYPE;
    forward_generic.owner = forward_generic_definition;
    forward_generic.name = cm_hir_intern(&context, "T");
    forward_generic.span = test_span(224u, 225u);
    assert(cm_hir_add_generic_param(&context, &forward_generic,
        &forward_generic_parameter) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_generic_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "ForwardGeneric"), test_span(221u, 230u));
    item.generic_parameter_start = forward_generic_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_generic_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "other_crate"), CM_HIR_EDITION_2024,
        test_span(0u, 50u), &other_crate_id, &other_root_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, other_crate_id,
        test_span(1u, 20u), &other_trait_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, other_trait_definition,
        other_root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "OtherTrait"), test_span(1u, 20u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(170u, 220u), &rejected_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, rejected_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Rejected"),
        test_span(170u, 220u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);

    item.data.trait_item.supertraits = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    item.data.trait_item.supertraits = supertraits;

    supertraits[0].modifier = (CmHirSupertraitModifier)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    supertraits[0].span = test_span(190u, 180u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertraits[0].span = test_span(180u, 190u);

    supertraits[0].trait_type.definition = rejected_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertraits[0].trait_type.definition = struct_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertraits[0].trait_type.definition = required_definition;
    supertraits[0].trait_type.arguments = arguments;
    supertraits[0].trait_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].data.type = u8_type;
    supertraits[0].trait_type.argument_count = 1u;
    supertraits[1] = supertraits[0];
    item.data.trait_item.supertrait_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_item.supertrait_count = 1u;
    supertraits[0].trait_type.definition.crate_id = crate_id;
    supertraits[0].trait_type.definition.index = 999u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertraits[0].trait_type.definition = other_trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    first_supertrait = strstr(first_dump,
        "supertrait item#4 index=0 modifier=required "
        "trait=1:2<ty#1> equalities=0 span=1:101..102\n");
    second_supertrait = strstr(first_dump,
        "supertrait item#4 index=1 modifier=const-if-const "
        "trait=1:3 equalities=0 span=1:103..104\n");
    assert(first_supertrait != NULL && second_supertrait != NULL
        && first_supertrait < second_supertrait);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_self_root_ingress_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId foreign_self_definition;
    CmHirDefId generic_trait_definition;
    CmHirDefId consumer_definition;
    CmHirDefId const_owner_definition;
    CmHirDefId enum_definition;
    CmHirType type;
    CmHirTypeId foreign_self_type;
    CmHirTypeId consumer_self_type;
    CmHirTypeId const_owner_self_type;
    CmHirGenericParam parameter;
    CmHirGenericParamId generic_parameter_id;
    CmHirGenericParamId const_parameter_id;
    CmHirGenericArg argument;
    CmHirSupertrait supertrait;
    CmHirVariant variant;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirDefinition *stored_definition;
    size_t arena_bytes;
    size_t item_count;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "self_root_ingress"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &crate_id,
        &root_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 20u),
        &foreign_self_definition) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(11u, 12u);
    type.data.self_type.owner = foreign_self_definition;
    assert(cm_hir_add_type(&context, &type, &foreign_self_type)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, foreign_self_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Foreign"),
        test_span(10u, 20u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(21u, 40u),
        &generic_trait_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = generic_trait_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(22u, 23u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &generic_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, generic_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Generic"),
        test_span(21u, 40u));
    item.generic_parameter_start = generic_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(41u, 70u), &consumer_definition)
        == CM_HIR_OK);
    type.span = test_span(42u, 43u);
    type.data.self_type.owner = consumer_definition;
    assert(cm_hir_add_type(&context, &type, &consumer_self_type)
        == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = foreign_self_type;
    memset(&supertrait, 0, sizeof(supertrait));
    supertrait.trait_type.definition = generic_trait_definition;
    supertrait.trait_type.arguments = &argument;
    supertrait.trait_type.argument_count = 1u;
    supertrait.span = test_span(50u, 60u);
    supertrait.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT, consumer_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Consumer"),
        test_span(41u, 70u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &supertrait;
    item.data.trait_item.supertrait_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        consumer_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);
    argument.data.type = consumer_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(71u, 100u),
        &const_owner_definition) == CM_HIR_OK);
    type.span = test_span(72u, 73u);
    type.data.self_type.owner = const_owner_definition;
    assert(cm_hir_add_type(&context, &type, &const_owner_self_type)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = const_owner_definition;
    parameter.name = cm_hir_intern(&context, "N");
    parameter.declared_type = const_owner_self_type;
    parameter.span = test_span(74u, 75u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &const_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, const_owner_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "BadConstType"), test_span(71u, 100u));
    item.generic_parameter_start = const_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    stored_definition = cm_hir_lookup_definition(&context,
        const_owner_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_ENUM, test_span(101u, 130u), &enum_definition)
        == CM_HIR_OK);
    memset(&variant, 0, sizeof(variant));
    variant.name = cm_hir_intern(&context, "Value");
    variant.form = CM_HIR_AGGREGATE_UNIT;
    variant.has_discriminant = 1;
    variant.discriminant.kind = CM_HIR_CONST_VALUE;
    variant.discriminant.type = foreign_self_type;
    variant.span = test_span(110u, 120u);
    init_test_item(&item, CM_HIR_ITEM_ENUM, enum_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "BadEnum"),
        test_span(101u, 130u));
    item.data.enum_item.variants = &variant;
    item.data.enum_item.variant_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    stored_definition = cm_hir_lookup_definition(&context, enum_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    cm_hir_context_destroy(&context);
}

static CmHirBodyId add_unlowered_value_body(CmHirContext *context,
    CmHirDefId owner, CmHirTypeId expected_type, uint32_t start)
{
    CmHirBody body;
    CmHirBodyId body_id;

    memset(&body, 0, sizeof(body));
    body.owner = owner;
    body.origin = cm_hir_body_origin_item_source(owner);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = expected_type;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(start, start + 1u);
    assert(cm_hir_add_body(context, &body, &body_id) == CM_HIR_OK);
    return body_id;
}

static void test_default_body_value_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId u8_type;
    CmHirDefId trait_definition;
    CmHirDefId required_definition;
    CmHirDefId opaque_definition;
    CmHirDefId concrete_definition;
    CmHirDefId free_definition;
    CmHirDefId impl_definition;
    CmHirDefId override_definition;
    CmHirBodyId concrete_body;
    CmHirBodyId free_body;
    CmHirBodyId override_body;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirItem *stored;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "default_value_model"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id, &root_id)
        == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 20u), &trait_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Defaults"),
        test_span(10u, 20u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_CONST, test_span(21u, 22u), &required_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_CONST, required_definition, root_id,
        trait_definition, cm_hir_intern(&context, "REQUIRED"),
        test_span(21u, 22u));
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_CONST, test_span(23u, 24u), &opaque_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_CONST, opaque_definition, root_id,
        trait_definition, cm_hir_intern(&context, "OPAQUE"),
        test_span(23u, 24u));
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    item.data.value_item.has_default_body = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.value_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL && stored->data.value_item.has_default_body == 1
        && stored->data.value_item.body == CM_HIR_BODY_NONE);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_CONST, test_span(25u, 26u), &concrete_definition)
        == CM_HIR_OK);
    concrete_body = add_unlowered_value_body(&context,
        concrete_definition, u8_type, 25u);
    init_test_item(&item, CM_HIR_ITEM_CONST, concrete_definition, root_id,
        trait_definition, cm_hir_intern(&context, "CONCRETE"),
        test_span(25u, 26u));
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    item.data.value_item.body = concrete_body;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.value_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_CONST, test_span(30u, 31u), &free_definition)
        == CM_HIR_OK);
    free_body = add_unlowered_value_body(&context, free_definition,
        u8_type, 30u);
    init_test_item(&item, CM_HIR_ITEM_CONST, free_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "FREE"),
        test_span(30u, 31u));
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    item.data.value_item.body = free_body;
    item.data.value_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.value_item.has_default_body = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(40u, 60u), &impl_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, impl_definition, root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(40u, 60u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = trait_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_CONST, test_span(45u, 46u), &override_definition)
        == CM_HIR_OK);
    override_body = add_unlowered_value_body(&context,
        override_definition, u8_type, 45u);
    init_test_item(&item, CM_HIR_ITEM_CONST, override_definition, root_id,
        impl_definition, cm_hir_intern(&context, "CONCRETE"),
        test_span(45u, 46u));
    item.data.value_item.type = u8_type;
    item.data.value_item.mutability = CM_HIR_IMMUTABLE;
    item.data.value_item.body = override_body;
    item.data.value_item.trait_item_definition = concrete_definition;
    item.data.value_item.has_default_body = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.value_item.has_default_body = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    cm_hir_context_destroy(&context);
}

static void test_static_supertrait_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId owner_definition;
    CmHirDefId rejected_definition;
    CmHirType type;
    CmHirTypeId self_type;
    CmHirOutlivesPredicate predicate;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirItem *stored;
    FILE *dump_file;
    char *dump;
    size_t arena_bytes;
    size_t item_count;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "static_supertrait"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 40u), &owner_definition)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(25u, 29u);
    type.data.self_type.owner = owner_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject_kind = CM_HIR_OUTLIVES_TYPE;
    predicate.subject.type = self_type;
    predicate.bound.kind = CM_HIR_REGION_STATIC;
    predicate.span = test_span(25u, 32u);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, owner_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Static"),
        test_span(10u, 40u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.outlives_predicates = &predicate;
    item.outlives_predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    predicate.subject.type = CM_HIR_TYPE_NONE;
    predicate.bound.kind = CM_HIR_REGION_INFER;
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL
        && stored->outlives_predicates != &predicate
        && stored->outlives_predicate_count == 1u
        && stored->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && stored->outlives_predicates[0].subject.type == self_type
        && stored->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && stored->outlives_predicates[0].span.start == 25u
        && stored->outlives_predicates[0].span.end == 32u);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(50u, 80u), &rejected_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, rejected_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Rejected"),
        test_span(50u, 80u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.outlives_predicate_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate = stored->outlives_predicates[0];
    item.outlives_predicates = &predicate;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strncmp(dump, "hir-v35\n", strlen("hir-v35\n")) == 0
        && strstr(dump,
            "outlives-predicate item#1 index=0 subject=ty#1 "
            "bound='static span=1:25..32\n") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_boundless_associated_type_prebinding(void)
{
    CmHirContext context;
    CmHirContextMark mark;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirModuleId child_module;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId transient_trait_definition;
    CmHirDefId transient_associated_definition;
    CmHirDefId struct_definition;
    CmHirDefId rejected_definition;
    CmHirDefId generic_definition;
    CmHirDefId generic_trait_definition;
    CmHirDefId mismatch_trait_definition;
    CmHirDefId mismatch_associated_definition;
    CmHirDefId wrong_owner_definition;
    CmHirDefId wrong_child_definition;
    CmHirItem item;
    CmHirItem stale_item;
    CmHirItemId associated_item_id;
    CmHirItemId item_id;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirGenericParamId ignored_parameter_id;
    CmHirGenericArg trait_argument;
    CmHirType type;
    CmHirTypeId self_type;
    CmHirTypeId parameter_type;
    CmHirTypeId projection_type;
    CmHirTypeId bound_projection_type;
    CmHirTypeId wrong_self_type;
    CmHirType *stored_projection;
    const CmHirDefinition *definition;
    CmHirTraitPredicate predicate;
    CmHirOutlivesPredicate outlives;
    CmHirAssociatedTypeBound bound;
    CmInternId residual_name;
    size_t arena_bytes;
    size_t interner_length;
    size_t item_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "prebind_model"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &crate_id, &root_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 80u), &trait_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = trait_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(11u, 12u);
    assert(cm_hir_add_generic_param(&context, &parameter, &parameter_id)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(20u, 30u),
        &associated_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(15u, 19u),
        &wrong_owner_definition) == CM_HIR_OK);
    residual_name = cm_hir_intern(&context, "Residual");
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_id, trait_definition, residual_name,
        test_span(20u, 30u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);

    memset(&predicate, 0, sizeof(predicate));
    item.predicate_count = 1u;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &associated_item_id) == CM_HIR_INVALID_ARGUMENT);
    item.predicate_count = 0u;
    item.predicates = &predicate;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &associated_item_id) == CM_HIR_INVALID_ARGUMENT);
    item.predicates = NULL;
    memset(&bound, 0, sizeof(bound));
    item.data.type_alias_item.bounds = &bound;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &associated_item_id) == CM_HIR_INVALID_ARGUMENT);
    item.data.type_alias_item.bounds = NULL;

    assert(cm_hir_context_mark(&context, &mark) == CM_HIR_OK
        && mark.prebound_associated_types == 0u);
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &associated_item_id) == CM_HIR_OK);
    assert(associated_item_id == CM_HIR_ITEM_NONE
        && context.items.len == 0u
        && context.prebound_associated_types.len == 1u);
    definition = cm_hir_lookup_definition(&context, trait_definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED);
    definition = cm_hir_lookup_definition(&context,
        associated_definition);
    assert(definition != NULL
        && definition->state == CM_HIR_DEFINITION_RESERVED);
    assert(cm_hir_context_rewind(&context, &mark) == CM_HIR_OK
        && context.prebound_associated_types.len == 0u
        && context.items.len == 0u);
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &associated_item_id) == CM_HIR_OK
        && context.prebound_associated_types.len == 1u);

    assert(cm_hir_context_mark(&context, &mark) == CM_HIR_OK
        && mark.prebound_associated_types == 1u);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(31u, 40u),
        &transient_trait_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(32u, 39u),
        &transient_associated_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        transient_associated_definition, root_id,
        transient_trait_definition, residual_name, test_span(32u, 39u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    stale_item = item;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_OK
        && context.prebound_associated_types.len == 2u);
    assert(cm_hir_context_rewind(&context, &mark) == CM_HIR_OK
        && context.prebound_associated_types.len == 1u);
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &stale_item, &item_id) == CM_HIR_INVALID_ID
        && context.prebound_associated_types.len == 1u);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_id, trait_definition, residual_name, test_span(20u, 30u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_INVARIANT_VIOLATION);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(31u, 32u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(31u, 32u);
    type.data.parameter_type.parameter = parameter_id;
    assert(cm_hir_add_type(&context, &type, &parameter_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(31u, 32u);
    type.data.self_type.owner = wrong_owner_definition;
    assert(cm_hir_add_type(&context, &type, &wrong_self_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = test_span(31u, 40u);
    type.data.projection_type.self_type = self_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    assert(cm_hir_add_type(&context, &type, &projection_type)
        == CM_HIR_INVALID_ID);
    memset(&trait_argument, 0, sizeof(trait_argument));
    trait_argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    trait_argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    type.data.projection_type.trait_type.arguments = &trait_argument;
    type.data.projection_type.trait_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &projection_type)
        == CM_HIR_INVALID_ID);
    trait_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_argument.data.type = parameter_type;
    type.data.projection_type.associated_type.arguments = &trait_argument;
    type.data.projection_type.associated_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &projection_type)
        == CM_HIR_INVALID_ID);
    type.data.projection_type.associated_type.arguments = NULL;
    type.data.projection_type.associated_type.argument_count = 0u;
    assert(cm_hir_add_type(&context, &type, &projection_type) == CM_HIR_OK);

    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_INVALID_ARGUMENT);
    item.data.type_alias_item.bound_count = 0u;
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Try"),
        test_span(10u, 80u));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(context.items.len == 1u);

    type.data.projection_type.self_type = self_type;
    assert(cm_hir_add_type(&context, &type, &bound_projection_type)
        == CM_HIR_OK);

    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_id, trait_definition, cm_hir_intern(&context, "Wrong"),
        test_span(20u, 30u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &associated_item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    item.name = residual_name;
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = bound_projection_type;
    outlives.bound.kind = CM_HIR_REGION_EARLY_BOUND;
    outlives.bound.data.parameter = parameter_id;
    outlives.span = test_span(21u, 29u);
    item.outlives_predicates = &outlives;
    item.outlives_predicate_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    interner_length = cm_interner_length(&context.strings);
    assert(cm_hir_add_item(&context, &item, &associated_item_id)
        == CM_HIR_INVALID_ID
        && associated_item_id == CM_HIR_ITEM_NONE
        && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && cm_interner_length(&context.strings) == interner_length);

    outlives.bound.kind = CM_HIR_REGION_STATIC;
    stored_projection = (CmHirType *)cm_vec_at(&context.types,
        (size_t)bound_projection_type - 1u);
    assert(stored_projection != NULL
        && stored_projection->kind == CM_HIR_TYPE_PROJECTION_KIND);
    stored_projection->data.projection_type.self_type = wrong_self_type;
    assert(cm_hir_add_item(&context, &item, &associated_item_id)
        == CM_HIR_INVALID_ID
        && associated_item_id == CM_HIR_ITEM_NONE
        && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && cm_interner_length(&context.strings) == interner_length);
    stored_projection->data.projection_type.self_type = self_type;
    assert(cm_hir_add_item(&context, &item, &associated_item_id)
        == CM_HIR_OK && context.items.len == 2u);
    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump, " projection ") != NULL
        && strstr(dump, "outlives-predicate item#2 index=0") != NULL
        && strstr(dump, "bound='static") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    definition = cm_hir_lookup_definition(&context,
        associated_definition);
    assert(definition != NULL && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->entity.item_id == associated_item_id);

    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "child"), test_span(81u, 99u),
        &child_module) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(82u, 98u),
        &mismatch_trait_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(83u, 90u),
        &mismatch_associated_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        mismatch_associated_definition, child_module,
        mismatch_trait_definition, cm_hir_intern(&context, "Mismatch"),
        test_span(83u, 90u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT,
        mismatch_trait_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "MismatchTrait"), test_span(82u, 98u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    item.owner_module = child_module;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        mismatch_associated_definition, child_module,
        mismatch_trait_definition, cm_hir_intern(&context, "Mismatch"),
        test_span(83u, 90u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(91u, 99u), &struct_definition)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(92u, 98u),
        &rejected_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, struct_definition, cm_hir_intern(&context, "Rejected"),
        test_span(92u, 98u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_INVARIANT_VIOLATION);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(92u, 99u),
        &generic_trait_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(93u, 99u),
        &generic_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(93u, 96u),
        &wrong_child_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        wrong_child_definition, root_id, generic_trait_definition,
        cm_hir_intern(&context, "WrongChild"), test_span(93u, 96u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_INVARIANT_VIOLATION);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = generic_definition;
    parameter.name = cm_hir_intern(&context, "U");
    parameter.span = test_span(94u, 95u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &ignored_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, generic_definition,
        root_id, generic_trait_definition,
        cm_hir_intern(&context, "Generic"), test_span(93u, 99u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_prebind_trait_associated_type_declaration(&context,
        &item, &item_id) == CM_HIR_INVARIANT_VIOLATION);
    cm_hir_context_destroy(&context);
}

static CmHirDefId add_test_plain_trait(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, const char *name,
    CmSpan span)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(context, crate_id, span,
        &definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, definition, module,
        cm_hir_def_id_none(), cm_hir_intern(context, name), span);
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static CmHirDefId add_test_associated_declaration(CmHirContext *context,
    CmHirCrateId crate_id, CmHirModuleId module, CmHirDefId trait_definition,
    const char *name, CmSpan span)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition(context, crate_id, span,
        &definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, definition, module,
        trait_definition, cm_hir_intern(context, name), span);
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(context, &item, &item_id) == CM_HIR_OK);
    return definition;
}

static void test_supertrait_equality_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId other_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId other_root_id;
    CmHirDefId target_definition;
    CmHirDefId output_definition;
    CmHirDefId owner_definition;
    CmHirDefId rejected_definition;
    CmHirDefId other_target_definition;
    CmHirDefId other_output_definition;
    CmHirType type;
    CmHirTypeId self_type;
    CmHirAssociatedTypeEquality equalities[2];
    CmHirSupertrait supertrait;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirItem *stored;
    FILE *dump_file;
    char *dump;
    size_t arena_bytes;
    size_t item_count;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "supertrait_equality"),
        CM_HIR_EDITION_2024, test_span(0u, 200u), &crate_id,
        &root_id) == CM_HIR_OK);
    target_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Target", test_span(10u, 30u));
    output_definition = add_test_associated_declaration(&context, crate_id,
        root_id, target_definition, "Output", test_span(20u, 25u));
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(40u, 80u), &owner_definition)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(55u, 59u);
    type.data.self_type.owner = owner_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = output_definition;
    equalities[0].value = self_type;
    equalities[0].span = test_span(50u, 63u);
    memset(&supertrait, 0, sizeof(supertrait));
    supertrait.trait_type.definition = target_definition;
    supertrait.equalities = equalities;
    supertrait.equality_count = 1u;
    supertrait.span = test_span(45u, 65u);
    supertrait.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT, owner_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Owner"),
        test_span(40u, 80u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &supertrait;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    equalities[0].associated_type = cm_hir_def_id_none();
    equalities[0].value = CM_HIR_TYPE_NONE;
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL
        && stored->data.trait_item.supertraits != &supertrait
        && stored->data.trait_item.supertraits[0].equalities != equalities
        && stored->data.trait_item.supertraits[0].equality_count == 1u
        && cm_hir_def_id_equal(stored->data.trait_item.supertraits[0]
                .equalities[0].associated_type,
            output_definition)
        && stored->data.trait_item.supertraits[0].equalities[0].value
            == self_type);

    other_target_definition = cm_hir_def_id_none();
    other_output_definition = cm_hir_def_id_none();
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "supertrait_equality_other"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &other_crate_id,
        &other_root_id) == CM_HIR_OK);
    other_target_definition = add_test_plain_trait(&context,
        other_crate_id, other_root_id, "OtherTarget",
        test_span(10u, 30u));
    other_output_definition = add_test_associated_declaration(&context,
        other_crate_id, other_root_id, other_target_definition, "Output",
        test_span(20u, 25u));
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(90u, 130u), &rejected_definition)
        == CM_HIR_OK);
    type.span = test_span(100u, 104u);
    type.data.self_type.owner = rejected_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = output_definition;
    equalities[0].value = self_type;
    equalities[0].span = test_span(100u, 113u);
    supertrait.trait_type.definition = target_definition;
    supertrait.equalities = equalities;
    supertrait.equality_count = 1u;
    supertrait.span = test_span(95u, 115u);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, rejected_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Rejected"),
        test_span(90u, 130u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &supertrait;
    item.data.trait_item.supertrait_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    supertrait.equalities = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertrait.equalities = equalities;
    supertrait.equality_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertrait.equality_count = 1u;
    equalities[0].associated_type = target_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type = output_definition;
    equalities[1] = equalities[0];
    supertrait.equality_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    supertrait.equality_count = 1u;
    supertrait.trait_type.definition = other_target_definition;
    equalities[0].associated_type = other_output_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    dump_file = tmpfile();
    assert(dump_file != NULL);
    assert(cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "supertrait item#3 index=0 modifier=required trait=1:2 "
        "equalities=1 span=1:45..65\n") != NULL);
    assert(strstr(dump,
        "supertrait-associated-type-equality item#3 supertrait=0 "
        "index=0 associated=1:3 value=ty#1 span=1:50..63\n")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_associated_type_bound_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId other_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId other_root_id;
    CmHirTypeId u8_type;
    CmHirTypeId owner_self_type;
    CmHirTypeId other_self_type;
    CmHirDefId generic_trait_definition;
    CmHirDefId iterator_definition;
    CmHirDefId sized_definition;
    CmHirDefId other_trait_definition;
    CmHirDefId item_definition;
    CmHirDefId gat_definition;
    CmHirDefId other_item_definition;
    CmHirDefId owner_definition;
    CmHirDefId into_iter_definition;
    CmHirDefId duplicate_item_definition;
    CmHirDefId struct_definition;
    CmHirDefId rejected_definition;
    CmHirDefId impl_definition;
    CmHirDefId impl_alias_definition;
    CmHirDefId foreign_trait_definition;
    CmHirDefId foreign_bound_definition;
    CmHirDefId forward_trait_definition;
    CmHirDefId forward_bound_definition;
    CmHirDefId forward_generic_definition;
    CmHirDefId forward_generic_bound_definition;
    CmHirDefId future_associated_definition;
    CmHirDefId future_bound_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId generic_parameter;
    CmHirGenericParamId gat_parameter;
    CmHirGenericParamId forward_generic_parameter;
    CmHirGenericArg arguments[1];
    CmHirAssociatedTypeEquality equalities[2];
    CmHirAssociatedTypeBound bounds[3];
    CmHirType type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId into_iter_item_id;
    const CmHirItem *stored;
    const CmHirDefinition *stored_definition;
    size_t item_count;
    size_t arena_bytes;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[256];
    const char *generic_line;
    const char *iterator_line;
    const char *equality_line;
    const char *relaxed_line;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "associated_bounds"), CM_HIR_EDITION_2024,
        test_span(0u, 1000u), &crate_id, &root_id) == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 20u), &generic_trait_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = generic_trait_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(12u, 13u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &generic_parameter) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, generic_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Generic"),
        test_span(10u, 20u));
    item.generic_parameter_start = generic_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    iterator_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Iterator", test_span(21u, 30u));
    sized_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Sized", test_span(31u, 40u));
    other_trait_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Other", test_span(41u, 50u));
    item_definition = add_test_associated_declaration(&context, crate_id,
        root_id, iterator_definition, "Item", test_span(51u, 60u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(61u, 70u), &gat_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = gat_definition;
    parameter.name = cm_hir_intern(&context, "U");
    parameter.span = test_span(63u, 64u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &gat_parameter) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, gat_definition, root_id,
        iterator_definition, cm_hir_intern(&context, "Gat"),
        test_span(61u, 70u));
    item.generic_parameter_start = gat_parameter;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    other_item_definition = add_test_associated_declaration(&context,
        crate_id, root_id, other_trait_definition, "Item",
        test_span(71u, 80u));
    owner_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Owner", test_span(81u, 100u));

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(82u, 83u);
    type.data.self_type.owner = owner_definition;
    assert(cm_hir_add_type(&context, &type, &owner_self_type) == CM_HIR_OK);
    type.span = test_span(84u, 85u);
    type.data.self_type.owner = other_trait_definition;
    assert(cm_hir_add_type(&context, &type, &other_self_type) == CM_HIR_OK);

    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = u8_type;
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = item_definition;
    equalities[0].value = owner_self_type;
    equalities[0].span = test_span(111u, 112u);
    memset(bounds, 0, sizeof(bounds));
    bounds[0].trait_type.definition = generic_trait_definition;
    bounds[0].trait_type.arguments = arguments;
    bounds[0].trait_type.argument_count = 1u;
    bounds[0].span = test_span(101u, 102u);
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    bounds[1].trait_type.definition = iterator_definition;
    bounds[1].equalities = equalities;
    bounds[1].equality_count = 1u;
    bounds[1].span = test_span(103u, 114u);
    bounds[1].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    bounds[2].trait_type.definition = sized_definition;
    bounds[2].span = test_span(115u, 116u);
    bounds[2].modifier = CM_HIR_ASSOC_BOUND_RELAXED;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 120u), &into_iter_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, into_iter_definition,
        root_id, owner_definition, cm_hir_intern(&context, "IntoIter"),
        test_span(101u, 120u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 3u;
    assert(cm_hir_add_item(&context, &item, &into_iter_item_id)
        == CM_HIR_OK);

    arguments[0].data.type = CM_HIR_TYPE_NONE;
    equalities[0].value = CM_HIR_TYPE_NONE;
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_RELAXED;
    bounds[1].trait_type.definition = sized_definition;
    stored = cm_hir_get_item(&context, into_iter_item_id);
    assert(stored != NULL
        && stored->data.type_alias_item.bounds != bounds
        && stored->data.type_alias_item.bound_count == 3u
        && stored->data.type_alias_item.bounds[0].trait_type.arguments
            != arguments
        && stored->data.type_alias_item.bounds[0].trait_type.arguments[0]
            .data.type == u8_type
        && stored->data.type_alias_item.bounds[0].modifier
            == CM_HIR_ASSOC_BOUND_REQUIRED
        && stored->data.type_alias_item.bounds[1].equalities != equalities
        && stored->data.type_alias_item.bounds[1].equalities[0].value
            == owner_self_type
        && cm_hir_def_id_equal(stored->data.type_alias_item.bounds[1]
                .trait_type.definition, iterator_definition));

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(snprintf(expected, sizeof(expected),
        "associated-type-bound item#%u index=0 modifier=required",
        (unsigned int)into_iter_item_id) > 0);
    generic_line = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "associated-type-bound item#%u index=1 modifier=required",
        (unsigned int)into_iter_item_id) > 0);
    iterator_line = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "associated-type-equality item#%u bound=1 index=0 associated=",
        (unsigned int)into_iter_item_id) > 0);
    equality_line = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "associated-type-bound item#%u index=2 modifier=relaxed",
        (unsigned int)into_iter_item_id) > 0);
    relaxed_line = strstr(first_dump, expected);
    assert(generic_line != NULL && iterator_line != NULL
        && equality_line != NULL && relaxed_line != NULL
        && generic_line < iterator_line && iterator_line < equality_line
        && equality_line < relaxed_line);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);

    arguments[0].data.type = u8_type;
    equalities[0].associated_type = item_definition;
    equalities[0].value = owner_self_type;
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    bounds[1].trait_type.definition = iterator_definition;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 140u), &rejected_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, owner_definition, cm_hir_intern(&context, "Rejected"),
        test_span(121u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);

    bounds[0].trait_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    bounds[0].trait_type.argument_count = 1u;
    arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    arguments[0].data.lifetime.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = u8_type;
    bounds[1] = bounds[0];
    item.data.type_alias_item.bound_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.type_alias_item.bound_count = 1u;

    bounds[0] = stored->data.type_alias_item.bounds[1];
    equalities[0] = stored->data.type_alias_item.bounds[1].equalities[0];
    bounds[0].equalities = equalities;
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_RELAXED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    equalities[1] = equalities[0];
    bounds[0].equality_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    bounds[0].equality_count = 1u;
    equalities[0].associated_type = other_item_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type = gat_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type = item_definition;
    equalities[0].value = other_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].value = owner_self_type;
    bounds[0].span = test_span(130u, 129u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    bounds[0].span = test_span(125u, 126u);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(141u, 150u), &struct_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NotTrait"),
        test_span(141u, 150u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, owner_definition, cm_hir_intern(&context, "Rejected"),
        test_span(121u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    bounds[0].trait_type.definition = struct_definition;
    bounds[0].trait_type.arguments = NULL;
    bounds[0].trait_type.argument_count = 0u;
    bounds[0].equalities = NULL;
    bounds[0].equality_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign"), CM_HIR_EDITION_2024,
        test_span(0u, 100u), &other_crate_id, &other_root_id) == CM_HIR_OK);
    foreign_trait_definition = add_test_plain_trait(&context,
        other_crate_id, other_root_id, "Foreign", test_span(1u, 10u));
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(151u, 160u), &foreign_bound_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        foreign_bound_definition, root_id, owner_definition,
        cm_hir_intern(&context, "ForeignBound"), test_span(151u, 160u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    bounds[0].trait_type.definition = foreign_trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, owner_definition, cm_hir_intern(&context, "Rejected"),
        test_span(121u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    bounds[0].trait_type.definition = rejected_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE
        && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    stored = cm_hir_get_item(&context, into_iter_item_id);
    assert(stored != NULL);
    bounds[0] = stored->data.type_alias_item.bounds[0];
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Free"),
        test_span(121u, 140u));
    item.data.type_alias_item.target = u8_type;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    init_test_item(&item, CM_HIR_ITEM_IMPL, cm_hir_def_id_none(), root_id,
        cm_hir_def_id_none(), CM_INTERN_ID_NONE, test_span(151u, 170u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = iterator_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    impl_definition = cm_hir_get_item(&context, item_id)->definition;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(171u, 180u), &impl_alias_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, impl_alias_definition,
        root_id, impl_definition, cm_hir_intern(&context, "Item"),
        test_span(171u, 180u));
    item.data.type_alias_item.target = u8_type;
    item.data.type_alias_item.trait_item_definition = item_definition;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    duplicate_item_definition = add_test_associated_declaration(&context,
        crate_id, root_id, iterator_definition, "Item",
        test_span(181u, 190u));
    equalities[0].associated_type = item_definition;
    equalities[0].value = owner_self_type;
    equalities[0].span = test_span(191u, 192u);
    equalities[1].associated_type = duplicate_item_definition;
    equalities[1].value = owner_self_type;
    equalities[1].span = test_span(193u, 194u);
    memset(bounds, 0, sizeof(bounds));
    bounds[0].trait_type.definition = iterator_definition;
    bounds[0].equalities = equalities;
    bounds[0].equality_count = 2u;
    bounds[0].span = test_span(191u, 195u);
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, rejected_definition,
        root_id, owner_definition, cm_hir_intern(&context, "Rejected"),
        test_span(121u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(201u, 210u), &forward_trait_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(211u, 220u), &forward_bound_definition) == CM_HIR_OK);
    memset(bounds, 0, sizeof(bounds));
    bounds[0].trait_type.definition = forward_trait_definition;
    bounds[0].span = test_span(213u, 214u);
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, forward_bound_definition,
        root_id, owner_definition, cm_hir_intern(&context, "ForwardBound"),
        test_span(211u, 220u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, forward_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Forward"),
        test_span(201u, 210u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_trait_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Forward"),
        test_span(201u, 210u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(221u, 230u), &forward_generic_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(231u, 240u), &forward_generic_bound_definition)
        == CM_HIR_OK);
    bounds[0].trait_type.definition = forward_generic_definition;
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        forward_generic_bound_definition, root_id, owner_definition,
        cm_hir_intern(&context, "ForwardGenericBound"),
        test_span(231u, 240u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = forward_generic_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(223u, 224u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &forward_generic_parameter) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_generic_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "LateGeneric"),
        test_span(221u, 230u));
    item.generic_parameter_start = forward_generic_parameter;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(241u, 250u), &future_associated_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(251u, 260u), &future_bound_definition) == CM_HIR_OK);
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = future_associated_definition;
    equalities[0].value = owner_self_type;
    equalities[0].span = test_span(253u, 254u);
    memset(bounds, 0, sizeof(bounds));
    bounds[0].trait_type.definition = iterator_definition;
    bounds[0].equalities = equalities;
    bounds[0].equality_count = 1u;
    bounds[0].span = test_span(252u, 255u);
    bounds[0].modifier = CM_HIR_ASSOC_BOUND_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, future_bound_definition,
        root_id, owner_definition, cm_hir_intern(&context, "FutureBound"),
        test_span(251u, 260u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    item.data.type_alias_item.bounds = bounds;
    item.data.type_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        future_associated_definition, root_id, other_trait_definition,
        cm_hir_intern(&context, "Future"), test_span(241u, 250u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS,
        future_associated_definition, root_id, iterator_definition,
        cm_hir_intern(&context, "Future"), test_span(241u, 250u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    cm_hir_context_destroy(&context);
}

static void init_test_trait_function(CmHirContext *context, CmHirItem *item,
    CmHirFunctionParameter *parameter, CmHirDefId definition,
    CmHirModuleId module, CmHirDefId parent, const char *name,
    CmHirTypeId self_type, CmHirTypeId return_type, CmSpan span)
{
    memset(parameter, 0, sizeof(*parameter));
    parameter->name = cm_hir_intern(context, "self");
    parameter->type = self_type;
    parameter->span = span;
    parameter->binding_kind = CM_HIR_BINDING_NAMED;
    init_test_item(item, CM_HIR_ITEM_FUNCTION, definition, module, parent,
        cm_hir_intern(context, name), span);
    item->data.function_item.signature.parameters = parameter;
    item->data.function_item.signature.parameter_count = 1u;
    item->data.function_item.signature.receiver = CM_HIR_RECEIVER_VALUE;
    item->data.function_item.signature.return_type = return_type;
    item->data.function_item.signature.abi = cm_hir_intern(context, "Rust");
    item->data.function_item.signature.safety = CM_HIR_SAFE;
}

static void test_item_trait_predicate_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId foreign_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId foreign_root_id;
    CmHirTypeId u8_type;
    CmHirTypeId parent_self_type;
    CmHirTypeId other_self_type;
    CmHirDefId generic_trait_definition;
    CmHirDefId sized_definition;
    CmHirDefId other_definition;
    CmHirDefId parent_definition;
    CmHirDefId struct_definition;
    CmHirDefId method_definition;
    CmHirDefId rejected_definition;
    CmHirDefId foreign_trait_definition;
    CmHirDefId foreign_method_definition;
    CmHirDefId forward_definition;
    CmHirDefId forward_method_definition;
    CmHirDefId forward_bad_generic_definition;
    CmHirDefId forward_bad_method_definition;
    CmHirDefId forward_good_generic_definition;
    CmHirDefId forward_good_method_definition;
    CmHirGenericParam generic_parameter;
    CmHirGenericParamId generic_parameter_id;
    CmHirGenericParamId forward_bad_parameter_id;
    CmHirGenericParamId forward_good_parameter_id;
    CmHirGenericArg arguments[1];
    CmHirTraitPredicate predicates[3];
    CmHirOutlivesPredicate outlives_predicates[2];
    CmHirFunctionParameter function_parameter;
    CmHirType type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId method_item_id;
    const CmHirItem *stored;
    const CmHirDefinition *stored_definition;
    size_t item_count;
    size_t arena_bytes;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[256];
    const char *first_predicate;
    const char *second_predicate;
    const char *duplicate_predicate;
    const char *first_outlives;
    const char *second_outlives;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "predicate_test"), CM_HIR_EDITION_2024,
        test_span(0u, 1000u), &crate_id, &root_id) == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 20u), &generic_trait_definition) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = generic_trait_definition;
    generic_parameter.name = cm_hir_intern(&context, "T");
    generic_parameter.span = test_span(12u, 13u);
    assert(cm_hir_add_generic_param(&context, &generic_parameter,
        &generic_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, generic_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Generic"),
        test_span(10u, 20u));
    item.generic_parameter_start = generic_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    sized_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Sized", test_span(21u, 30u));
    other_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Other", test_span(31u, 40u));
    parent_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Iterator", test_span(41u, 60u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(61u, 70u), &struct_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NotTrait"),
        test_span(61u, 70u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(42u, 43u);
    type.data.self_type.owner = parent_definition;
    assert(cm_hir_add_type(&context, &type, &parent_self_type) == CM_HIR_OK);
    type.span = test_span(32u, 33u);
    type.data.self_type.owner = other_definition;
    assert(cm_hir_add_type(&context, &type, &other_self_type) == CM_HIR_OK);

    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = parent_self_type;
    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = parent_self_type;
    predicates[0].trait_type.definition = sized_definition;
    predicates[0].span = test_span(81u, 82u);
    predicates[1].subject = u8_type;
    predicates[1].trait_type.definition = generic_trait_definition;
    predicates[1].trait_type.arguments = arguments;
    predicates[1].trait_type.argument_count = 1u;
    predicates[1].span = test_span(83u, 86u);
    predicates[2] = predicates[0];
    predicates[2].span = test_span(87u, 88u);
    memset(outlives_predicates, 0, sizeof(outlives_predicates));
    outlives_predicates[0].subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives_predicates[0].subject.type = parent_self_type;
    outlives_predicates[0].bound.kind = CM_HIR_REGION_STATIC;
    outlives_predicates[0].span = test_span(89u, 90u);
    outlives_predicates[1].subject_kind = CM_HIR_OUTLIVES_LIFETIME;
    outlives_predicates[1].subject.lifetime.kind = CM_HIR_REGION_STATIC;
    outlives_predicates[1].bound.kind = CM_HIR_REGION_STATIC;
    outlives_predicates[1].span = test_span(91u, 92u);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(80u, 100u), &method_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        method_definition, root_id, parent_definition, "count",
        parent_self_type, u8_type, test_span(80u, 100u));
    item.predicates = predicates;
    item.predicate_count = 3u;
    item.outlives_predicates = outlives_predicates;
    item.outlives_predicate_count = 2u;
    assert(cm_hir_add_item(&context, &item, &method_item_id) == CM_HIR_OK);

    arguments[0].data.type = CM_HIR_TYPE_NONE;
    predicates[0].subject = CM_HIR_TYPE_NONE;
    predicates[1].trait_type.definition = struct_definition;
    outlives_predicates[0].subject.type = CM_HIR_TYPE_NONE;
    outlives_predicates[1].bound.kind = (CmHirRegionKind)99;
    stored = cm_hir_get_item(&context, method_item_id);
    assert(stored != NULL && stored->predicates != predicates
        && stored->predicate_count == 3u
        && stored->predicates[0].subject == parent_self_type
        && cm_hir_def_id_equal(stored->predicates[0].trait_type.definition,
            sized_definition)
        && stored->predicates[1].trait_type.arguments != arguments
        && stored->predicates[1].trait_type.arguments[0].data.type
            == parent_self_type
        && cm_hir_def_id_equal(stored->predicates[1].trait_type.definition,
            generic_trait_definition)
        && cm_hir_def_id_equal(stored->predicates[2].trait_type.definition,
            sized_definition)
        && stored->outlives_predicates != outlives_predicates
        && stored->outlives_predicate_count == 2u
        && stored->outlives_predicates[0].subject_kind
            == CM_HIR_OUTLIVES_TYPE
        && stored->outlives_predicates[0].subject.type == parent_self_type
        && stored->outlives_predicates[0].bound.kind
            == CM_HIR_REGION_STATIC
        && stored->outlives_predicates[1].subject_kind
            == CM_HIR_OUTLIVES_LIFETIME
        && stored->outlives_predicates[1].subject.lifetime.kind
            == CM_HIR_REGION_STATIC
        && stored->outlives_predicates[1].bound.kind
            == CM_HIR_REGION_STATIC);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate item#%u index=0 subject=ty#%u trait=",
        (unsigned int)method_item_id, (unsigned int)parent_self_type) > 0);
    first_predicate = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate item#%u index=1 subject=ty#%u trait=",
        (unsigned int)method_item_id, (unsigned int)u8_type) > 0);
    second_predicate = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate item#%u index=2 subject=ty#%u trait=",
        (unsigned int)method_item_id, (unsigned int)parent_self_type) > 0);
    duplicate_predicate = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "outlives-predicate item#%u index=0 subject=ty#%u "
        "bound='static",
        (unsigned int)method_item_id, (unsigned int)parent_self_type) > 0);
    first_outlives = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "outlives-predicate item#%u index=1 subject='static "
        "bound='static", (unsigned int)method_item_id) > 0);
    second_outlives = strstr(first_dump, expected);
    assert(first_predicate != NULL && second_predicate != NULL
        && duplicate_predicate != NULL && first_predicate < second_predicate
        && second_predicate < duplicate_predicate
        && first_outlives != NULL && second_outlives != NULL
        && duplicate_predicate < first_outlives
        && first_outlives < second_outlives);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);

    arguments[0].data.type = parent_self_type;
    predicates[0] = stored->predicates[0];
    predicates[1] = stored->predicates[1];
    predicates[1].trait_type.arguments = arguments;
    predicates[2] = stored->predicates[2];
    outlives_predicates[0] = stored->outlives_predicates[0];
    outlives_predicates[1] = stored->outlives_predicates[1];
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 120u), &rejected_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        rejected_definition, root_id, parent_definition, "rejected",
        parent_self_type, u8_type, test_span(101u, 120u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    item.outlives_predicates = outlives_predicates;
    item.outlives_predicate_count = 2u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);

    item.outlives_predicates = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.outlives_predicates = outlives_predicates;
    item.outlives_predicate_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.outlives_predicate_count = 2u;
    outlives_predicates[0].subject.type = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    outlives_predicates[0].subject.type = parent_self_type;
    outlives_predicates[0].subject_kind = (CmHirOutlivesSubjectKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    outlives_predicates[0].subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives_predicates[0].span = test_span(110u, 109u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    outlives_predicates[0].span = test_span(109u, 110u);
    outlives_predicates[0].bound.kind = (CmHirRegionKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    outlives_predicates[0].bound.kind = CM_HIR_REGION_STATIC;
    outlives_predicates[1].subject.lifetime.kind = (CmHirRegionKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    outlives_predicates[1].subject.lifetime.kind = CM_HIR_REGION_STATIC;

    item.predicates = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.predicates = predicates;
    item.predicate_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.predicate_count = 1u;
    predicates[0].subject = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].subject = other_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0] = stored->predicates[1];
    predicates[0].trait_type.arguments = arguments;
    arguments[0].data.type = other_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].data.type = parent_self_type;
    predicates[0].span = test_span(110u, 109u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].span = test_span(109u, 110u);
    predicates[0].modifier = (CmHirTraitPredicateModifier)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].modifier = CM_HIR_PREDICATE_REQUIRED;
    predicates[0].trait_type.definition = struct_definition;
    predicates[0].trait_type.arguments = NULL;
    predicates[0].trait_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].trait_type.definition = generic_trait_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].trait_type.arguments = arguments;
    predicates[0].trait_type.argument_count = 1u;
    arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    arguments[0].data.lifetime.kind = CM_HIR_REGION_ERASED;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = parent_self_type;
    predicates[0].trait_type.definition.crate_id = crate_id;
    predicates[0].trait_type.definition.index = 999u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign_predicate"), CM_HIR_EDITION_2024,
        test_span(0u, 50u), &foreign_crate_id, &foreign_root_id) == CM_HIR_OK);
    foreign_trait_definition = add_test_plain_trait(&context,
        foreign_crate_id, foreign_root_id, "Foreign", test_span(1u, 10u));
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 130u), &foreign_method_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        foreign_method_definition, root_id, parent_definition,
        "foreign_bound", parent_self_type, u8_type,
        test_span(121u, 130u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    predicates[0].trait_type.definition = foreign_trait_definition;
    predicates[0].trait_type.arguments = NULL;
    predicates[0].trait_type.argument_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_trait_function(&context, &item, &function_parameter,
        rejected_definition, root_id, parent_definition, "rejected",
        parent_self_type, u8_type, test_span(101u, 120u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    predicates[0].trait_type.definition = rejected_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE
        && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 130u), &forward_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(131u, 140u), &forward_method_definition) == CM_HIR_OK);
    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = parent_self_type;
    predicates[0].trait_type.definition = forward_definition;
    predicates[0].span = test_span(133u, 134u);
    init_test_trait_function(&context, &item, &function_parameter,
        forward_method_definition, root_id, parent_definition,
        "forward_method", parent_self_type, u8_type,
        test_span(131u, 140u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, forward_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Forward"),
        test_span(121u, 130u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, forward_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Forward"),
        test_span(121u, 130u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(141u, 150u), &forward_bad_generic_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(151u, 160u), &forward_bad_method_definition) == CM_HIR_OK);
    predicates[0].trait_type.definition = forward_bad_generic_definition;
    init_test_trait_function(&context, &item, &function_parameter,
        forward_bad_method_definition, root_id, parent_definition,
        "forward_bad_generic", parent_self_type, u8_type,
        test_span(151u, 160u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = forward_bad_generic_definition;
    generic_parameter.name = cm_hir_intern(&context, "T");
    generic_parameter.span = test_span(143u, 144u);
    assert(cm_hir_add_generic_param(&context, &generic_parameter,
        &forward_bad_parameter_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TRAIT,
        forward_bad_generic_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "ForwardBadGeneric"),
        test_span(141u, 150u));
    item.generic_parameter_start = forward_bad_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(161u, 170u), &forward_good_generic_definition) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = forward_good_generic_definition;
    generic_parameter.name = cm_hir_intern(&context, "T");
    generic_parameter.span = test_span(163u, 164u);
    assert(cm_hir_add_generic_param(&context, &generic_parameter,
        &forward_good_parameter_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(171u, 180u), &forward_good_method_definition) == CM_HIR_OK);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = parent_self_type;
    predicates[0].trait_type.definition = forward_good_generic_definition;
    predicates[0].trait_type.arguments = arguments;
    predicates[0].trait_type.argument_count = 1u;
    init_test_trait_function(&context, &item, &function_parameter,
        forward_good_method_definition, root_id, parent_definition,
        "forward_good_generic", parent_self_type, u8_type,
        test_span(171u, 180u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT,
        forward_good_generic_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "ForwardGoodGeneric"),
        test_span(161u, 170u));
    item.generic_parameter_start = forward_good_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    cm_hir_context_destroy(&context);
}

static void test_trait_predicate_equality_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId foreign_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId foreign_root_id;
    CmHirDefId generic_trait_definition;
    CmHirDefId owner_definition;
    CmHirDefId unrelated_definition;
    CmHirDefId method_definition;
    CmHirDefId rejected_definition;
    CmHirDefId item_definition;
    CmHirDefId other_item_definition;
    CmHirDefId gat_definition;
    CmHirDefId foreign_item_definition;
    CmHirDefId future_item_definition;
    CmHirDefId future_method_definition;
    CmHirDefId duplicate_a_definition;
    CmHirDefId duplicate_b_definition;
    CmHirDefId duplicate_method_definition;
    CmHirDefId base_definition;
    CmHirDefId output_definition;
    CmHirDefId derived_definition;
    CmHirDefId inherited_method_definition;
    CmHirDefId left_definition;
    CmHirDefId right_definition;
    CmHirDefId left_item_definition;
    CmHirDefId right_item_definition;
    CmHirDefId ambiguous_definition;
    CmHirDefId ambiguous_method_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId generic_parameter_id;
    CmHirGenericParamId owner_parameter_start;
    CmHirGenericParamId owner_lifetime_id;
    CmHirGenericParamId owner_type_id;
    CmHirGenericParamId owner_const_id;
    CmHirGenericParamId unrelated_parameter_start;
    CmHirGenericParamId unrelated_lifetime_id;
    CmHirGenericParamId unrelated_type_id;
    CmHirGenericParamId unrelated_const_id;
    CmHirGenericParamId method_parameter_id;
    CmHirGenericParamId gat_parameter_id;
    CmHirTypeId u8_type;
    CmHirTypeId owner_type;
    CmHirTypeId unrelated_type;
    CmHirTypeId method_type;
    CmHirTypeId owner_array_type;
    CmHirTypeId owner_reference_type;
    CmHirTypeId unrelated_const_array_type;
    CmHirTypeId unrelated_lifetime_reference_type;
    CmHirTypeId owner_self_type;
    CmHirTypeId wrong_self_type;
    CmHirGenericArg arguments[1];
    CmHirAssociatedTypeEquality equalities[2];
    CmHirTraitPredicate predicates[2];
    CmHirSupertrait supertraits[2];
    CmHirFunctionParameter function_parameter;
    CmHirType type;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId method_item_id;
    const CmHirItem *stored;
    const CmHirModule *root;
    const CmHirDefinition *stored_definition;
    size_t item_count;
    size_t arena_bytes;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[256];
    const char *first_predicate;
    const char *first_equality;
    const char *second_predicate;
    const char *second_equality;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "predicate_equalities"),
        CM_HIR_EDITION_2024, test_span(0u, 1000u), &crate_id,
        &root_id) == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 20u), &generic_trait_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = generic_trait_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(11u, 12u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &generic_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, generic_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Generic"),
        test_span(10u, 20u));
    item.generic_parameter_start = generic_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_definition = add_test_associated_declaration(&context, crate_id,
        root_id, generic_trait_definition, "Item", test_span(21u, 22u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(30u, 50u), &owner_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = owner_definition;
    parameter.name = cm_hir_intern(&context, "a");
    parameter.span = test_span(31u, 32u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &owner_lifetime_id) == CM_HIR_OK);
    owner_parameter_start = owner_lifetime_id;
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "P");
    parameter.span = test_span(33u, 34u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &owner_type_id) == CM_HIR_OK);
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.index = 2u;
    parameter.name = cm_hir_intern(&context, "N");
    parameter.span = test_span(35u, 36u);
    parameter.declared_type = u8_type;
    assert(cm_hir_add_generic_param(&context, &parameter,
        &owner_const_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, owner_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Owner"),
        test_span(30u, 50u));
    item.generic_parameter_start = owner_parameter_start;
    item.generic_parameter_count = 3u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(51u, 70u), &unrelated_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = unrelated_definition;
    parameter.name = cm_hir_intern(&context, "b");
    parameter.span = test_span(52u, 53u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &unrelated_lifetime_id) == CM_HIR_OK);
    unrelated_parameter_start = unrelated_lifetime_id;
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "Q");
    parameter.span = test_span(54u, 55u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &unrelated_type_id) == CM_HIR_OK);
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.index = 2u;
    parameter.name = cm_hir_intern(&context, "M");
    parameter.span = test_span(56u, 57u);
    parameter.declared_type = u8_type;
    assert(cm_hir_add_generic_param(&context, &parameter,
        &unrelated_const_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, unrelated_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Unrelated"),
        test_span(51u, 70u));
    item.generic_parameter_start = unrelated_parameter_start;
    item.generic_parameter_count = 3u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    other_item_definition = add_test_associated_declaration(&context,
        crate_id, root_id, unrelated_definition, "Item",
        test_span(71u, 72u));

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(73u, 74u);
    type.data.parameter_type.parameter = owner_type_id;
    assert(cm_hir_add_type(&context, &type, &owner_type) == CM_HIR_OK);
    type.data.parameter_type.parameter = unrelated_type_id;
    assert(cm_hir_add_type(&context, &type, &unrelated_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(75u, 76u);
    type.data.array_type.element = owner_type;
    type.data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
    type.data.array_type.length.type = u8_type;
    type.data.array_type.length.data.parameter = owner_const_id;
    assert(cm_hir_add_type(&context, &type, &owner_array_type) == CM_HIR_OK);
    type.data.array_type.length.data.parameter = unrelated_const_id;
    assert(cm_hir_add_type(&context, &type,
        &unrelated_const_array_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(77u, 78u);
    type.data.reference_type.region.kind = CM_HIR_REGION_EARLY_BOUND;
    type.data.reference_type.region.data.parameter = owner_lifetime_id;
    type.data.reference_type.pointee = owner_array_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type,
        &owner_reference_type) == CM_HIR_OK);
    type.data.reference_type.region.data.parameter = unrelated_lifetime_id;
    type.data.reference_type.pointee = owner_type;
    assert(cm_hir_add_type(&context, &type,
        &unrelated_lifetime_reference_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(79u, 80u);
    type.data.self_type.owner = owner_definition;
    assert(cm_hir_add_type(&context, &type, &owner_self_type) == CM_HIR_OK);
    type.data.self_type.owner = unrelated_definition;
    assert(cm_hir_add_type(&context, &type, &wrong_self_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(81u, 100u), &method_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = method_definition;
    parameter.name = cm_hir_intern(&context, "U");
    parameter.span = test_span(82u, 83u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &method_parameter_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(84u, 85u);
    type.data.parameter_type.parameter = method_parameter_id;
    assert(cm_hir_add_type(&context, &type, &method_type) == CM_HIR_OK);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    arguments[0].data.type = owner_reference_type;
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = item_definition;
    equalities[0].value = method_type;
    equalities[0].span = test_span(90u, 91u);
    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = owner_type;
    predicates[0].trait_type.definition = generic_trait_definition;
    predicates[0].trait_type.arguments = arguments;
    predicates[0].trait_type.argument_count = 1u;
    predicates[0].equalities = equalities;
    predicates[0].equality_count = 1u;
    predicates[0].span = test_span(88u, 92u);
    predicates[1] = predicates[0];
    predicates[1].span = test_span(93u, 97u);
    init_test_trait_function(&context, &item, &function_parameter,
        method_definition, root_id, owner_definition, "map", owner_self_type,
        u8_type, test_span(81u, 100u));
    item.generic_parameter_start = method_parameter_id;
    item.generic_parameter_count = 1u;
    item.predicates = predicates;
    item.predicate_count = 2u;
    assert(cm_hir_add_item(&context, &item, &method_item_id) == CM_HIR_OK);
    equalities[0].value = u8_type;
    arguments[0].data.type = u8_type;
    stored = cm_hir_get_item(&context, method_item_id);
    assert(stored != NULL && stored->predicates != predicates
        && stored->predicate_count == 2u
        && stored->predicates[0].equalities != equalities
        && stored->predicates[1].equalities != equalities
        && stored->predicates[0].equalities
            != stored->predicates[1].equalities
        && stored->predicates[0].equalities[0].value == method_type
        && stored->predicates[0].trait_type.arguments != arguments
        && stored->predicates[0].trait_type.arguments[0].data.type
            == owner_reference_type);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate item#%u index=0 subject=ty#%u trait=",
        (unsigned int)method_item_id, (unsigned int)owner_type) > 0);
    first_predicate = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate-equality item#%u predicate=0 index=0 associated=",
        (unsigned int)method_item_id) > 0);
    first_equality = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate item#%u index=1 subject=ty#%u trait=",
        (unsigned int)method_item_id, (unsigned int)owner_type) > 0);
    second_predicate = strstr(first_dump, expected);
    assert(snprintf(expected, sizeof(expected),
        "trait-predicate-equality item#%u predicate=1 index=0 associated=",
        (unsigned int)method_item_id) > 0);
    second_equality = strstr(first_dump, expected);
    assert(first_predicate != NULL && first_equality != NULL
        && second_predicate != NULL && second_equality != NULL
        && strstr(first_predicate, " equalities=1 span=") != NULL
        && first_predicate < first_equality
        && first_equality < second_predicate
        && second_predicate < second_equality);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 120u), &rejected_definition) == CM_HIR_OK);
    arguments[0].data.type = owner_reference_type;
    equalities[0] = stored->predicates[0].equalities[0];
    predicates[0] = stored->predicates[0];
    predicates[0].trait_type.arguments = arguments;
    predicates[0].equalities = equalities;
    init_test_trait_function(&context, &item, &function_parameter,
        rejected_definition, root_id, owner_definition, "rejected",
        owner_self_type, u8_type, test_span(101u, 120u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    predicates[0].equalities = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].equalities = equalities;
    predicates[0].equality_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].equality_count = 1u;
    equalities[0].value = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].value = method_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].value = wrong_self_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].value = u8_type;
    equalities[0].span = test_span(110u, 109u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].span = test_span(109u, 110u);
    predicates[0].subject = unrelated_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].subject = owner_type;
    arguments[0].data.type = unrelated_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].data.type = unrelated_lifetime_reference_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].data.type = unrelated_const_array_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    arguments[0].data.type = owner_reference_type;
    equalities[0].value = unrelated_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].value = u8_type;
    equalities[0].associated_type = other_item_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type = rejected_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    root = cm_hir_get_module(&context, root_id);
    assert(root != NULL);
    equalities[0].associated_type = root->definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type.crate_id = crate_id;
    equalities[0].associated_type.index = 999u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(121u, 130u), &gat_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = gat_definition;
    parameter.name = cm_hir_intern(&context, "G");
    parameter.span = test_span(122u, 123u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &gat_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, gat_definition, root_id,
        generic_trait_definition, cm_hir_intern(&context, "Gat"),
        test_span(121u, 130u));
    item.generic_parameter_start = gat_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    equalities[0].associated_type = gat_definition;
    init_test_trait_function(&context, &item, &function_parameter,
        rejected_definition, root_id, owner_definition, "rejected",
        owner_self_type, u8_type, test_span(101u, 120u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "foreign_equalities"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &foreign_crate_id,
        &foreign_root_id) == CM_HIR_OK);
    foreign_item_definition = add_test_plain_trait(&context,
        foreign_crate_id, foreign_root_id, "Foreign", test_span(1u, 10u));
    equalities[0].associated_type = foreign_item_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    equalities[0].associated_type = item_definition;
    equalities[1] = equalities[0];
    predicates[0].equality_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicates[0].equality_count = 1u;
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len > item_count
        && cm_arena_bytes_used(&context.storage) >= arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(131u, 140u), &future_item_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(141u, 150u), &future_method_definition) == CM_HIR_OK);
    equalities[0].associated_type = future_item_definition;
    equalities[0].value = u8_type;
    predicates[0].equality_count = 1u;
    init_test_trait_function(&context, &item, &function_parameter,
        future_method_definition, root_id, owner_definition, "future",
        owner_self_type, u8_type, test_span(141u, 150u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, future_item_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Future"),
        test_span(131u, 140u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, future_item_definition,
        root_id, generic_trait_definition, cm_hir_intern(&context, "Future"),
        test_span(131u, 140u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(151u, 160u), &duplicate_a_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(161u, 170u), &duplicate_b_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(171u, 180u), &duplicate_method_definition) == CM_HIR_OK);
    equalities[0].associated_type = duplicate_a_definition;
    equalities[1] = equalities[0];
    equalities[1].associated_type = duplicate_b_definition;
    equalities[1].span = test_span(175u, 176u);
    predicates[0].equality_count = 2u;
    init_test_trait_function(&context, &item, &function_parameter,
        duplicate_method_definition, root_id, owner_definition,
        "duplicates", owner_self_type, u8_type, test_span(171u, 180u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, duplicate_a_definition,
        root_id, generic_trait_definition, cm_hir_intern(&context, "Same"),
        test_span(151u, 160u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, duplicate_b_definition,
        root_id, generic_trait_definition, cm_hir_intern(&context, "Same"),
        test_span(161u, 170u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    base_definition = add_test_plain_trait(&context, crate_id, root_id,
        "FnOnce", test_span(181u, 190u));
    output_definition = add_test_associated_declaration(&context, crate_id,
        root_id, base_definition, "Output", test_span(191u, 200u));
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(201u, 210u), &derived_definition) == CM_HIR_OK);
    memset(supertraits, 0, sizeof(supertraits));
    supertraits[0].trait_type.definition = base_definition;
    supertraits[0].span = test_span(202u, 203u);
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT, derived_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "FnMut"),
        test_span(201u, 210u));
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(211u, 220u), &inherited_method_definition) == CM_HIR_OK);
    memset(equalities, 0, sizeof(equalities));
    equalities[0].associated_type = output_definition;
    equalities[0].value = u8_type;
    equalities[0].span = test_span(214u, 215u);
    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = owner_type;
    predicates[0].trait_type.definition = derived_definition;
    predicates[0].equalities = equalities;
    predicates[0].equality_count = 1u;
    predicates[0].span = test_span(213u, 216u);
    init_test_trait_function(&context, &item, &function_parameter,
        inherited_method_definition, root_id, owner_definition,
        "call_mut", owner_self_type, u8_type, test_span(211u, 220u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    left_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Left", test_span(221u, 230u));
    right_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Right", test_span(231u, 240u));
    left_item_definition = add_test_associated_declaration(&context,
        crate_id, root_id, left_definition, "Value",
        test_span(241u, 242u));
    right_item_definition = add_test_associated_declaration(&context,
        crate_id, root_id, right_definition, "Value",
        test_span(243u, 244u));
    assert(!cm_hir_def_id_equal(left_item_definition,
        right_item_definition));
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(245u, 255u), &ambiguous_definition) == CM_HIR_OK);
    memset(supertraits, 0, sizeof(supertraits));
    supertraits[0].trait_type.definition = left_definition;
    supertraits[0].span = test_span(246u, 247u);
    supertraits[0].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    supertraits[1].trait_type.definition = right_definition;
    supertraits[1].span = test_span(248u, 249u);
    supertraits[1].modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT, ambiguous_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Ambiguous"),
        test_span(245u, 255u));
    item.data.trait_item.supertraits = supertraits;
    item.data.trait_item.supertrait_count = 2u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(256u, 265u), &ambiguous_method_definition) == CM_HIR_OK);
    equalities[0].associated_type = left_item_definition;
    predicates[0].trait_type.definition = ambiguous_definition;
    init_test_trait_function(&context, &item, &function_parameter,
        ambiguous_method_definition, root_id, owner_definition,
        "ambiguous", owner_self_type, u8_type, test_span(256u, 265u));
    item.predicates = predicates;
    item.predicate_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    cm_hir_context_destroy(&context);
}

static CmHirExprId add_test_integer_expression(CmHirContext *context,
    CmHirBodyId body, CmHirTypeId type, CmSpan span)
{
    CmHirExpr expression;
    CmHirExprId expression_id;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.owner_body = body;
    expression.type = type;
    expression.span = span;
    expression.data.integer.low_bits = 1u;
    assert(cm_hir_add_expr(context, &expression, &expression_id)
        == CM_HIR_OK);
    return expression_id;
}

static void assert_aggregate_expression_rejected(CmHirContext *context,
    const CmHirExpr *expression)
{
    CmHirExprId expression_id;
    size_t expression_count;
    size_t arena_bytes;

    expression_count = context->expressions.len;
    arena_bytes = cm_arena_bytes_used(&context->storage);
    assert(cm_hir_add_expr(context, expression, &expression_id)
        != CM_HIR_OK);
    assert(expression_id == CM_HIR_EXPR_NONE);
    assert(context->expressions.len == expression_count);
    assert(cm_arena_bytes_used(&context->storage) == arena_bytes);
}

static void assert_field_expression_rejected(CmHirContext *context,
    const CmHirExpr *expression)
{
    CmHirExprId expression_id;
    size_t expression_count;
    size_t arena_bytes;

    expression_count = context->expressions.len;
    arena_bytes = cm_arena_bytes_used(&context->storage);
    assert(cm_hir_add_expr(context, expression, &expression_id)
        != CM_HIR_OK);
    assert(expression_id == CM_HIR_EXPR_NONE);
    assert(context->expressions.len == expression_count);
    assert(cm_arena_bytes_used(&context->storage) == arena_bytes);
}

static void assert_reference_expression_rejected(CmHirContext *context,
    const CmHirExpr *expression)
{
    CmHirExprId expression_id;
    size_t expression_count;
    size_t arena_bytes;

    expression_count = context->expressions.len;
    arena_bytes = cm_arena_bytes_used(&context->storage);
    assert(cm_hir_add_expr(context, expression, &expression_id)
        != CM_HIR_OK);
    assert(expression_id == CM_HIR_EXPR_NONE
        && context->expressions.len == expression_count
        && cm_arena_bytes_used(&context->storage) == arena_bytes);
}

static void test_reference_expression_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId body_definition;
    CmHirDefId other_body_definition;
    CmHirType type;
    CmHirTypeId u32_type;
    CmHirTypeId shared_u32_type;
    CmHirTypeId mutable_u32_type;
    CmHirTypeId static_u32_type;
    CmHirTypeId raw_u32_type;
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirBodyId other_body_id;
    CmHirExpr expression;
    CmHirExprId value_local;
    CmHirExprId reference_local;
    CmHirExprId other_local;
    CmHirExprId integer;
    CmHirExprId borrow;
    CmHirExprId dereference;
    CmHirExprId nested_dereference;
    const CmHirExpr *stored;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[256];

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "reference_expression_model"),
        CM_HIR_EDITION_2021, test_span(0u, 200u), &crate_id,
        &root_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&context, &type, &u32_type) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(3u, 4u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = u32_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &shared_u32_type)
        == CM_HIR_OK);
    type.span = test_span(5u, 6u);
    type.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &mutable_u32_type)
        == CM_HIR_OK);
    type.span = test_span(7u, 8u);
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_add_type(&context, &type, &static_u32_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = test_span(9u, 10u);
    type.data.raw_pointer_type.pointee = u32_type;
    type.data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &raw_u32_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(20u, 100u), &body_definition) == CM_HIR_OK);
    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "value");
    locals[0].type = u32_type;
    locals[0].span = test_span(25u, 30u);
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(&context, "reference");
    locals[1].type = shared_u32_type;
    locals[1].span = test_span(31u, 37u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = body_definition;
    body.origin = cm_hir_body_origin_item_source(body_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(20u, 100u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 180u), &other_body_definition) == CM_HIR_OK);
    body.owner = other_body_definition;
    body.origin = cm_hir_body_origin_item_source(other_body_definition);
    body.locals = locals;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source_expression_id = 2u;
    body.span = test_span(101u, 180u);
    assert(cm_hir_add_body(&context, &body, &other_body_id) == CM_HIR_OK);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = u32_type;
    expression.span = test_span(40u, 41u);
    assert(cm_hir_add_expr(&context, &expression, &value_local)
        == CM_HIR_OK);
    expression.type = shared_u32_type;
    expression.span = test_span(50u, 51u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&context, &expression, &reference_local)
        == CM_HIR_OK);
    expression.owner_body = other_body_id;
    expression.type = u32_type;
    expression.span = test_span(120u, 121u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&context, &expression, &other_local)
        == CM_HIR_OK);
    integer = add_test_integer_expression(&context, body_id, u32_type,
        test_span(60u, 61u));

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BORROW_SHARED;
    expression.owner_body = body_id;
    expression.type = shared_u32_type;
    expression.span = test_span(39u, 41u);
    expression.data.borrow_shared.operand = value_local;
    assert(cm_hir_add_expr(&context, &expression, &borrow) == CM_HIR_OK);
    stored = cm_hir_get_expr(&context, borrow);
    expression.data.borrow_shared.operand = CM_HIR_EXPR_NONE;
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_BORROW_SHARED
        && stored->data.borrow_shared.operand == value_local);

    expression.data.borrow_shared.operand = integer;
    expression.span = test_span(59u, 61u);
    assert_reference_expression_rejected(&context, &expression);
    expression.data.borrow_shared.operand = other_local;
    expression.span = test_span(119u, 121u);
    assert_reference_expression_rejected(&context, &expression);
    expression.data.borrow_shared.operand = value_local;
    expression.span = test_span(39u, 41u);
    expression.type = mutable_u32_type;
    assert_reference_expression_rejected(&context, &expression);
    expression.type = static_u32_type;
    assert_reference_expression_rejected(&context, &expression);
    expression.type = raw_u32_type;
    assert_reference_expression_rejected(&context, &expression);
    expression.type = shared_u32_type;
    expression.span = test_span(40u, 41u);
    assert_reference_expression_rejected(&context, &expression);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_DEREFERENCE;
    expression.owner_body = body_id;
    expression.type = u32_type;
    expression.span = test_span(49u, 51u);
    expression.data.dereference.operand = reference_local;
    assert(cm_hir_add_expr(&context, &expression, &dereference)
        == CM_HIR_OK);
    expression.span = test_span(38u, 41u);
    expression.data.dereference.operand = borrow;
    assert(cm_hir_add_expr(&context, &expression, &nested_dereference)
        == CM_HIR_OK);
    stored = cm_hir_get_expr(&context, nested_dereference);
    expression.data.dereference.operand = CM_HIR_EXPR_NONE;
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_DEREFERENCE
        && stored->data.dereference.operand == borrow);

    expression.data.dereference.operand = value_local;
    expression.span = test_span(39u, 41u);
    assert_reference_expression_rejected(&context, &expression);
    expression.data.dereference.operand = other_local;
    expression.span = test_span(119u, 121u);
    assert_reference_expression_rejected(&context, &expression);
    expression.data.dereference.operand = reference_local;
    expression.span = test_span(50u, 51u);
    assert_reference_expression_rejected(&context, &expression);
    expression.type = shared_u32_type;
    expression.span = test_span(49u, 51u);
    assert_reference_expression_rejected(&context, &expression);
    expression.type = u32_type;
    expression.kind = (CmHirExprKind)99;
    assert_reference_expression_rejected(&context, &expression);

    assert(cm_hir_set_body_root_expression(&context, body_id,
        nested_dereference) == CM_HIR_OK);
    assert(cm_hir_get_body(&context, body_id)->state == CM_HIR_BODY_TYPED);
    {
        CmSemanticMarkResult mark_result;
        uint64_t generation;

        generation = context.semantic_generation;
        mark_result = cm_hir_semantic_mark_bodies(&context, &body_id, 1u);
        assert(mark_result.status
                == CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
            && mark_result.body_index == 0u && mark_result.body == body_id
            && mark_result.expression == nested_dereference
            && context.semantic_generation == generation
            && cm_hir_get_expr(&context, nested_dereference)->usage
                == CM_HIR_USAGE_UNKNOWN
            && cm_hir_get_expr(&context, borrow)->usage
                == CM_HIR_USAGE_UNKNOWN);
    }
    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL
        && cm_hir_dump(first_file, &context) == 0
        && cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(snprintf(expected, sizeof(expected),
        "expr#%u borrow-shared type=ty#%u operand=expr#%u "
        "owner=body#%u span=1:39..41",
        (unsigned int)borrow, (unsigned int)shared_u32_type,
        (unsigned int)value_local, (unsigned int)body_id) > 0
        && strstr(first_dump, expected) != NULL);
    assert(snprintf(expected, sizeof(expected),
        "expr#%u dereference type=ty#%u operand=expr#%u "
        "owner=body#%u span=1:38..41",
        (unsigned int)nested_dereference, (unsigned int)u32_type,
        (unsigned int)borrow, (unsigned int)body_id) > 0
        && strstr(first_dump, expected) != NULL);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0 && fclose(first_file) == 0);
    cm_hir_context_destroy(&context);
}

static void assert_method_call_expression_rejected(CmHirContext *context,
    const CmHirExpr *expression)
{
    CmHirExprId expression_id;
    size_t expression_count;
    size_t arena_bytes;

    expression_count = context->expressions.len;
    arena_bytes = cm_arena_bytes_used(&context->storage);
    assert(cm_hir_add_expr(context, expression, &expression_id)
        != CM_HIR_OK);
    assert(expression_id == CM_HIR_EXPR_NONE
        && context->expressions.len == expression_count
        && cm_arena_bytes_used(&context->storage) == arena_bytes);
}

static void test_method_call_expression_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId trait_definitions[2];
    CmHirDefId non_trait_definition;
    CmHirDefId body_definitions[2];
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u32_type;
    CmHirLocal local;
    CmHirBody body;
    CmHirBodyId body_ids[2];
    CmHirExpr expression;
    CmHirExprId receiver;
    CmHirExprId argument;
    CmHirExprId reordered_argument;
    CmHirExprId other_receiver;
    CmHirExprId method_call_id;
    CmHirExprId arguments[2];
    CmHirDefId traits[2];
    const CmHirExpr *stored;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[256];
    uint32_t index;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "method_call_model"),
        CM_HIR_EDITION_2021, test_span(0u, 200u), &crate_id,
        &root_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 4u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&context, &type, &u32_type) == CM_HIR_OK);

    for (index = 0u; index < 2u; ++index) {
        assert(cm_hir_reserve_item_definition(&context, crate_id,
            test_span(5u + index * 10u, 14u + index * 10u),
            &trait_definitions[index]) == CM_HIR_OK);
        init_test_item(&item, CM_HIR_ITEM_TRAIT,
            trait_definitions[index], root_id, cm_hir_def_id_none(),
            cm_hir_intern(&context, index == 0u ? "Value" : "Imported"),
            test_span(5u + index * 10u, 14u + index * 10u));
        item.data.trait_item.safety = CM_HIR_SAFE;
        assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    }
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(25u, 34u), &non_trait_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, non_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "NotTrait"),
        test_span(25u, 34u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&local, 0, sizeof(local));
    local.name = cm_hir_intern(&context, "value");
    local.type = u32_type;
    local.span = test_span(35u, 39u);
    local.parameter_index = 0u;
    for (index = 0u; index < 2u; ++index) {
        assert(cm_hir_reserve_item_definition(&context, crate_id,
            test_span(35u + index * 80u, 100u + index * 80u),
            &body_definitions[index]) == CM_HIR_OK);
        memset(&body, 0, sizeof(body));
        body.owner = body_definitions[index];
        body.origin = cm_hir_body_origin_item_source(
            body_definitions[index]);
        body.state = CM_HIR_BODY_UNLOWERED;
        body.expected_type = u32_type;
        body.locals = &local;
        body.local_count = 1u;
        body.parameter_count = 1u;
        body.source = 1u;
        body.source_expression_id = index + 1u;
        body.span = test_span(35u + index * 80u,
            100u + index * 80u);
        assert(cm_hir_add_body(&context, &body, &body_ids[index])
            == CM_HIR_OK);
    }
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_ids[0];
    expression.type = u32_type;
    expression.span = test_span(40u, 41u);
    assert(cm_hir_add_expr(&context, &expression, &receiver) == CM_HIR_OK);
    expression.kind = CM_HIR_EXPR_INTEGER;
    expression.span = test_span(50u, 51u);
    expression.data.integer.low_bits = 7u;
    assert(cm_hir_add_expr(&context, &expression, &argument) == CM_HIR_OK);
    expression.span = test_span(45u, 46u);
    expression.data.integer.low_bits = 8u;
    assert(cm_hir_add_expr(&context, &expression, &reordered_argument)
        == CM_HIR_OK);
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_ids[1];
    expression.span = test_span(120u, 121u);
    expression.data.local.local_index = 0u;
    assert(cm_hir_add_expr(&context, &expression, &other_receiver)
        == CM_HIR_OK);

    arguments[0] = argument;
    traits[0] = trait_definitions[0];
    traits[1] = trait_definitions[1];
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_METHOD_CALL;
    expression.owner_body = body_ids[0];
    expression.type = u32_type;
    expression.span = test_span(40u, 55u);
    expression.data.method_call.syntax = CM_HIR_CALLABLE_DOT_METHOD;
    expression.data.method_call.method_name =
        cm_hir_intern(&context, "value");
    expression.data.method_call.receiver = receiver;
    expression.data.method_call.arguments = arguments;
    expression.data.method_call.argument_count = 1u;
    expression.data.method_call.in_scope_traits = traits;
    expression.data.method_call.in_scope_trait_count = 2u;
    assert(cm_hir_add_expr(&context, &expression, &method_call_id)
        == CM_HIR_OK);
    assert(cm_hir_set_body_root_expression(&context, body_ids[0],
            method_call_id) == CM_HIR_OK);
    {
        CmSemanticMarkResult mark_result;
        uint64_t generation;

        generation = context.semantic_generation;
        mark_result = cm_hir_semantic_mark_bodies(&context,
            &body_ids[0], 1u);
        assert(mark_result.status
                == CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
            && mark_result.body_index == 0u
            && mark_result.body == body_ids[0]
            && mark_result.expression == method_call_id
            && context.semantic_generation == generation
            && cm_hir_get_expr(&context, method_call_id)->usage
                == CM_HIR_USAGE_UNKNOWN
            && cm_hir_get_expr(&context, receiver)->usage
                == CM_HIR_USAGE_UNKNOWN
            && cm_hir_get_expr(&context, argument)->usage
                == CM_HIR_USAGE_UNKNOWN);
    }
    arguments[0] = other_receiver;
    traits[0] = non_trait_definition;
    stored = cm_hir_get_expr(&context, method_call_id);
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_METHOD_CALL
        && stored->data.method_call.arguments != arguments
        && stored->data.method_call.arguments[0] == argument
        && stored->data.method_call.in_scope_traits != traits
        && cm_hir_def_id_equal(
            stored->data.method_call.in_scope_traits[0],
            trait_definitions[0]));

    expression.data.method_call.method_name = cm_hir_intern(&context, "");
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.method_name =
        cm_hir_intern(&context, "value");
    expression.data.method_call.syntax =
        CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.syntax = CM_HIR_CALLABLE_DOT_METHOD;
    expression.data.method_call.receiver = CM_HIR_EXPR_NONE;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.receiver = receiver;
    expression.data.method_call.arguments = NULL;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.arguments = arguments;
    arguments[0] = argument;
    expression.data.method_call.in_scope_traits = traits;
    traits[0] = trait_definitions[0];
    traits[1] = trait_definitions[0];
    assert_method_call_expression_rejected(&context, &expression);
    traits[1] = non_trait_definition;
    assert_method_call_expression_rejected(&context, &expression);
    traits[1] = trait_definitions[1];
    expression.data.method_call.in_scope_traits = NULL;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.in_scope_traits = traits;
    expression.data.method_call.in_scope_trait_count = 0u;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.in_scope_trait_count = 2u;
    expression.data.method_call.receiver = other_receiver;
    expression.span = test_span(120u, 130u);
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.receiver = receiver;
    expression.span = test_span(40u, 55u);
    arguments[0] = other_receiver;
    assert_method_call_expression_rejected(&context, &expression);
    arguments[0] = argument;
    arguments[1] = reordered_argument;
    expression.data.method_call.argument_count = 2u;
    assert_method_call_expression_rejected(&context, &expression);
    expression.data.method_call.argument_count = 0u;
    assert_method_call_expression_rejected(&context, &expression);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL
        && cm_hir_dump(first_file, &context) == 0
        && cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(snprintf(expected, sizeof(expected),
        "expr#%u method-call type=ty#%u syntax=dot-method name=\"value\" "
        "receiver=expr#%u arguments=[expr#%u] traits=[%u:%u,%u:%u]",
        (unsigned int)method_call_id, (unsigned int)u32_type,
        (unsigned int)receiver, (unsigned int)argument,
        (unsigned int)trait_definitions[0].crate_id,
        (unsigned int)trait_definitions[0].index,
        (unsigned int)trait_definitions[1].crate_id,
        (unsigned int)trait_definitions[1].index) > 0
        && strstr(first_dump, expected) != NULL);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0 && fclose(first_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_aggregate_expression_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirCrateId other_crate_id;
    CmHirModuleId root_id;
    CmHirModuleId other_root_id;
    CmHirDefId struct_definition;
    CmHirDefId tuple_definition;
    CmHirDefId cross_crate_definition;
    CmHirDefId body_definition;
    CmHirDefId other_body_definition;
    CmHirField declaration_fields[2];
    CmHirField tuple_field;
    CmHirField cross_crate_field;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId u16_type;
    CmHirTypeId struct_type;
    CmHirTypeId tuple_type;
    CmHirTypeId cross_crate_type;
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirBodyId other_body_id;
    CmHirExprId u8_value;
    CmHirExprId later_u8_value;
    CmHirExprId u16_value;
    CmHirExprId other_body_value;
    CmHirAggregateFieldValue field_values[2];
    CmHirAggregateFieldValue *owned_fields;
    CmHirAggregateFieldValue *wrong_owner;
    CmHirExpr aggregate;
    CmHirExpr field_expression;
    CmHirExpr field_block;
    CmHirExpr release_probe;
    CmHirExprId aggregate_id;
    CmHirExprId field_expression_id;
    CmHirExprId field_block_id;
    CmHirExprId owned_aggregate_id;
    CmHirExprId shared_aggregate_id;
    CmHirExprId owned_base_field_id;
    const CmHirExpr *stored;
    size_t expression_count;
    size_t arena_bytes;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    char expected[320];

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "aggregate_expression"),
        CM_HIR_EDITION_2024, test_span(0u, 300u), &crate_id, &root_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    type.span = test_span(2u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U16;
    assert(cm_hir_add_type(&context, &type, &u16_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 40u), &struct_definition) == CM_HIR_OK);
    memset(declaration_fields, 0, sizeof(declaration_fields));
    declaration_fields[0].name = cm_hir_intern(&context, "first");
    declaration_fields[0].type = u8_type;
    declaration_fields[0].visibility.kind = CM_HIR_VIS_PRIVATE;
    declaration_fields[0].visibility.restriction = cm_hir_def_id_none();
    declaration_fields[0].span = test_span(20u, 25u);
    declaration_fields[1].name = cm_hir_intern(&context, "second");
    declaration_fields[1].type = u16_type;
    declaration_fields[1].visibility.kind = CM_HIR_VIS_PRIVATE;
    declaration_fields[1].visibility.restriction = cm_hir_def_id_none();
    declaration_fields[1].span = test_span(27u, 33u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, struct_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Pair"),
        test_span(10u, 40u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = declaration_fields;
    item.data.aggregate_item.field_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(41u, 45u);
    type.data.named_type.definition = struct_definition;
    assert(cm_hir_add_type(&context, &type, &struct_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(50u, 70u), &tuple_definition) == CM_HIR_OK);
    memset(&tuple_field, 0, sizeof(tuple_field));
    tuple_field.type = u8_type;
    tuple_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    tuple_field.visibility.restriction = cm_hir_def_id_none();
    tuple_field.span = test_span(55u, 60u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, tuple_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Tuple"),
        test_span(50u, 70u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_TUPLE;
    item.data.aggregate_item.fields = &tuple_field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(71u, 75u);
    type.data.named_type.definition = tuple_definition;
    assert(cm_hir_add_type(&context, &type, &tuple_type) == CM_HIR_OK);

    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "other_crate"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &other_crate_id, &other_root_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, other_crate_id,
        test_span(76u, 90u), &cross_crate_definition) == CM_HIR_OK);
    memset(&cross_crate_field, 0, sizeof(cross_crate_field));
    cross_crate_field.name = cm_hir_intern(&context, "value");
    cross_crate_field.type = u8_type;
    cross_crate_field.visibility.kind = CM_HIR_VIS_PRIVATE;
    cross_crate_field.visibility.restriction = cm_hir_def_id_none();
    cross_crate_field.span = test_span(80u, 85u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, cross_crate_definition,
        other_root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "Foreign"), test_span(76u, 90u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &cross_crate_field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(91u, 95u);
    type.data.named_type.definition = cross_crate_definition;
    assert(cm_hir_add_type(&context, &type, &cross_crate_type)
        == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(100u, 200u), &body_definition) == CM_HIR_OK);
    memset(&body, 0, sizeof(body));
    body.owner = body_definition;
    body.origin = cm_hir_body_origin_item_source(body_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = struct_type;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(100u, 200u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(100u, 200u), &other_body_definition) == CM_HIR_OK);
    body.owner = other_body_definition;
    body.origin = cm_hir_body_origin_item_source(other_body_definition);
    body.expected_type = u16_type;
    body.source_expression_id = 2u;
    assert(cm_hir_add_body(&context, &body, &other_body_id) == CM_HIR_OK);

    u8_value = add_test_integer_expression(&context, body_id, u8_type,
        test_span(132u, 133u));
    u16_value = add_test_integer_expression(&context, body_id, u16_type,
        test_span(120u, 121u));
    later_u8_value = add_test_integer_expression(&context, body_id, u8_type,
        test_span(140u, 141u));
    other_body_value = add_test_integer_expression(&context, other_body_id,
        u16_type, test_span(120u, 121u));

    memset(field_values, 0, sizeof(field_values));
    field_values[0].field_index = 1u;
    field_values[0].value = u16_value;
    field_values[0].span = test_span(118u, 124u);
    field_values[1].field_index = 0u;
    field_values[1].value = u8_value;
    field_values[1].span = test_span(130u, 136u);
    memset(&aggregate, 0, sizeof(aggregate));
    aggregate.kind = CM_HIR_EXPR_AGGREGATE;
    aggregate.owner_body = body_id;
    aggregate.type = struct_type;
    aggregate.span = test_span(110u, 150u);
    aggregate.data.aggregate.definition = struct_definition;
    aggregate.data.aggregate.fields = field_values;
    aggregate.data.aggregate.field_count = 2u;
    aggregate.data.aggregate.owned_storage = field_values;

    aggregate.data.aggregate.field_count = 1u;
    assert_aggregate_expression_rejected(&context, &aggregate);
    aggregate.data.aggregate.field_count = 2u;
    field_values[0].field_index = 0u;
    field_values[0].value = u8_value;
    field_values[0].span = test_span(130u, 136u);
    field_values[1].field_index = 0u;
    field_values[1].value = later_u8_value;
    field_values[1].span = test_span(138u, 142u);
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].field_index = 2u;
    assert_aggregate_expression_rejected(&context, &aggregate);

    field_values[0].field_index = 1u;
    field_values[0].value = u16_value;
    field_values[0].span = test_span(118u, 124u);
    field_values[1].field_index = 0u;
    field_values[1].value = u8_value;
    field_values[1].span = test_span(130u, 136u);
    aggregate.data.aggregate.definition = tuple_definition;
    assert_aggregate_expression_rejected(&context, &aggregate);
    aggregate.data.aggregate.definition = struct_definition;
    aggregate.type = tuple_type;
    assert_aggregate_expression_rejected(&context, &aggregate);
    aggregate.type = struct_type;

    aggregate.data.aggregate.definition = tuple_definition;
    aggregate.type = tuple_type;
    aggregate.data.aggregate.field_count = 1u;
    field_values[0].field_index = 0u;
    field_values[0].value = u8_value;
    field_values[0].span = test_span(130u, 136u);
    assert_aggregate_expression_rejected(&context, &aggregate);
    aggregate.data.aggregate.definition = cross_crate_definition;
    aggregate.type = cross_crate_type;
    assert_aggregate_expression_rejected(&context, &aggregate);

    aggregate.data.aggregate.definition = struct_definition;
    aggregate.type = struct_type;
    aggregate.data.aggregate.field_count = 2u;
    field_values[0].field_index = 1u;
    field_values[0].value = other_body_value;
    field_values[0].span = test_span(118u, 124u);
    field_values[1].field_index = 0u;
    field_values[1].value = u8_value;
    field_values[1].span = test_span(130u, 136u);
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].value = u8_value;
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].value = u16_value;

    field_values[0].span = test_span(108u, 124u);
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].span = test_span(122u, 124u);
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].span = test_span(118u, 124u);
    field_values[0].span.source = 2u;
    assert_aggregate_expression_rejected(&context, &aggregate);
    field_values[0].span = test_span(130u, 136u);
    field_values[1].field_index = 1u;
    field_values[1].value = u16_value;
    field_values[1].span = test_span(118u, 124u);
    field_values[0].field_index = 0u;
    field_values[0].value = u8_value;
    assert_aggregate_expression_rejected(&context, &aggregate);
    aggregate.span = test_span(90u, 150u);
    assert_aggregate_expression_rejected(&context, &aggregate);

    aggregate.span = test_span(110u, 150u);
    field_values[0].field_index = 1u;
    field_values[0].value = u16_value;
    field_values[0].span = test_span(118u, 124u);
    field_values[1].field_index = 0u;
    field_values[1].value = u8_value;
    field_values[1].span = test_span(130u, 136u);
    expression_count = context.expressions.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_expr(&context, &aggregate, &aggregate_id)
        == CM_HIR_OK);
    assert(context.expressions.len == expression_count + 1u);
    assert(cm_arena_bytes_used(&context.storage) > arena_bytes);
    stored = cm_hir_get_expr(&context, aggregate_id);
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_AGGREGATE
        && stored->data.aggregate.fields != field_values
        && stored->data.aggregate.owned_storage == NULL
        && stored->data.aggregate.field_count == 2u
        && stored->data.aggregate.fields[0].field_index == 1u
        && stored->data.aggregate.fields[0].value == u16_value
        && stored->data.aggregate.fields[1].field_index == 0u
        && stored->data.aggregate.fields[1].value == u8_value);

    field_values[0].field_index = 0u;
    field_values[0].value = u8_value;
    field_values[0].span = test_span(1u, 1u);
    assert(stored->data.aggregate.fields[0].field_index == 1u
        && stored->data.aggregate.fields[0].value == u16_value
        && stored->data.aggregate.fields[0].span.start == 118u);

    memset(&field_expression, 0, sizeof(field_expression));
    field_expression.kind = CM_HIR_EXPR_FIELD;
    field_expression.owner_body = body_id;
    field_expression.type = u8_type;
    field_expression.span = test_span(110u, 155u);
    field_expression.data.field.base = aggregate_id;
    field_expression.data.field.definition = struct_definition;
    field_expression.data.field.field_index = 2u;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.data.field.field_index = 0u;
    field_expression.type = u16_type;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.type = u8_type;
    field_expression.data.field.definition = tuple_definition;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.data.field.definition = struct_definition;
    field_expression.data.field.base = u8_value;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.data.field.base = aggregate_id;
    field_expression.span = test_span(120u, 155u);
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.span = test_span(110u, 155u);
    field_expression.owner_body = other_body_id;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.owner_body = body_id;
    field_expression.span.source = 2u;
    assert_field_expression_rejected(&context, &field_expression);
    field_expression.span = test_span(110u, 155u);
    expression_count = context.expressions.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_expr(&context, &field_expression,
        &field_expression_id) == CM_HIR_OK);
    assert(context.expressions.len == expression_count + 1u
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored = cm_hir_get_expr(&context, field_expression_id);
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_FIELD
        && stored->data.field.base == aggregate_id
        && cm_hir_def_id_equal(stored->data.field.definition,
            struct_definition)
        && stored->data.field.field_index == 0u
        && stored->type == u8_type && stored->owner_body == body_id);

    memset(&field_block, 0, sizeof(field_block));
    field_block.kind = CM_HIR_EXPR_BLOCK;
    field_block.owner_body = body_id;
    field_block.type = u8_type;
    field_block.span = test_span(105u, 160u);
    field_block.data.block.tail_expression = field_expression_id;
    assert(cm_hir_add_expr(&context, &field_block, &field_block_id)
        == CM_HIR_OK);
    stored = cm_hir_get_expr(&context, field_block_id);
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_BLOCK
        && stored->data.block.tail_expression == field_expression_id);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(snprintf(expected, sizeof(expected),
        "expr#%u aggregate type=ty#%u aggregate=%u:%u "
        "fields=[field(index=1,value=expr#%u,span=1:118..124),"
        "field(index=0,value=expr#%u,span=1:130..136)] "
        "owner=body#%u span=1:110..150",
        (unsigned int)aggregate_id, (unsigned int)struct_type,
        (unsigned int)struct_definition.crate_id,
        (unsigned int)struct_definition.index, (unsigned int)u16_value,
        (unsigned int)u8_value, (unsigned int)body_id) > 0);
    assert(strstr(first_dump, expected) != NULL);
    assert(snprintf(expected, sizeof(expected),
        "expr#%u field type=ty#%u base=expr#%u definition=%u:%u "
        "field=0 owner=body#%u span=1:110..155",
        (unsigned int)field_expression_id, (unsigned int)u8_type,
        (unsigned int)aggregate_id,
        (unsigned int)struct_definition.crate_id,
        (unsigned int)struct_definition.index,
        (unsigned int)body_id) > 0);
    assert(strstr(first_dump, expected) != NULL);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);

    owned_fields = (CmHirAggregateFieldValue *)cm_alloc(
        4u * sizeof(CmHirAggregateFieldValue));
    wrong_owner = (CmHirAggregateFieldValue *)cm_alloc(
        2u * sizeof(CmHirAggregateFieldValue));
    owned_fields[0].field_index = 1u;
    owned_fields[0].value = u16_value;
    owned_fields[0].span = test_span(118u, 124u);
    owned_fields[1].field_index = 0u;
    owned_fields[1].value = u8_value;
    owned_fields[1].span = test_span(130u, 136u);
    owned_fields[2] = owned_fields[0];
    owned_fields[3] = owned_fields[1];
    aggregate.data.aggregate.fields = owned_fields;
    aggregate.data.aggregate.owned_storage = wrong_owner;
    expression_count = context.expressions.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    cm_vec_reserve(&context.expressions, expression_count + 2u);
    cm_alloc_fail_after(0u);
    assert(cm_hir_add_owned_aggregate_expr(&context, &aggregate,
        &owned_aggregate_id) == CM_HIR_INVALID_ARGUMENT);
    cm_alloc_fail_never();
    assert(owned_aggregate_id == CM_HIR_EXPR_NONE
        && context.expressions.len == expression_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && aggregate.data.aggregate.fields == owned_fields
        && aggregate.data.aggregate.owned_storage == wrong_owner);
    cm_free(wrong_owner);

    aggregate.data.aggregate.owned_storage = owned_fields;
    owned_fields[0].field_index = 0u;
    cm_alloc_fail_after(0u);
    assert(cm_hir_add_owned_aggregate_expr(&context, &aggregate,
        &owned_aggregate_id) == CM_HIR_INVARIANT_VIOLATION);
    cm_alloc_fail_never();
    assert(owned_aggregate_id == CM_HIR_EXPR_NONE
        && context.expressions.len == expression_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && aggregate.data.aggregate.fields == owned_fields
        && aggregate.data.aggregate.owned_storage == owned_fields);
    owned_fields[0].field_index = 1u;
    cm_alloc_fail_after(0u);
    assert(cm_hir_add_owned_aggregate_expr(&context, &aggregate,
        &owned_aggregate_id) == CM_HIR_OK);
    cm_alloc_fail_never();
    assert(context.expressions.len == expression_count + 1u
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored = cm_hir_get_expr(&context, owned_aggregate_id);
    assert(stored != NULL && stored->data.aggregate.fields == owned_fields
        && stored->data.aggregate.owned_storage == owned_fields);

    aggregate.data.aggregate.fields = owned_fields + 2u;
    aggregate.data.aggregate.owned_storage = NULL;
    cm_alloc_fail_after(0u);
    assert(cm_hir_add_owned_aggregate_expr(&context, &aggregate,
        &shared_aggregate_id) == CM_HIR_OK);
    cm_alloc_fail_never();
    assert(context.expressions.len == expression_count + 2u
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored = cm_hir_get_expr(&context, shared_aggregate_id);
    assert(stored != NULL
        && stored->data.aggregate.fields == owned_fields + 2u
        && stored->data.aggregate.owned_storage == NULL);

    field_expression.data.field.base = owned_aggregate_id;
    expression_count = context.expressions.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    cm_vec_reserve(&context.expressions, expression_count + 1u);
    cm_alloc_fail_after(0u);
    assert(cm_hir_add_expr(&context, &field_expression,
        &owned_base_field_id) == CM_HIR_OK);
    cm_alloc_fail_never();
    assert(context.expressions.len == expression_count + 1u
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored = cm_hir_get_expr(&context, owned_aggregate_id);
    assert(stored != NULL
        && stored->data.aggregate.fields == owned_fields
        && stored->data.aggregate.owned_storage == owned_fields);

    ((CmHirBody *)cm_vec_at(&context.bodies,
        (size_t)body_id - 1u))->expected_type = u8_type;
    ((CmHirExpr *)cm_vec_at(&context.expressions,
        (size_t)later_u8_value - 1u))->owner_body = other_body_id;
    ((CmHirExpr *)cm_vec_at(&context.expressions,
        (size_t)owned_aggregate_id - 1u))->owner_body = other_body_id;
    ((CmHirExpr *)cm_vec_at(&context.expressions,
        (size_t)shared_aggregate_id - 1u))->owner_body = other_body_id;
    ((CmHirExpr *)cm_vec_at(&context.expressions,
        (size_t)owned_base_field_id - 1u))->owner_body = other_body_id;
    assert(cm_hir_set_body_root_expression(&context, body_id,
            field_block_id) == CM_HIR_OK);
    {
        CmSemanticMarkResult mark_result;
        const CmHirExpr *marked_block;
        const CmHirExpr *marked_field;
        const CmHirExpr *marked_aggregate;
        const CmHirExpr *marked_u8;
        const CmHirExpr *marked_u16;
        CmHirExpr *spoofed;
        CmHirValueUsage saved_usage;
        uint64_t generation;

        generation = context.semantic_generation;
        mark_result = cm_hir_semantic_mark_bodies(&context, &body_id, 1u);
        marked_block = cm_hir_get_expr(&context, field_block_id);
        marked_field = cm_hir_get_expr(&context, field_expression_id);
        marked_aggregate = cm_hir_get_expr(&context, aggregate_id);
        marked_u8 = cm_hir_get_expr(&context, u8_value);
        marked_u16 = cm_hir_get_expr(&context, u16_value);
        assert(mark_result.status == CM_SEMANTIC_MARK_OK
            && context.semantic_generation == generation + UINT64_C(1)
            && marked_block != NULL
            && marked_block->usage == CM_HIR_USAGE_MOVE
            && marked_field != NULL
            && marked_field->usage == CM_HIR_USAGE_MOVE
            && marked_aggregate != NULL
            && marked_aggregate->usage == CM_HIR_USAGE_BORROW
            && marked_aggregate->static_borrow_state
                == CM_HIR_STATIC_BORROW_NOT_PROMOTED
            && marked_u8 != NULL && marked_u8->usage == CM_HIR_USAGE_MOVE
            && marked_u16 != NULL && marked_u16->usage == CM_HIR_USAGE_MOVE);
        generation = context.semantic_generation;
        mark_result = cm_hir_semantic_mark_bodies(&context, &body_id, 1u);
        assert(mark_result.status == CM_SEMANTIC_MARK_INVALID_HIR
            && context.semantic_generation == generation);
        spoofed = (CmHirExpr *)cm_vec_at(&context.expressions,
            (size_t)u8_value - 1u);
        assert(spoofed != NULL);
        saved_usage = spoofed->usage;
        spoofed->usage = CM_HIR_USAGE_UNKNOWN;
        spoofed->static_borrow_state = CM_HIR_STATIC_BORROW_UNKNOWN;
        mark_result = cm_hir_semantic_mark_bodies(&context, &body_id, 1u);
        assert(mark_result.status == CM_SEMANTIC_MARK_INVALID_HIR
            && context.semantic_generation == generation);
        spoofed->usage = saved_usage;
        spoofed->static_borrow_state =
            CM_HIR_STATIC_BORROW_NOT_PROMOTED;
    }

    memset(&release_probe, 0, sizeof(release_probe));
    release_probe.kind = CM_HIR_EXPR_AGGREGATE;
    release_probe.data.aggregate.fields =
        (CmHirAggregateFieldValue *)cm_alloc(
            sizeof(CmHirAggregateFieldValue));
    release_probe.data.aggregate.owned_storage =
        release_probe.data.aggregate.fields;
    cm_hir_release_expr_owned_storage(&release_probe);
    assert(release_probe.data.aggregate.fields == NULL
        && release_probe.data.aggregate.owned_storage == NULL);
    cm_hir_release_expr_owned_storage(&release_probe);
    cm_hir_context_destroy(&context);
}

static void test_trait_predicate_lifetime_binder_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId bound_definition;
    CmHirDefId owner_definition;
    CmHirDefId method_definition;
    CmHirDefId scoped_definition;
    CmHirDefId rejected_definition;
    CmHirGenericParam lifetime_parameter;
    CmHirGenericParamId lifetime_parameter_id;
    CmHirType type;
    CmHirTypeId self_type;
    CmHirTypeId unit_type;
    CmHirTypeId late_reference_type;
    CmHirGenericArg argument;
    CmHirTraitPredicate predicate;
    CmHirPredicateScope predicate_scope;
    CmInternId binder_names[2];
    CmHirFunctionParameter function_parameter;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirItem *stored;
    const CmInternedString *stored_name;
    const CmInternedString *stored_scope_name;
    size_t item_count;
    size_t arena_bytes;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "binder_model"), CM_HIR_EDITION_2024,
        test_span(0u, 200u), &crate_id, &root_id) == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 30u), &bound_definition) == CM_HIR_OK);
    memset(&lifetime_parameter, 0, sizeof(lifetime_parameter));
    lifetime_parameter.kind = CM_HIR_GENERIC_LIFETIME;
    lifetime_parameter.owner = bound_definition;
    lifetime_parameter.name = cm_hir_intern(&context, "'bound");
    lifetime_parameter.span = test_span(16u, 22u);
    assert(cm_hir_add_generic_param(&context, &lifetime_parameter,
        &lifetime_parameter_id) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, bound_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Bound"),
        test_span(10u, 30u));
    item.generic_parameter_start = lifetime_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    owner_definition = add_test_plain_trait(&context, crate_id, root_id,
        "Owner", test_span(31u, 50u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(40u, 41u);
    type.data.self_type.owner = owner_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);

    binder_names[0] = cm_hir_intern(&context, "'copy");
    binder_names[1] = binder_names[0];
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    argument.data.lifetime.kind = CM_HIR_REGION_LATE_BOUND;
    argument.data.lifetime.data.binder_index = 0u;
    memset(&predicate, 0, sizeof(predicate));
    predicate.subject = self_type;
    predicate.trait_type.definition = bound_definition;
    predicate.trait_type.arguments = &argument;
    predicate.trait_type.argument_count = 1u;
    predicate.binder.lifetimes = binder_names;
    predicate.binder.lifetime_count = 1u;
    predicate.binder.span = test_span(72u, 84u);
    predicate.span = test_span(70u, 90u);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(60u, 100u), &method_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        method_definition, root_id, owner_definition, "accept", self_type,
        unit_type, test_span(60u, 100u));
    item.predicates = &predicate;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    binder_names[0] = cm_hir_intern(&context, "'mutated");
    stored_name = stored == NULL || stored->predicate_count != 1u
            || stored->predicates[0].binder.lifetimes == NULL
        ? NULL : cm_interner_get(&context.strings,
            stored->predicates[0].binder.lifetimes[0]);
    assert(stored != NULL && stored->predicates != &predicate
        && stored->predicates[0].binder.lifetimes != binder_names
        && stored_name != NULL && stored_name->len == strlen("'copy")
        && memcmp(stored_name->bytes, "'copy", stored_name->len) == 0);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump, "binder=for<\"'copy\"> binder-span=1:72..84")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);

    binder_names[0] = cm_hir_intern(&context, "'scope");
    memset(&predicate_scope, 0, sizeof(predicate_scope));
    predicate_scope.subject_kind = CM_HIR_OUTLIVES_TYPE;
    predicate_scope.subject.type = self_type;
    predicate_scope.binder.lifetimes = binder_names;
    predicate_scope.binder.lifetime_count = 1u;
    predicate_scope.binder.span = test_span(108u, 120u);
    predicate_scope.trait_predicate_count = 1u;
    predicate_scope.span = test_span(106u, 132u);
    memset(&predicate.binder, 0, sizeof(predicate.binder));
    predicate.scope = 1u;
    predicate.span = predicate_scope.span;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 140u), &scoped_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        scoped_definition, root_id, owner_definition, "scoped", self_type,
        unit_type, test_span(101u, 140u));
    item.predicate_scopes = &predicate_scope;
    item.predicate_scope_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    binder_names[0] = cm_hir_intern(&context, "'changed_scope");
    stored_scope_name = stored == NULL
            || stored->predicate_scope_count != 1u
            || stored->predicate_scopes == NULL
            || stored->predicate_scopes[0].binder.lifetimes == NULL
        ? NULL : cm_interner_get(&context.strings,
            stored->predicate_scopes[0].binder.lifetimes[0]);
    assert(stored != NULL && stored->predicate_scopes != &predicate_scope
        && stored->predicate_scopes[0].binder.lifetimes != binder_names
        && stored->predicates[0].scope == 1u
        && stored_scope_name != NULL
        && stored_scope_name->len == strlen("'scope")
        && memcmp(stored_scope_name->bytes, "'scope",
            stored_scope_name->len) == 0);
    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "predicate-scope item#4 index=0 subject=ty#2 "
        "binder=for<\"'scope\">") != NULL
        && strstr(dump, "trait-predicate item#4 index=0 subject=ty#2 "
            "scope=1") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);

    binder_names[0] = cm_hir_intern(&context, "'copy");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(95u, 96u);
    type.data.reference_type.region.kind = CM_HIR_REGION_LATE_BOUND;
    type.data.reference_type.region.data.binder_index = 0u;
    type.data.reference_type.pointee = unit_type;
    assert(cm_hir_add_type(&context, &type, &late_reference_type)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 140u), &rejected_definition) == CM_HIR_OK);
    init_test_trait_function(&context, &item, &function_parameter,
        rejected_definition, root_id, owner_definition, "reject", self_type,
        unit_type, test_span(101u, 140u));
    predicate_scope.binder.lifetimes = binder_names;
    predicate_scope.binder.lifetime_count = 1u;
    predicate_scope.trait_predicate_count = 1u;
    predicate_scope.outlives_predicate_count = 0u;
    predicate.scope = 1u;
    memset(&predicate.binder, 0, sizeof(predicate.binder));
    item.predicate_scopes = &predicate_scope;
    item.predicate_scope_count = 1u;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);

    predicate_scope.trait_predicate_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate_scope.trait_predicate_count = 1u;
    predicate.scope = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate.scope = 1u;
    predicate_scope.binder.lifetimes = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate_scope.binder.lifetimes = binder_names;
    predicate_scope.subject.type = unit_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate_scope.subject.type = self_type;
    predicate.binder.lifetimes = binder_names;
    predicate.binder.lifetime_count = 1u;
    predicate.binder.span = test_span(108u, 120u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);

    item.predicate_scopes = NULL;
    item.predicate_scope_count = 0u;

    predicate.scope = CM_HIR_PREDICATE_SCOPE_NONE;
    predicate.binder.lifetimes = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate.binder.lifetimes = binder_names;
    predicate.binder.span.source = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate.binder.span = test_span(84u, 72u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate.binder.span = test_span(72u, 84u);
    binder_names[0] = CM_INTERN_ID_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    binder_names[0] = cm_hir_intern(&context, "'copy");
    predicate.binder.lifetime_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    predicate.binder.lifetime_count = 1u;
    argument.data.lifetime.data.binder_index = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    argument.data.lifetime.data.binder_index = 0u;
    predicate.binder.lifetime_count = 1u;
    predicate.binder.lifetimes = binder_names;
    predicate.binder.span = test_span(72u, 84u);
    item.data.function_item.signature.return_type = late_reference_type;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.function_item.signature.return_type = unit_type;
    predicate.binder.lifetime_count = 0u;
    predicate.binder.lifetimes = NULL;
    predicate.binder.span = (CmSpan){ 0u, 0u, 0u };
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    cm_hir_context_destroy(&context);
}

static void test_context_transaction_marks(void)
{
    CmHirContext context;
    CmHirContext other;
    CmHirContextMark mark;
    CmHirContextMark outer;
    CmHirContextMark inner;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId bool_type;
    size_t initial_strings;

    cm_hir_context_init(&context);
    cm_hir_context_init(&other);
    memset(&mark, 0, sizeof(mark));
    initial_strings = cm_interner_length(&context.strings);
    assert(cm_hir_context_mark(&context, &mark) == CM_HIR_OK
        && mark.active && mark.context == &context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "transient"), CM_HIR_EDITION_2024,
        test_span(0u, 10u), &crate_id, &root_module) == CM_HIR_OK);
    bool_type = add_simple_type(&context, CM_HIR_TYPE_BOOL_KIND,
        test_span(1u, 2u));
    assert(bool_type != CM_HIR_TYPE_NONE && context.crates.len == 1u
        && context.modules.len == 1u && context.definitions.len == 1u
        && context.types.len == 1u);
    assert(cm_hir_context_rewind(&other, &mark)
        == CM_HIR_INVALID_ARGUMENT);
    assert(mark.active && cm_hir_context_rewind(&context, &mark)
        == CM_HIR_OK && !mark.active
        && context.crates.len == 0u && context.modules.len == 0u
        && context.definitions.len == 0u && context.types.len == 0u
        && cm_interner_length(&context.strings) == initial_strings);
    assert(cm_hir_context_rewind(&context, &mark)
        == CM_HIR_INVALID_ARGUMENT);

    assert(cm_hir_context_mark(&context, &mark) == CM_HIR_OK);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "committed"), CM_HIR_EDITION_2024,
        test_span(0u, 10u), &crate_id, &root_module) == CM_HIR_OK);
    assert(cm_hir_context_commit(&context, &mark) == CM_HIR_OK
        && !mark.active && context.crates.len == 1u
        && context.modules.len == 1u && context.definitions.len == 1u);
    assert(cm_hir_context_commit(&context, &mark)
        == CM_HIR_INVALID_ARGUMENT);

    assert(cm_hir_context_mark(&context, &outer) == CM_HIR_OK);
    assert(cm_hir_context_mark(&context, &inner) == CM_HIR_OK);
    assert(cm_hir_intern(&context, "nested") != CM_INTERN_ID_NONE);
    assert(cm_hir_context_rewind(&context, &outer) == CM_HIR_OK);
    assert(cm_hir_context_rewind(&context, &inner)
        == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_context_mark(NULL, &mark) == CM_HIR_INVALID_ARGUMENT
        && cm_hir_context_mark(&context, NULL) == CM_HIR_INVALID_ARGUMENT);
    cm_hir_context_destroy(&other);
    cm_hir_context_destroy(&context);
}

static CmHirBodyId add_closure_mark_test_body(CmHirContext *context,
    CmHirCrateId crate_id, CmHirTypeId initial_expected,
    const CmHirLocal *locals, uint32_t local_count, uint32_t start)
{
    CmHirDefId definition;
    CmHirBody body;
    CmHirBodyId body_id;

    assert(cm_hir_reserve_item_definition(context, crate_id,
        test_span(start, start + 90u), &definition) == CM_HIR_OK);
    memset(&body, 0, sizeof(body));
    body.owner = definition;
    body.origin = cm_hir_body_origin_item_source(definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = initial_expected;
    body.locals = (CmHirLocal *)locals;
    body.local_count = local_count;
    body.parameter_count = local_count;
    body.source = 1u;
    body.source_expression_id = start;
    body.span = test_span(start, start + 90u);
    assert(cm_hir_add_body(context, &body, &body_id) == CM_HIR_OK);
    return body_id;
}

static CmHirClosureId add_closure_mark_test_shell(CmHirContext *context,
    CmHirBodyId body_id, CmHirTypeId return_type,
    uint32_t visible_local_count, int is_move, uint32_t start,
    CmHirTypeId *out_closure_type)
{
    CmHirClosureId closure_id;
    CmHirType type;

    assert(cm_hir_reserve_closure(context, body_id, start, NULL, 0u,
        return_type, visible_local_count, is_move,
        test_span(start, start + 60u), &closure_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_CLOSURE_KIND;
    type.span = test_span(start, start + 60u);
    type.data.closure_type.closure = closure_id;
    assert(cm_hir_add_type(context, &type, out_closure_type) == CM_HIR_OK);
    return closure_id;
}

static CmHirExprId finish_closure_mark_test_body(CmHirContext *context,
    CmHirBodyId body_id, CmHirClosureId closure_id,
    CmHirTypeId closure_type, CmHirExprId closure_body,
    uint32_t closure_start)
{
    CmHirBody *body;
    CmHirExpr expression;
    CmHirExprId closure_expression;

    assert(cm_hir_bind_closure_body(context, closure_id, closure_body)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE;
    expression.owner_body = body_id;
    expression.type = closure_type;
    expression.span = test_span(closure_start, closure_start + 60u);
    expression.data.closure.closure = closure_id;
    assert(cm_hir_add_expr(context, &expression, &closure_expression)
        == CM_HIR_OK);
    body = (CmHirBody *)cm_vec_at(&context->bodies,
        (size_t)body_id - 1u);
    assert(body != NULL);
    body->expected_type = closure_type;
    assert(cm_hir_set_body_root_expression(context, body_id,
        closure_expression) == CM_HIR_OK);
    return closure_expression;
}

static CmHirExprId add_closure_mark_test_local(CmHirContext *context,
    CmHirBodyId body_id, CmHirTypeId type, uint32_t local_index,
    uint32_t start)
{
    CmHirExpr expression;
    CmHirExprId expression_id;

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = type;
    expression.span = test_span(start, start + 1u);
    expression.data.local.local_index = local_index;
    assert(cm_hir_add_expr(context, &expression, &expression_id)
        == CM_HIR_OK);
    return expression_id;
}

static void test_closure_capture_semantic_mark(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirType type;
    CmHirTypeId u32_type;
    CmHirTypeId shared_u32_type;
    CmHirTypeId mutable_u32_type;
    CmHirTypeId tuple_type;
    CmHirTypeId array_type;
    CmHirTypeId tuple_elements[2];
    CmHirBodyId bodies[9];
    CmHirClosureId closures[9];
    CmHirClosureId inner_closure;
    CmHirClosureParam closure_parameter;
    CmHirTypeId closure_types[9];
    CmHirTypeId inner_closure_type;
    CmHirExprId closure_expressions[9];
    CmHirExprId body_expressions[9];
    CmHirExprId inner_body_expression;
    CmHirLocal locals[2];
    CmHirExpr expression;
    CmHirExprId reversed_left;
    CmHirExprId reversed_right;
    CmSemanticMarkResult result;
    const CmHirClosure *closure;
    uint64_t generation;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "closure_capture_semantic_mark"),
        CM_HIR_EDITION_2021, test_span(0u, 1000u), &crate_id,
        &root_id) == CM_HIR_OK);
    assert(root_id != CM_HIR_MODULE_NONE);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&context, &type, &u32_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(3u, 4u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = u32_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &shared_u32_type)
        == CM_HIR_OK);
    type.span = test_span(5u, 6u);
    type.data.reference_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &mutable_u32_type)
        == CM_HIR_OK);
    tuple_elements[0] = u32_type;
    tuple_elements[1] = shared_u32_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(7u, 8u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    assert(cm_hir_add_type(&context, &type, &tuple_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(9u, 10u);
    type.data.array_type.element = u32_type;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = u32_type;
    type.data.array_type.length.data.value.low_bits = 3u;
    assert(cm_hir_add_type(&context, &type, &array_type) == CM_HIR_OK);

    bodies[0] = add_closure_mark_test_body(&context, crate_id, u32_type,
        NULL, 0u, 100u);
    closures[0] = add_closure_mark_test_shell(&context, bodies[0],
        u32_type, 0u, 0, 110u, &closure_types[0]);
    body_expressions[0] = add_test_integer_expression(&context, bodies[0],
        u32_type, test_span(120u, 121u));
    closure_expressions[0] = finish_closure_mark_test_body(&context,
        bodies[0], closures[0], closure_types[0], body_expressions[0],
        110u);

    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "first");
    locals[0].type = u32_type;
    locals[0].span = test_span(201u, 202u);
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(&context, "second");
    locals[1].type = u32_type;
    locals[1].span = test_span(203u, 204u);
    locals[1].parameter_index = 1u;
    bodies[1] = add_closure_mark_test_body(&context, crate_id, u32_type,
        locals, 2u, 200u);
    closures[1] = add_closure_mark_test_shell(&context, bodies[1],
        u32_type, 2u, 0, 210u, &closure_types[1]);
    reversed_left = add_closure_mark_test_local(&context, bodies[1],
        u32_type, 1u, 220u);
    reversed_right = add_closure_mark_test_local(&context, bodies[1],
        u32_type, 0u, 222u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_BINARY;
    expression.owner_body = bodies[1];
    expression.type = u32_type;
    expression.span = test_span(218u, 225u);
    expression.data.binary.operator_kind = CM_HIR_BINARY_ADD;
    expression.data.binary.left = reversed_left;
    expression.data.binary.right = reversed_right;
    assert(cm_hir_add_expr(&context, &expression, &body_expressions[1])
        == CM_HIR_OK);
    closure_expressions[1] = finish_closure_mark_test_body(&context,
        bodies[1], closures[1], closure_types[1], body_expressions[1],
        210u);

    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "moved_copy");
    locals[0].type = u32_type;
    locals[0].span = test_span(301u, 302u);
    locals[0].parameter_index = 0u;
    bodies[2] = add_closure_mark_test_body(&context, crate_id, u32_type,
        locals, 1u, 300u);
    closures[2] = add_closure_mark_test_shell(&context, bodies[2],
        u32_type, 1u, 1, 310u, &closure_types[2]);
    body_expressions[2] = add_closure_mark_test_local(&context, bodies[2],
        u32_type, 0u, 320u);
    closure_expressions[2] = finish_closure_mark_test_body(&context,
        bodies[2], closures[2], closure_types[2], body_expressions[2],
        310u);

    locals[0].name = cm_hir_intern(&context, "shared_reference");
    locals[0].type = shared_u32_type;
    locals[0].span = test_span(401u, 402u);
    bodies[3] = add_closure_mark_test_body(&context, crate_id,
        shared_u32_type, locals, 1u, 400u);
    closures[3] = add_closure_mark_test_shell(&context, bodies[3],
        shared_u32_type, 1u, 0, 410u, &closure_types[3]);
    body_expressions[3] = add_closure_mark_test_local(&context, bodies[3],
        shared_u32_type, 0u, 420u);
    closure_expressions[3] = finish_closure_mark_test_body(&context,
        bodies[3], closures[3], closure_types[3], body_expressions[3],
        410u);

    locals[0].name = cm_hir_intern(&context, "mutable_reference");
    locals[0].type = mutable_u32_type;
    locals[0].span = test_span(501u, 502u);
    bodies[4] = add_closure_mark_test_body(&context, crate_id,
        mutable_u32_type, locals, 1u, 500u);
    closures[4] = add_closure_mark_test_shell(&context, bodies[4],
        mutable_u32_type, 1u, 0, 510u, &closure_types[4]);
    body_expressions[4] = add_closure_mark_test_local(&context, bodies[4],
        mutable_u32_type, 0u, 520u);
    closure_expressions[4] = finish_closure_mark_test_body(&context,
        bodies[4], closures[4], closure_types[4], body_expressions[4],
        510u);

    locals[0].name = cm_hir_intern(&context, "nested_root");
    locals[0].type = u32_type;
    locals[0].span = test_span(601u, 602u);
    bodies[5] = add_closure_mark_test_body(&context, crate_id, u32_type,
        locals, 1u, 600u);
    assert(cm_hir_reserve_closure(&context, bodies[5], 621u, NULL, 0u,
        u32_type, 1u, 1, test_span(620u, 650u), &inner_closure)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_CLOSURE_KIND;
    type.span = test_span(620u, 650u);
    type.data.closure_type.closure = inner_closure;
    assert(cm_hir_add_type(&context, &type, &inner_closure_type)
        == CM_HIR_OK);
    closures[5] = add_closure_mark_test_shell(&context, bodies[5],
        inner_closure_type, 1u, 0, 610u, &closure_types[5]);
    inner_body_expression = add_closure_mark_test_local(&context,
        bodies[5], u32_type, 0u, 630u);
    assert(cm_hir_bind_closure_body(&context, inner_closure,
        inner_body_expression) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE;
    expression.owner_body = bodies[5];
    expression.type = inner_closure_type;
    expression.span = test_span(620u, 650u);
    expression.data.closure.closure = inner_closure;
    assert(cm_hir_add_expr(&context, &expression, &body_expressions[5])
        == CM_HIR_OK);
    closure_expressions[5] = finish_closure_mark_test_body(&context,
        bodies[5], closures[5], closure_types[5], body_expressions[5],
        610u);

    locals[0].name = cm_hir_intern(&context, "copy_tuple");
    locals[0].type = tuple_type;
    locals[0].span = test_span(701u, 702u);
    bodies[6] = add_closure_mark_test_body(&context, crate_id, tuple_type,
        locals, 1u, 700u);
    closures[6] = add_closure_mark_test_shell(&context, bodies[6],
        tuple_type, 1u, 0, 710u, &closure_types[6]);
    body_expressions[6] = add_closure_mark_test_local(&context, bodies[6],
        tuple_type, 0u, 720u);
    closure_expressions[6] = finish_closure_mark_test_body(&context,
        bodies[6], closures[6], closure_types[6], body_expressions[6],
        710u);

    locals[0].name = cm_hir_intern(&context, "copy_array");
    locals[0].type = array_type;
    locals[0].span = test_span(801u, 802u);
    bodies[7] = add_closure_mark_test_body(&context, crate_id, array_type,
        locals, 1u, 800u);
    closures[7] = add_closure_mark_test_shell(&context, bodies[7],
        array_type, 1u, 0, 810u, &closure_types[7]);
    body_expressions[7] = add_closure_mark_test_local(&context, bodies[7],
        array_type, 0u, 820u);
    closure_expressions[7] = finish_closure_mark_test_body(&context,
        bodies[7], closures[7], closure_types[7], body_expressions[7],
        810u);

    bodies[8] = add_closure_mark_test_body(&context, crate_id, u32_type,
        NULL, 0u, 900u);
    memset(&closure_parameter, 0, sizeof(closure_parameter));
    closure_parameter.name = cm_hir_intern(&context, "parameter");
    closure_parameter.type = u32_type;
    closure_parameter.span = test_span(920u, 925u);
    closure_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    assert(cm_hir_reserve_closure(&context, bodies[8], 910u,
        &closure_parameter, 1u, u32_type, 0u, 0,
        test_span(910u, 970u), &closures[8]) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_CLOSURE_KIND;
    type.span = test_span(910u, 970u);
    type.data.closure_type.closure = closures[8];
    assert(cm_hir_add_type(&context, &type, &closure_types[8]) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE_PARAMETER;
    expression.owner_body = bodies[8];
    expression.type = u32_type;
    expression.span = test_span(922u, 923u);
    expression.data.closure_parameter.closure = closures[8];
    assert(cm_hir_add_expr(&context, &expression, &body_expressions[8])
        == CM_HIR_OK);
    closure_expressions[8] = finish_closure_mark_test_body(&context,
        bodies[8], closures[8], closure_types[8], body_expressions[8],
        910u);

    generation = context.semantic_generation;
    result = cm_hir_semantic_mark_bodies(&context, bodies,
        CM_ARRAY_LEN(bodies));
    assert(result.status == CM_SEMANTIC_MARK_OK
        && context.semantic_generation == generation + UINT64_C(1));
    closure = cm_hir_get_closure(&context, closures[0]);
    assert(closure != NULL
        && closure->capture_state == CM_HIR_CLOSURE_CAPTURES_MARKED
        && closure->capture_count == 0u && closure->captures == NULL
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_NO_CAPTURE
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[1]);
    assert(closure != NULL && closure->capture_count == 2u
        && closure->captures[0].local_index == 0u
        && closure->captures[1].local_index == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_BORROW
        && closure->captures[1].usage == CM_HIR_USAGE_BORROW
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[2]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_MOVE
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[3]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_MOVE
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[4]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_MUTATE
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_MUT
        && closure->is_copy == 0);
    closure = cm_hir_get_closure(&context, inner_closure);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].local_index == 0u
        && closure->captures[0].usage == CM_HIR_USAGE_MOVE
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[5]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].local_index == 0u
        && closure->captures[0].usage == CM_HIR_USAGE_BORROW
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[6]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_BORROW
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[7]);
    assert(closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_BORROW
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1);
    closure = cm_hir_get_closure(&context, closures[8]);
    assert(closure != NULL && closure->capture_count == 0u
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_NO_CAPTURE
        && closure->is_copy == 1);
    assert(cm_hir_get_expr(&context, closure_expressions[0])->usage
            == CM_HIR_USAGE_MOVE
        && cm_hir_get_expr(&context, closure_expressions[4])->usage
            == CM_HIR_USAGE_MOVE);
    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strstr(dump,
        "captures=marked class=shared copy=1 capture-values=[capture("
        "local=0,type=ty#1,usage=borrow)") != NULL
        && strstr(dump, "captures=marked class=mut copy=0") != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    generation = context.semantic_generation;
    result = cm_hir_semantic_mark_bodies(&context, bodies,
        CM_ARRAY_LEN(bodies));
    assert(result.status == CM_SEMANTIC_MARK_INVALID_HIR
        && context.semantic_generation == generation);
    cm_hir_context_destroy(&context);
}

static void test_closure_capture_semantic_mark_failures(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId aggregate_definition;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirField field;
    CmHirType type;
    CmHirTypeId u32_type;
    CmHirTypeId aggregate_type;
    CmHirTypeId zero_array_type;
    CmHirBodyId bodies[2];
    CmHirBodyId field_body;
    CmHirBodyId zero_array_body;
    CmHirClosureId valid_closure;
    CmHirClosureId unknown_closure;
    CmHirClosureId field_closure;
    CmHirClosureId zero_array_closure;
    CmHirTypeId valid_closure_type;
    CmHirTypeId unknown_closure_type;
    CmHirTypeId field_closure_type;
    CmHirTypeId zero_array_closure_type;
    CmHirLocal local;
    CmHirExpr expression;
    CmHirExprId valid_body_expression;
    CmHirExprId valid_closure_expression;
    CmHirExprId unknown_local;
    CmHirExprId unknown_closure_expression;
    CmHirExprId field_base;
    CmHirExprId field_expression;
    CmHirExprId field_closure_expression;
    CmHirExprId zero_array_local;
    CmHirExprId zero_array_closure_expression;
    CmSemanticMarkResult result;
    const CmHirClosure *closure;
    uint64_t generation;
    size_t arena_bytes;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "closure_capture_failures"),
        CM_HIR_EDITION_2021, test_span(0u, 500u), &crate_id,
        &root_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&context, &type, &u32_type) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 40u), &aggregate_definition) == CM_HIR_OK);
    memset(&field, 0, sizeof(field));
    field.name = cm_hir_intern(&context, "value");
    field.type = u32_type;
    field.visibility.kind = CM_HIR_VIS_PRIVATE;
    field.visibility.restriction = cm_hir_def_id_none();
    field.span = test_span(20u, 25u);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, aggregate_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "Aggregate"), test_span(10u, 40u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = &field;
    item.data.aggregate_item.field_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    assert(item_id != CM_HIR_ITEM_NONE);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(41u, 42u);
    type.data.named_type.definition = aggregate_definition;
    assert(cm_hir_add_type(&context, &type, &aggregate_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(43u, 44u);
    type.data.array_type.element = aggregate_type;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = u32_type;
    assert(cm_hir_add_type(&context, &type, &zero_array_type) == CM_HIR_OK);

    bodies[0] = add_closure_mark_test_body(&context, crate_id, u32_type,
        NULL, 0u, 100u);
    valid_closure = add_closure_mark_test_shell(&context, bodies[0],
        u32_type, 0u, 0, 110u, &valid_closure_type);
    valid_body_expression = add_test_integer_expression(&context, bodies[0],
        u32_type, test_span(120u, 121u));
    valid_closure_expression = finish_closure_mark_test_body(&context,
        bodies[0], valid_closure, valid_closure_type,
        valid_body_expression, 110u);

    memset(&local, 0, sizeof(local));
    local.name = cm_hir_intern(&context, "unknown_copy");
    local.type = aggregate_type;
    local.span = test_span(201u, 202u);
    local.parameter_index = 0u;
    bodies[1] = add_closure_mark_test_body(&context, crate_id,
        aggregate_type, &local, 1u, 200u);
    unknown_closure = add_closure_mark_test_shell(&context, bodies[1],
        aggregate_type, 1u, 0, 210u, &unknown_closure_type);
    unknown_local = add_closure_mark_test_local(&context, bodies[1],
        aggregate_type, 0u, 220u);
    unknown_closure_expression = finish_closure_mark_test_body(&context,
        bodies[1], unknown_closure, unknown_closure_type, unknown_local,
        210u);

    generation = context.semantic_generation;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    result = cm_hir_semantic_mark_bodies(&context, bodies,
        CM_ARRAY_LEN(bodies));
    closure = cm_hir_get_closure(&context, valid_closure);
    assert(result.status == CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
        && result.body_index == 1u && result.body == bodies[1]
        && result.expression == unknown_local
        && context.semantic_generation == generation
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && closure != NULL
        && closure->capture_state == CM_HIR_CLOSURE_CAPTURES_UNMARKED
        && closure->captures == NULL && closure->capture_count == 0u
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_UNKNOWN
        && cm_hir_get_closure(&context, unknown_closure)->capture_state
            == CM_HIR_CLOSURE_CAPTURES_UNMARKED
        && cm_hir_get_expr(&context, valid_body_expression)->usage
            == CM_HIR_USAGE_UNKNOWN
        && cm_hir_get_expr(&context, valid_closure_expression)->usage
            == CM_HIR_USAGE_UNKNOWN
        && cm_hir_get_expr(&context, unknown_local)->usage
            == CM_HIR_USAGE_UNKNOWN
        && cm_hir_get_expr(&context, unknown_closure_expression)->usage
            == CM_HIR_USAGE_UNKNOWN);

    local.name = cm_hir_intern(&context, "projected");
    local.span = test_span(301u, 302u);
    field_body = add_closure_mark_test_body(&context, crate_id, u32_type,
        &local, 1u, 300u);
    field_closure = add_closure_mark_test_shell(&context, field_body,
        u32_type, 1u, 0, 310u, &field_closure_type);
    field_base = add_closure_mark_test_local(&context, field_body,
        aggregate_type, 0u, 320u);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_FIELD;
    expression.owner_body = field_body;
    expression.type = u32_type;
    expression.span = test_span(320u, 323u);
    expression.data.field.base = field_base;
    expression.data.field.definition = aggregate_definition;
    expression.data.field.field_index = 0u;
    assert(cm_hir_add_expr(&context, &expression, &field_expression)
        == CM_HIR_OK);
    field_closure_expression = finish_closure_mark_test_body(&context,
        field_body, field_closure, field_closure_type, field_expression,
        310u);
    generation = context.semantic_generation;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    result = cm_hir_semantic_mark_bodies(&context, &field_body, 1u);
    closure = cm_hir_get_closure(&context, field_closure);
    assert(result.status == CM_SEMANTIC_MARK_UNSUPPORTED_EXPRESSION
        && result.body_index == 0u && result.body == field_body
        && result.expression == field_expression
        && context.semantic_generation == generation
        && cm_arena_bytes_used(&context.storage) == arena_bytes
        && closure != NULL
        && closure->capture_state == CM_HIR_CLOSURE_CAPTURES_UNMARKED
        && closure->captures == NULL && closure->capture_count == 0u
        && cm_hir_get_expr(&context, field_base)->usage
            == CM_HIR_USAGE_UNKNOWN
        && cm_hir_get_expr(&context, field_expression)->usage
            == CM_HIR_USAGE_UNKNOWN
        && cm_hir_get_expr(&context, field_closure_expression)->usage
            == CM_HIR_USAGE_UNKNOWN);

    local.name = cm_hir_intern(&context, "zero_array");
    local.type = zero_array_type;
    local.span = test_span(401u, 402u);
    zero_array_body = add_closure_mark_test_body(&context, crate_id,
        zero_array_type, &local, 1u, 400u);
    zero_array_closure = add_closure_mark_test_shell(&context,
        zero_array_body, zero_array_type, 1u, 0, 410u,
        &zero_array_closure_type);
    zero_array_local = add_closure_mark_test_local(&context,
        zero_array_body, zero_array_type, 0u, 420u);
    zero_array_closure_expression = finish_closure_mark_test_body(&context,
        zero_array_body, zero_array_closure, zero_array_closure_type,
        zero_array_local, 410u);
    generation = context.semantic_generation;
    result = cm_hir_semantic_mark_bodies(&context, &zero_array_body, 1u);
    closure = cm_hir_get_closure(&context, zero_array_closure);
    assert(result.status == CM_SEMANTIC_MARK_OK
        && context.semantic_generation == generation + UINT64_C(1)
        && closure != NULL && closure->capture_count == 1u
        && closure->captures[0].usage == CM_HIR_USAGE_BORROW
        && closure->callable_class == CM_HIR_CLOSURE_CLASS_SHARED
        && closure->is_copy == 1
        && cm_hir_get_expr(&context, zero_array_local)->usage
            == CM_HIR_USAGE_MOVE
        && cm_hir_get_expr(&context, zero_array_closure_expression)->usage
            == CM_HIR_USAGE_MOVE);
    cm_hir_context_destroy(&context);
}

static void test_closure_hir_model(void)
{
    CmHirContext context;
    CmHirContextMark mark;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirDefId body_definition;
    CmHirType type;
    CmHirTypeId u32_type;
    CmHirTypeId closure_type;
    CmHirTypeId second_closure_type;
    CmHirLocal locals[2];
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirClosureParam parameters[2];
    CmHirClosureId closure_id;
    CmHirClosureId second_closure_id;
    CmHirClosureId rejected_closure_id;
    CmHirExpr expression;
    CmHirExprId parameter_expression;
    CmHirExprId later_local_expression;
    CmHirExprId closure_expression;
    CmHirExprId rejected_expression;
    const CmHirClosure *stored;
    uint64_t generation;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "closure_hir_model"), CM_HIR_EDITION_2024,
        test_span(0u, 300u), &crate_id, &root_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U32;
    assert(cm_hir_add_type(&context, &type, &u32_type) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(10u, 250u), &body_definition) == CM_HIR_OK);
    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "before");
    locals[0].type = u32_type;
    locals[0].span = test_span(20u, 25u);
    locals[0].parameter_index = 0u;
    locals[1].name = cm_hir_intern(&context, "after");
    locals[1].type = u32_type;
    locals[1].span = test_span(200u, 205u);
    locals[1].parameter_index = 1u;
    memset(&body, 0, sizeof(body));
    body.owner = body_definition;
    body.origin = cm_hir_body_origin_item_source(body_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = u32_type;
    body.locals = locals;
    body.local_count = 2u;
    body.parameter_count = 2u;
    body.source = 1u;
    body.source_expression_id = 1u;
    body.span = test_span(10u, 250u);
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    assert(root_id != CM_HIR_MODULE_NONE);

    memset(&mark, 0, sizeof(mark));
    assert(cm_hir_context_mark(&context, &mark) == CM_HIR_OK);
    memset(parameters, 0, sizeof(parameters));
    parameters[0].name = cm_hir_intern(&context, "value");
    parameters[0].type = u32_type;
    parameters[0].span = test_span(52u, 57u);
    parameters[0].binding_kind = CM_HIR_BINDING_NAMED;
    parameters[1].name = CM_INTERN_ID_NONE;
    parameters[1].type = u32_type;
    parameters[1].span = test_span(59u, 60u);
    parameters[1].binding_kind = CM_HIR_BINDING_DISCARD;
    assert(cm_hir_reserve_closure(&context, body_id, 7u, parameters, 2u,
        u32_type, 1u, 1, test_span(50u, 100u), &closure_id) == CM_HIR_OK);
    stored = cm_hir_get_closure(&context, closure_id);
    parameters[0].name = CM_INTERN_ID_NONE;
    parameters[0].type = CM_HIR_TYPE_NONE;
    assert(stored != NULL && stored->state
            == CM_HIR_CLOSURE_SIGNATURE_RESERVED
        && stored->parameters != parameters
        && stored->parameters[0].name != CM_INTERN_ID_NONE
        && stored->parameters[0].type == u32_type
        && stored->visible_local_count == 1u && stored->is_move);
    parameters[0] = stored->parameters[0];
    assert(cm_hir_reserve_closure(&context, body_id, 7u, parameters, 2u,
        u32_type, 1u, 1, test_span(50u, 100u), &rejected_closure_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(rejected_closure_id == CM_HIR_CLOSURE_NONE);
    parameters[1] = parameters[0];
    parameters[1].span = test_span(59u, 60u);
    assert(cm_hir_reserve_closure(&context, body_id, 8u, parameters, 2u,
        u32_type, 1u, 0, test_span(50u, 100u), &rejected_closure_id)
        == CM_HIR_INVARIANT_VIOLATION);
    parameters[1] = stored->parameters[1];

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_CLOSURE_KIND;
    type.span = test_span(50u, 100u);
    type.data.closure_type.closure = closure_id;
    assert(cm_hir_add_type(&context, &type, &closure_type) == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE;
    expression.owner_body = body_id;
    expression.type = closure_type;
    expression.span = test_span(50u, 100u);
    expression.data.closure.closure = closure_id;
    assert(cm_hir_add_expr(&context, &expression, &rejected_expression)
        == CM_HIR_INVARIANT_VIOLATION);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE_PARAMETER;
    expression.owner_body = body_id;
    expression.type = u32_type;
    expression.span = test_span(70u, 75u);
    expression.data.closure_parameter.closure = closure_id;
    expression.data.closure_parameter.parameter_index = 0u;
    assert(cm_hir_add_expr(&context, &expression, &parameter_expression)
        == CM_HIR_OK);
    expression.data.closure_parameter.parameter_index = 1u;
    assert(cm_hir_add_expr(&context, &expression, &rejected_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    generation = context.semantic_generation;
    assert(cm_hir_bind_closure_body(&context, closure_id,
        parameter_expression) == CM_HIR_OK);
    assert(context.semantic_generation == generation + UINT64_C(1));
    stored = cm_hir_get_closure(&context, closure_id);
    assert(stored != NULL && stored->state == CM_HIR_CLOSURE_BODY_BOUND
        && stored->body_expression == parameter_expression);
    assert(cm_hir_bind_closure_body(&context, closure_id,
        parameter_expression) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_set_body_root_expression(&context, body_id,
        parameter_expression) == CM_HIR_INVARIANT_VIOLATION);

    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_CLOSURE;
    expression.owner_body = body_id;
    expression.type = closure_type;
    expression.span = test_span(50u, 100u);
    expression.data.closure.closure = closure_id;
    assert(cm_hir_add_expr(&context, &expression, &closure_expression)
        == CM_HIR_OK);
    assert(cm_hir_get_expr(&context, closure_expression) != NULL);

    parameters[0] = stored->parameters[0];
    parameters[0].span = test_span(112u, 117u);
    assert(cm_hir_reserve_closure(&context, body_id, 9u, parameters, 1u,
        u32_type, 1u, 0, test_span(110u, 180u), &second_closure_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_CLOSURE_KIND;
    type.span = test_span(110u, 180u);
    type.data.closure_type.closure = second_closure_id;
    assert(cm_hir_add_type(&context, &type, &second_closure_type)
        == CM_HIR_OK);
    memset(&expression, 0, sizeof(expression));
    expression.kind = CM_HIR_EXPR_LOCAL;
    expression.owner_body = body_id;
    expression.type = u32_type;
    expression.span = test_span(130u, 135u);
    expression.data.local.local_index = 1u;
    assert(cm_hir_add_expr(&context, &expression, &later_local_expression)
        == CM_HIR_OK);
    assert(cm_hir_bind_closure_body(&context, second_closure_id,
        later_local_expression) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_bind_closure_body(&context, second_closure_id,
        parameter_expression) == CM_HIR_INVARIANT_VIOLATION);
    assert(second_closure_type != CM_HIR_TYPE_NONE);

    assert(context.closures.len == 2u && context.expressions.len != 0u
        && context.types.len >= 3u);
    assert(cm_hir_context_rewind(&context, &mark) == CM_HIR_OK);
    assert(context.closures.len == 0u
        && cm_hir_get_closure(&context, closure_id) == NULL
        && cm_hir_get_type(&context, closure_type) == NULL
        && cm_hir_get_expr(&context, parameter_expression) == NULL);
    cm_hir_context_destroy(&context);
}

static void test_context_generation_exhaustion_boundary(void)
{
    CmHirContext semantic_context;
    CmHirContext rewind_context;
    CmHirContextMark mark;

    cm_hir_context_init(&semantic_context);
    semantic_context.semantic_generation = UINT64_MAX - UINT64_C(1);
    cm_hir_context_record_semantic_mutation(&semantic_context);
    assert(semantic_context.semantic_generation == UINT64_MAX);
    cm_hir_context_destroy(&semantic_context);

    cm_hir_context_init(&rewind_context);
    memset(&mark, 0, sizeof(mark));
    assert(cm_hir_context_mark(&rewind_context, &mark) == CM_HIR_OK);
    rewind_context.semantic_generation = UINT64_MAX - UINT64_C(1);
    rewind_context.rewind_generation = UINT64_MAX - UINT64_C(1);
    assert(cm_hir_context_rewind(&rewind_context, &mark) == CM_HIR_OK);
    assert(rewind_context.semantic_generation == UINT64_MAX
        && rewind_context.rewind_generation == UINT64_MAX);
    cm_hir_context_destroy(&rewind_context);

    /* Crossing UINT64_MAX deliberately aborts before mutation.  This test
     * binary is portable C99 and has no subprocess death-test harness, so it
     * exercises the final legal transitions without killing the test run. */
}

static void test_adt_generic_default_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId enum_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId first_parameter;
    CmHirGenericParamId second_parameter;
    CmHirGenericParamId rejected_parameter;
    CmHirGenericArg argument;
    CmHirType type;
    CmHirTypeId first_parameter_type;
    CmHirTypeId second_parameter_type;
    CmHirItem item;
    CmHirItemId item_id;
    const CmHirGenericParam *stored_first;
    const CmHirGenericParam *stored_second;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "adt_default_model"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_ENUM, test_span(10u, 90u), &enum_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = enum_definition;
    parameter.name = cm_hir_intern(&context, "B");
    parameter.span = test_span(20u, 21u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &first_parameter) == CM_HIR_OK);
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "C");
    parameter.span = test_span(22u, 23u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &second_parameter) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(20u, 21u);
    type.data.parameter_type.parameter = first_parameter;
    assert(cm_hir_add_type(&context, &type, &first_parameter_type)
        == CM_HIR_OK);
    type.span = test_span(22u, 23u);
    type.data.parameter_type.parameter = second_parameter;
    assert(cm_hir_add_type(&context, &type, &second_parameter_type)
        == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_set_generic_param_default(&context, second_parameter,
        &argument) == CM_HIR_INVALID_ARGUMENT);
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = second_parameter_type;
    assert(cm_hir_set_generic_param_default(&context, first_parameter,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_set_generic_param_default(&context, second_parameter,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    argument.data.type = first_parameter_type;
    assert(cm_hir_set_generic_param_default(&context, second_parameter,
        &argument) == CM_HIR_OK);
    parameter.index = 2u;
    parameter.name = cm_hir_intern(&context, "D");
    parameter.span = test_span(24u, 25u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &rejected_parameter) == CM_HIR_INVARIANT_VIOLATION);
    assert(rejected_parameter == CM_HIR_GENERIC_PARAM_NONE);
    init_test_item(&item, CM_HIR_ITEM_ENUM, enum_definition, root_module,
        cm_hir_def_id_none(), cm_hir_intern(&context, "ControlFlow"),
        test_span(10u, 90u));
    item.generic_parameter_start = first_parameter;
    item.generic_parameter_count = 2u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored_first = cm_hir_get_generic_param(&context, first_parameter);
    stored_second = cm_hir_get_generic_param(&context, second_parameter);
    assert(stored_first != NULL && !stored_first->has_default
        && cm_hir_def_id_equal(stored_first->owner, enum_definition)
        && stored_second != NULL && stored_second->has_default
        && cm_hir_def_id_equal(stored_second->owner, enum_definition)
        && stored_second->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && stored_second->default_argument.data.type
            == first_parameter_type);
    cm_hir_context_destroy(&context);
}

static void test_generic_default_resource_limits(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirGenericArg argument;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId deep_type;
    CmHirTypeId shared_type;
    CmHirTypeId elements[2];
    const CmHirGenericParam *stored;
    uint32_t index;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "default_resource_limits"),
        CM_HIR_EDITION_2024, test_span(0u, 1000u), &crate_id,
        &root_module) == CM_HIR_OK);
    (void)root_module;
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(1u, 999u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&context, &parameter, &parameter_id)
        == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(4u, 5u));

    deep_type = unit_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = test_span(6u, 7u);
    type.data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
    for (index = 0u; index < 300u; ++index) {
        type.data.raw_pointer_type.pointee = deep_type;
        assert(cm_hir_add_type(&context, &type, &deep_type) == CM_HIR_OK);
    }
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = deep_type;
    assert(cm_hir_set_generic_param_default(&context, parameter_id,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    stored = cm_hir_get_generic_param(&context, parameter_id);
    assert(stored != NULL && !stored->has_default
        && stored->default_argument.data.type == CM_HIR_TYPE_NONE);

    shared_type = unit_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(8u, 9u);
    type.data.tuple_type.elements = elements;
    type.data.tuple_type.element_count = 2u;
    for (index = 0u; index < 12u; ++index) {
        elements[0] = shared_type;
        elements[1] = shared_type;
        assert(cm_hir_add_type(&context, &type, &shared_type) == CM_HIR_OK);
    }
    argument.data.type = shared_type;
    assert(cm_hir_set_generic_param_default(&context, parameter_id,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    stored = cm_hir_get_generic_param(&context, parameter_id);
    assert(stored != NULL && !stored->has_default
        && stored->default_argument.data.type == CM_HIR_TYPE_NONE);

    argument.data.type = unit_type;
    assert(cm_hir_set_generic_param_default(&context, parameter_id,
        &argument) == CM_HIR_OK);
    stored = cm_hir_get_generic_param(&context, parameter_id);
    assert(stored != NULL && stored->has_default
        && stored->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && stored->default_argument.data.type == unit_type);
    cm_hir_context_destroy(&context);
}

static void test_generic_default_nominal_depth_boundary(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirGenericArg default_argument;
    CmHirGenericArg named_argument;
    CmHirType type;
    CmHirTypeId current;
    CmHirTypeId boundary;
    const CmHirGenericParam *stored;
    uint32_t index;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "default_nominal_depth"),
        CM_HIR_EDITION_2024, test_span(0u, 1000u), &crate_id,
        &root_module) == CM_HIR_OK);
    (void)root_module;
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_STRUCT, test_span(1u, 999u), &definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(2u, 3u);
    assert(cm_hir_add_generic_param(&context, &parameter, &parameter_id)
        == CM_HIR_OK);
    current = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(4u, 5u));
    memset(&named_argument, 0, sizeof(named_argument));
    named_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(6u, 7u);
    type.data.named_type.definition = definition;
    type.data.named_type.arguments = &named_argument;
    type.data.named_type.argument_count = 1u;
    boundary = CM_HIR_TYPE_NONE;
    for (index = 0u; index < 257u; ++index) {
        named_argument.data.type = current;
        assert(cm_hir_add_type(&context, &type, &current) == CM_HIR_OK);
        if (index == 255u) boundary = current;
    }
    assert(boundary != CM_HIR_TYPE_NONE);
    memset(&default_argument, 0, sizeof(default_argument));
    default_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    default_argument.data.type = current;
    assert(cm_hir_set_generic_param_default(&context, parameter_id,
        &default_argument) == CM_HIR_INVARIANT_VIOLATION);
    stored = cm_hir_get_generic_param(&context, parameter_id);
    assert(stored != NULL && !stored->has_default);
    default_argument.data.type = boundary;
    assert(cm_hir_set_generic_param_default(&context, parameter_id,
        &default_argument) == CM_HIR_OK);
    stored = cm_hir_get_generic_param(&context, parameter_id);
    assert(stored != NULL && stored->has_default
        && stored->default_argument.data.type == boundary);
    cm_hir_context_destroy(&context);
}

static void test_const_generic_default_type_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId mismatched_definition;
    CmHirDefId matching_definition;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId second_u8_type;
    CmHirTypeId usize_type;
    CmHirGenericParam parameter;
    CmHirGenericParamId source_parameter;
    CmHirGenericParamId target_parameter;
    CmHirGenericParamId matching_source;
    CmHirGenericParamId matching_target;
    CmHirGenericArg argument;
    const CmHirGenericParam *stored;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "const_default_type_model"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    (void)root_module;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 2u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    type.span = test_span(3u, 4u);
    assert(cm_hir_add_type(&context, &type, &second_u8_type) == CM_HIR_OK);
    type.span = test_span(5u, 6u);
    type.data.integer_type.kind = CM_HIR_INT_USIZE;
    assert(cm_hir_add_type(&context, &type, &usize_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 40u), &mismatched_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = mismatched_definition;
    parameter.name = cm_hir_intern(&context, "A");
    parameter.declared_type = u8_type;
    parameter.span = test_span(12u, 13u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &source_parameter) == CM_HIR_OK);
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "B");
    parameter.declared_type = usize_type;
    parameter.span = test_span(15u, 16u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &target_parameter) == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_CONST;
    argument.data.constant.kind = CM_HIR_CONST_PARAMETER;
    argument.data.constant.type = usize_type;
    argument.data.constant.data.parameter = source_parameter;
    assert(cm_hir_set_generic_param_default(&context, target_parameter,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    stored = cm_hir_get_generic_param(&context, target_parameter);
    assert(stored != NULL && !stored->has_default);
    argument.data.constant.kind = CM_HIR_CONST_VALUE;
    argument.data.constant.type = u8_type;
    argument.data.constant.data.value.low_bits = 1u;
    assert(cm_hir_set_generic_param_default(&context, target_parameter,
        &argument) == CM_HIR_INVARIANT_VIOLATION);
    stored = cm_hir_get_generic_param(&context, target_parameter);
    assert(stored != NULL && !stored->has_default);
    argument.data.constant.type = usize_type;
    assert(cm_hir_set_generic_param_default(&context, target_parameter,
        &argument) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(50u, 90u), &matching_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = matching_definition;
    parameter.name = cm_hir_intern(&context, "C");
    parameter.declared_type = u8_type;
    parameter.span = test_span(52u, 53u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &matching_source) == CM_HIR_OK);
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "D");
    parameter.declared_type = second_u8_type;
    parameter.span = test_span(55u, 56u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &matching_target) == CM_HIR_OK);
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_CONST;
    argument.data.constant.kind = CM_HIR_CONST_PARAMETER;
    argument.data.constant.type = second_u8_type;
    argument.data.constant.data.parameter = matching_source;
    assert(cm_hir_set_generic_param_default(&context, matching_target,
        &argument) == CM_HIR_OK);
    stored = cm_hir_get_generic_param(&context, matching_target);
    assert(stored != NULL && stored->has_default
        && stored->default_argument.kind == CM_HIR_GENERIC_ARG_CONST
        && stored->default_argument.data.constant.kind
            == CM_HIR_CONST_PARAMETER
        && stored->default_argument.data.constant.data.parameter
            == matching_source);
    cm_hir_context_destroy(&context);
}

static void test_const_generic_trait_method_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId trait_definition;
    CmHirDefId method_definition;
    CmHirDefId rejected_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirGenericParamId rejected_parameter_id;
    CmHirGenericArg default_argument;
    CmHirType type;
    CmHirTypeId usize_type;
    CmHirTypeId unit_type;
    CmHirTypeId self_type;
    CmHirTypeId array_type;
    CmHirTypeId reference_type;
    CmHirFunctionParameter function_parameter;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirItemId method_item_id;
    const CmHirItem *stored_method;
    const CmHirGenericParam *stored_parameter;
    const CmHirType *stored_array;
    const CmHirDefinition *stored_definition;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "const_generic_trait_method"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &crate_id,
        &root_module) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 6u);
    type.data.integer_type.kind = CM_HIR_INT_USIZE;
    assert(cm_hir_add_type(&context, &type, &usize_type) == CM_HIR_OK);
    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(7u, 9u));
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 90u), &trait_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, trait_definition,
        root_module, cm_hir_def_id_none(),
        cm_hir_intern(&context, "SpecArrayClone"), test_span(10u, 90u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = test_span(40u, 44u);
    type.data.self_type.owner = trait_definition;
    assert(cm_hir_add_type(&context, &type, &self_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(20u, 70u), &method_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_CONST;
    parameter.owner = method_definition;
    parameter.name = cm_hir_intern(&context, "N");
    parameter.declared_type = usize_type;
    parameter.span = test_span(29u, 43u);
    assert(cm_hir_add_generic_param(&context, &parameter, &parameter_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(45u, 54u);
    type.data.array_type.element = self_type;
    type.data.array_type.length.kind = CM_HIR_CONST_PARAMETER;
    type.data.array_type.length.type = usize_type;
    type.data.array_type.length.data.parameter = parameter_id;
    assert(cm_hir_add_type(&context, &type, &array_type) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(44u, 54u);
    type.data.reference_type.region.kind = CM_HIR_REGION_ERASED;
    type.data.reference_type.pointee = array_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &reference_type) == CM_HIR_OK);
    memset(&function_parameter, 0, sizeof(function_parameter));
    function_parameter.name = cm_hir_intern(&context, "array");
    function_parameter.type = reference_type;
    function_parameter.span = test_span(37u, 54u);
    function_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, method_definition,
        root_module, trait_definition, cm_hir_intern(&context, "clone"),
        test_span(20u, 70u));
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.function_item.signature.parameters = &function_parameter;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type = array_type;
    item.data.function_item.signature.abi = cm_hir_intern(&context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &method_item_id) == CM_HIR_OK);
    stored_method = cm_hir_get_item(&context, method_item_id);
    stored_parameter = cm_hir_get_generic_param(&context, parameter_id);
    stored_array = cm_hir_get_type(&context, array_type);
    assert(stored_method != NULL
        && stored_method->generic_parameter_start == parameter_id
        && stored_method->generic_parameter_count == 1u
        && stored_method->data.function_item.signature.parameters
            != &function_parameter
        && stored_parameter != NULL
        && stored_parameter->kind == CM_HIR_GENERIC_CONST
        && stored_parameter->declared_type == usize_type
        && !stored_parameter->has_default
        && stored_array != NULL
        && stored_array->data.array_type.length.kind
            == CM_HIR_CONST_PARAMETER
        && stored_array->data.array_type.length.type == usize_type
        && stored_array->data.array_type.length.data.parameter
            == parameter_id);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(71u, 89u), &rejected_definition)
        == CM_HIR_OK);
    parameter.owner = rejected_definition;
    parameter.name = cm_hir_intern(&context, "M");
    parameter.span = test_span(72u, 80u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &rejected_parameter_id) == CM_HIR_OK);
    memset(&default_argument, 0, sizeof(default_argument));
    default_argument.kind = CM_HIR_GENERIC_ARG_CONST;
    default_argument.data.constant.kind = CM_HIR_CONST_VALUE;
    default_argument.data.constant.type = usize_type;
    default_argument.data.constant.data.value.low_bits = 1u;
    assert(cm_hir_set_generic_param_default(&context,
        rejected_parameter_id, &default_argument) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_FUNCTION, rejected_definition,
        root_module, trait_definition, cm_hir_intern(&context, "bad"),
        test_span(71u, 89u));
    item.generic_parameter_start = rejected_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = cm_hir_intern(&context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(item_id == CM_HIR_ITEM_NONE);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);
    cm_hir_context_destroy(&context);
}

static void test_auto_trait_and_negative_impl_model(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId ordinary_definition;
    CmHirDefId auto_definition;
    CmHirDefId associated_definition;
    CmHirDefId negative_definition;
    CmHirDefId ordinary_negative_definition;
    CmHirDefId positive_definition;
    CmHirDefId reservation_definition;
    CmHirDefId child_definition;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId u16_type;
    CmHirTypeId dyn_type;
    CmHirTypeId rejected_type;
    CmHirNamedType markers[2];
    CmHirAssociatedTypeEquality equality;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirSupertrait supertrait;
    const CmHirItem *stored;
    const CmHirType *stored_dyn;
    size_t arena_bytes;
    size_t type_count;
    FILE *dump_file;
    char *dump;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "auto_trait_model"), CM_HIR_EDITION_2024,
        test_span(0u, 200u), &crate_id, &root_module) == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(1u, 3u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    type.span = test_span(4u, 7u);
    type.data.integer_type.kind = CM_HIR_INT_U16;
    assert(cm_hir_add_type(&context, &type, &u16_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 30u), &ordinary_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, ordinary_definition,
        root_module, cm_hir_def_id_none(),
        cm_hir_intern(&context, "Ordinary"), test_span(10u, 30u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.is_const = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_item.is_const = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(31u, 60u), &auto_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, auto_definition, root_module,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Auto"),
        test_span(31u, 60u));
    item.data.trait_item.safety = CM_HIR_UNSAFE;
    item.data.trait_item.is_auto = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_item.is_auto = 1;
    item.data.trait_item.is_const = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_item.is_const = 1;
    memset(&supertrait, 0, sizeof(supertrait));
    supertrait.trait_type.definition = ordinary_definition;
    supertrait.span = test_span(40u, 48u);
    item.data.trait_item.supertraits = &supertrait;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_item.supertraits = NULL;
    item.data.trait_item.supertrait_count = 0u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL && stored->kind == CM_HIR_ITEM_TRAIT
        && stored->data.trait_item.is_auto
        && stored->data.trait_item.is_const
        && stored->data.trait_item.safety == CM_HIR_UNSAFE);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(20u, 25u),
        &associated_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, associated_definition,
        root_module, ordinary_definition, cm_hir_intern(&context, "Item"),
        test_span(20u, 25u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(markers, 0, sizeof(markers));
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = associated_definition;
    equality.value = u8_type;
    equality.span = test_span(40u, 50u);
    markers[0].definition = auto_definition;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_DYN_TRAIT_KIND;
    type.span = test_span(31u, 60u);
    type.data.dyn_trait_type.has_principal = 1;
    type.data.dyn_trait_type.principal_trait.definition = ordinary_definition;
    type.data.dyn_trait_type.equalities = &equality;
    type.data.dyn_trait_type.equality_count = 1u;
    type.data.dyn_trait_type.auto_traits = markers;
    type.data.dyn_trait_type.auto_trait_count = 1u;
    type.data.dyn_trait_type.region.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_add_type(&context, &type, &dyn_type) == CM_HIR_OK);
    markers[0].definition = ordinary_definition;
    equality.value = u16_type;
    stored_dyn = cm_hir_get_type(&context, dyn_type);
    assert(stored_dyn != NULL
        && stored_dyn->data.dyn_trait_type.auto_traits != markers
        && stored_dyn->data.dyn_trait_type.equalities != &equality
        && stored_dyn->data.dyn_trait_type.equality_count == 1u
        && cm_hir_def_id_equal(stored_dyn->data.dyn_trait_type
                .equalities[0].associated_type,
            associated_definition)
        && stored_dyn->data.dyn_trait_type.equalities[0].value == u8_type
        && cm_hir_def_id_equal(
            stored_dyn->data.dyn_trait_type.auto_traits[0].definition,
            auto_definition));

    type_count = context.types.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    rejected_type = CM_HIR_TYPE_NONE;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_DYN_TRAIT_KIND;
    type.span = test_span(31u, 60u);
    type.data.dyn_trait_type.region.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    type.data.dyn_trait_type.has_principal = 1;
    type.data.dyn_trait_type.principal_trait.definition = auto_definition;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    type.data.dyn_trait_type.has_principal = 0;
    type.data.dyn_trait_type.principal_trait.definition =
        cm_hir_def_id_none();
    type.data.dyn_trait_type.equalities = &equality;
    type.data.dyn_trait_type.equality_count = 1u;
    markers[0].definition = ordinary_definition;
    type.data.dyn_trait_type.auto_traits = markers;
    type.data.dyn_trait_type.auto_trait_count = 1u;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    markers[0].definition = auto_definition;
    markers[1].definition = auto_definition;
    type.data.dyn_trait_type.auto_trait_count = 2u;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    type.data.dyn_trait_type.auto_trait_count = 0u;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    type.data.dyn_trait_type.equalities = NULL;
    type.data.dyn_trait_type.equality_count = 1u;
    assert(cm_hir_add_type(&context, &type, &rejected_type)
        == CM_HIR_INVALID_ID);
    type.data.dyn_trait_type.equality_count = 0u;
    assert(rejected_type == CM_HIR_TYPE_NONE
        && context.types.len == type_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);

    type.data.dyn_trait_type.auto_traits = markers;
    type.data.dyn_trait_type.auto_trait_count = 1u;
    assert(cm_hir_add_type(&context, &type, &dyn_type) == CM_HIR_OK);
    stored_dyn = cm_hir_get_type(&context, dyn_type);
    assert(stored_dyn != NULL
        && !stored_dyn->data.dyn_trait_type.has_principal
        && stored_dyn->data.dyn_trait_type.auto_trait_count == 1u
        && cm_hir_def_id_equal(
            stored_dyn->data.dyn_trait_type.auto_traits[0].definition,
            auto_definition));

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(45u, 50u), &child_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, child_definition,
        root_module, auto_definition, cm_hir_intern(&context, "Child"),
        test_span(45u, 50u));
    item.data.type_alias_item.target = u8_type;
    assert(cm_hir_add_item(&context, &item, &item_id)
        == CM_HIR_INVARIANT_VIOLATION);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(61u, 100u), &negative_definition)
        == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(101u, 140u),
        &ordinary_negative_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, negative_definition,
        root_module, cm_hir_def_id_none(), CM_INTERN_ID_NONE,
        test_span(61u, 100u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.polarity = (CmHirImplPolarity)3;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.trait_type.definition = auto_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.polarity = CM_HIR_IMPL_NEGATIVE;
    item.data.impl_item.is_const = 2;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.is_const = 0;
    item.data.impl_item.has_trait = 0;
    item.data.impl_item.trait_type.definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = ordinary_definition;
    item.definition = ordinary_negative_definition;
    item.data.impl_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.is_const = 0;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item.definition = negative_definition;
    item.data.impl_item.trait_type.definition = auto_definition;
    item.data.impl_item.is_const = 1;
    item.data.impl_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL && stored->kind == CM_HIR_ITEM_IMPL
        && stored->data.impl_item.polarity == CM_HIR_IMPL_NEGATIVE
        && stored->data.impl_item.is_const
        && stored->data.impl_item.safety == CM_HIR_SAFE
        && cm_hir_def_id_equal(
            stored->data.impl_item.trait_type.definition,
            auto_definition));

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(101u, 140u), &positive_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, positive_definition,
        root_module, cm_hir_def_id_none(), CM_INTERN_ID_NONE,
        test_span(101u, 140u));
    item.data.impl_item.self_type = u16_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = auto_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.is_const = 1;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.safety = CM_HIR_UNSAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_IMPL, test_span(141u, 175u),
        &reservation_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_IMPL, reservation_definition,
        root_module, cm_hir_def_id_none(), CM_INTERN_ID_NONE,
        test_span(141u, 175u));
    item.data.impl_item.self_type = u8_type;
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = ordinary_definition;
    item.data.impl_item.safety = CM_HIR_SAFE;
    item.data.impl_item.polarity = (CmHirImplPolarity)3;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.polarity = CM_HIR_IMPL_RESERVATION;
    item.data.impl_item.has_trait = 0;
    item.data.impl_item.trait_type.definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.impl_item.has_trait = 1;
    item.data.impl_item.trait_type.definition = ordinary_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    stored = cm_hir_get_item(&context, item_id);
    assert(stored != NULL
        && stored->data.impl_item.polarity == CM_HIR_IMPL_RESERVATION);

    dump_file = tmpfile();
    assert(dump_file != NULL && cm_hir_dump(dump_file, &context) == 0);
    dump = read_dump(dump_file);
    assert(strncmp(dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(strstr(dump,
        "trait-header item#2 safety=unsafe auto=1 const=1")
        != NULL);
    assert(strstr(dump, "{assoc=1:4,value=ty#1}") != NULL);
    assert(strstr(dump,
        "impl-header item#4 safety=safe polarity=negative const=0")
        != NULL);
    assert(strstr(dump,
        "impl-header item#5 safety=safe polarity=negative const=1")
        != NULL);
    assert(strstr(dump,
        "impl-header item#6 safety=unsafe polarity=positive const=1")
        != NULL);
    assert(strstr(dump,
        "impl-header item#7 safety=safe polarity=reservation const=0")
        != NULL);
    free(dump);
    assert(fclose(dump_file) == 0);
    cm_hir_context_destroy(&context);
}

static void test_trait_alias_model_invariants(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirTypeId u8_type;
    CmHirDefId pointee_definition;
    CmHirDefId metadata_definition;
    CmHirDefId const_definition;
    CmHirDefId base_alias_definition;
    CmHirDefId alias_definition;
    CmHirDefId rejected_definition;
    CmHirDefId forward_definition;
    CmHirDefId predicate_owner_definition;
    CmHirDefId supertrait_owner_definition;
    CmHirDefId cycle_a_definition;
    CmHirDefId cycle_b_definition;
    CmHirDefId mixed_trait_definition;
    CmHirDefId mixed_alias_definition;
    CmHirGenericParam lifetime_parameter;
    CmHirGenericParam type_parameter;
    CmHirGenericParamId lifetime_parameter_id;
    CmHirGenericParamId type_parameter_id;
    CmHirGenericArg default_argument;
    CmHirAssociatedTypeEquality equality;
    CmHirTraitAliasBound base_bound;
    CmHirTraitAliasBound bounds[4];
    CmHirTraitAliasBound cycle_bound;
    CmHirSupertrait ordinary_supertrait;
    CmHirTraitPredicate predicate;
    CmHirGenericArg predicate_arguments[2];
    CmHirType self_type;
    CmHirTypeId self_type_id;
    CmHirItem item;
    CmHirItemId alias_item_id;
    CmHirItemId item_id;
    const CmHirItem *stored;
    const CmHirDefinition *stored_definition;
    size_t item_count;
    size_t arena_bytes;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    const char *first_bound;
    const char *lifetime_bound;
    const char *const_bound;
    const char *alias_bound;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context,
        cm_hir_intern(&context, "trait_alias_model"),
        CM_HIR_EDITION_2024, test_span(0u, 400u), &crate_id, &root_id)
        == CM_HIR_OK);
    u8_type = add_simple_type(&context, CM_HIR_TYPE_INTEGER_KIND,
        test_span(1u, 2u));

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(10u, 40u), &pointee_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, pointee_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "Pointee"),
        test_span(10u, 40u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TYPE_ALIAS, test_span(20u, 30u),
        &metadata_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TYPE_ALIAS, metadata_definition,
        root_id, pointee_definition, cm_hir_intern(&context, "Metadata"),
        test_span(20u, 30u));
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(41u, 60u), &const_definition)
        == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, const_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "ConstTrait"),
        test_span(41u, 60u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&base_bound, 0, sizeof(base_bound));
    base_bound.kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    base_bound.span = test_span(71u, 80u);
    base_bound.data.trait_bound.trait_type.definition =
        pointee_definition;
    base_bound.data.trait_bound.span = base_bound.span;
    base_bound.data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(70u, 90u),
        &base_alias_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS,
        base_alias_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "BaseAlias"), test_span(70u, 90u));
    item.data.trait_alias_item.bounds = &base_bound;
    item.data.trait_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(100u, 200u),
        &alias_definition) == CM_HIR_OK);
    memset(&self_type, 0, sizeof(self_type));
    self_type.kind = CM_HIR_TYPE_SELF_KIND;
    self_type.span = test_span(101u, 102u);
    self_type.data.self_type.owner = alias_definition;
    assert(cm_hir_add_type(&context, &self_type, &self_type_id)
        == CM_HIR_OK);
    memset(&lifetime_parameter, 0, sizeof(lifetime_parameter));
    lifetime_parameter.kind = CM_HIR_GENERIC_LIFETIME;
    lifetime_parameter.owner = alias_definition;
    lifetime_parameter.name = cm_hir_intern(&context, "'a");
    lifetime_parameter.span = test_span(101u, 103u);
    assert(cm_hir_add_generic_param(&context, &lifetime_parameter,
        &lifetime_parameter_id) == CM_HIR_OK);
    memset(&type_parameter, 0, sizeof(type_parameter));
    type_parameter.kind = CM_HIR_GENERIC_TYPE;
    type_parameter.owner = alias_definition;
    type_parameter.index = 1u;
    type_parameter.name = cm_hir_intern(&context, "T");
    type_parameter.span = test_span(104u, 105u);
    assert(cm_hir_add_generic_param(&context, &type_parameter,
        &type_parameter_id) == CM_HIR_OK);
    memset(&default_argument, 0, sizeof(default_argument));
    default_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    default_argument.data.type = self_type_id;
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &default_argument) == CM_HIR_OK);

    memset(&equality, 0, sizeof(equality));
    equality.associated_type = metadata_definition;
    equality.value = self_type_id;
    equality.span = test_span(120u, 130u);
    memset(bounds, 0, sizeof(bounds));
    bounds[0].kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    bounds[0].span = test_span(110u, 131u);
    bounds[0].data.trait_bound.trait_type.definition = pointee_definition;
    bounds[0].data.trait_bound.equalities = &equality;
    bounds[0].data.trait_bound.equality_count = 1u;
    bounds[0].data.trait_bound.span = bounds[0].span;
    bounds[0].data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    bounds[1].kind = CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME;
    bounds[1].span = test_span(132u, 134u);
    bounds[1].data.lifetime.kind = CM_HIR_REGION_EARLY_BOUND;
    bounds[1].data.lifetime.data.parameter = lifetime_parameter_id;
    bounds[2].kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    bounds[2].span = test_span(135u, 150u);
    bounds[2].data.trait_bound.trait_type.definition = const_definition;
    bounds[2].data.trait_bound.span = bounds[2].span;
    bounds[2].data.trait_bound.modifier =
        CM_HIR_SUPERTRAIT_CONST_IF_CONST;
    bounds[3].kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    bounds[3].span = test_span(151u, 165u);
    bounds[3].data.trait_bound.trait_type.definition =
        base_alias_definition;
    bounds[3].data.trait_bound.span = bounds[3].span;
    bounds[3].data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS, alias_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Alias"),
        test_span(100u, 200u));
    item.generic_parameter_start = lifetime_parameter_id;
    item.generic_parameter_count = 2u;
    item.data.trait_alias_item.bounds = bounds;
    item.data.trait_alias_item.bound_count = 4u;
    assert(cm_hir_add_item(&context, &item, &alias_item_id) == CM_HIR_OK);

    equality.value = CM_HIR_TYPE_NONE;
    bounds[0].data.trait_bound.trait_type.definition = const_definition;
    bounds[1].data.lifetime.kind = CM_HIR_REGION_STATIC;
    bounds[2].data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    bounds[3].data.trait_bound.trait_type.definition = pointee_definition;
    stored = cm_hir_get_item(&context, alias_item_id);
    assert(stored != NULL && stored->kind == CM_HIR_ITEM_TRAIT_ALIAS
        && stored->data.trait_alias_item.bounds != bounds
        && stored->data.trait_alias_item.bound_count == 4u
        && stored->data.trait_alias_item.bounds[0].data.trait_bound
            .equalities != &equality
        && stored->data.trait_alias_item.bounds[0].data.trait_bound
            .equalities[0].value == self_type_id
        && cm_hir_def_id_equal(stored->data.trait_alias_item.bounds[0]
                .data.trait_bound.trait_type.definition,
            pointee_definition)
        && stored->data.trait_alias_item.bounds[1].data.lifetime.kind
            == CM_HIR_REGION_EARLY_BOUND
        && stored->data.trait_alias_item.bounds[2].data.trait_bound
            .modifier == CM_HIR_SUPERTRAIT_CONST_IF_CONST
        && cm_hir_def_id_equal(stored->data.trait_alias_item.bounds[3]
                .data.trait_bound.trait_type.definition,
            base_alias_definition));

    memset(&predicate, 0, sizeof(predicate));
    memset(predicate_arguments, 0, sizeof(predicate_arguments));
    predicate_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    predicate_arguments[0].data.lifetime.kind = CM_HIR_REGION_STATIC;
    predicate_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    predicate_arguments[1].data.type = u8_type;
    predicate.subject = u8_type;
    predicate.trait_type.definition = alias_definition;
    predicate.trait_type.arguments = predicate_arguments;
    predicate.trait_type.argument_count = 2u;
    predicate.modifier = CM_HIR_PREDICATE_REQUIRED;
    predicate.span = test_span(202u, 208u);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(201u, 209u), &predicate_owner_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, predicate_owner_definition,
        root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "AliasPredicateOwner"),
        test_span(201u, 209u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    item.predicates = &predicate;
    item.predicate_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    memset(&ordinary_supertrait, 0, sizeof(ordinary_supertrait));
    ordinary_supertrait.trait_type.definition = base_alias_definition;
    ordinary_supertrait.span = test_span(211u, 219u);
    ordinary_supertrait.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(210u, 220u), &supertrait_owner_definition) == CM_HIR_OK);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, supertrait_owner_definition,
        root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "NotWeakened"),
        test_span(210u, 220u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &ordinary_supertrait;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(230u, 260u), &forward_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(221u, 230u), &rejected_definition) == CM_HIR_OK);
    base_bound.span = test_span(222u, 229u);
    base_bound.data.trait_bound.span = base_bound.span;
    base_bound.data.trait_bound.trait_type.definition = forward_definition;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS, rejected_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Forward"),
        test_span(221u, 230u));
    item.data.trait_alias_item.bounds = &base_bound;
    item.data.trait_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    init_test_item(&item, CM_HIR_ITEM_STRUCT, forward_definition, root_id,
        cm_hir_def_id_none(), cm_hir_intern(&context, "WrongTarget"),
        test_span(230u, 260u));
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_UNIT;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        forward_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(270u, 310u),
        &rejected_definition) == CM_HIR_OK);
    base_bound.span = test_span(280u, 290u);
    base_bound.data.trait_bound.span = base_bound.span;
    base_bound.data.trait_bound.trait_type.definition = pointee_definition;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS, rejected_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "Rejected"),
        test_span(270u, 310u));
    item.data.trait_alias_item.bounds = &base_bound;
    item.data.trait_alias_item.bound_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);

    item.data.trait_alias_item.bounds = NULL;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    item.data.trait_alias_item.bounds = &base_bound;
    base_bound.kind = (CmHirTraitAliasBoundKind)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    base_bound.kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    base_bound.span = test_span(280u, 320u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    base_bound.span = test_span(280u, 290u);
    base_bound.data.trait_bound.span = test_span(281u, 290u);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    base_bound.data.trait_bound.span = base_bound.span;
    base_bound.data.trait_bound.modifier = (CmHirSupertraitModifier)99;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    base_bound.data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    base_bound.data.trait_bound.trait_type.definition = rejected_definition;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    base_bound.kind = CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME;
    base_bound.data.lifetime.kind = CM_HIR_REGION_EARLY_BOUND;
    base_bound.data.lifetime.data.parameter = lifetime_parameter_id;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        rejected_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(320u, 340u),
        &cycle_a_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(341u, 360u),
        &cycle_b_definition) == CM_HIR_OK);
    memset(&cycle_bound, 0, sizeof(cycle_bound));
    cycle_bound.kind = CM_HIR_TRAIT_ALIAS_BOUND_TRAIT;
    cycle_bound.span = test_span(325u, 335u);
    cycle_bound.data.trait_bound.trait_type.definition =
        cycle_b_definition;
    cycle_bound.data.trait_bound.span = cycle_bound.span;
    cycle_bound.data.trait_bound.modifier = CM_HIR_SUPERTRAIT_REQUIRED;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS, cycle_a_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "CycleA"),
        test_span(320u, 340u));
    item.data.trait_alias_item.bounds = &cycle_bound;
    item.data.trait_alias_item.bound_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    cycle_bound.span = test_span(345u, 355u);
    cycle_bound.data.trait_bound.span = cycle_bound.span;
    cycle_bound.data.trait_bound.trait_type.definition =
        cycle_a_definition;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS, cycle_b_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "CycleB"),
        test_span(341u, 360u));
    item.data.trait_alias_item.bounds = &cycle_bound;
    item.data.trait_alias_item.bound_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        cycle_b_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT, test_span(361u, 380u),
        &mixed_trait_definition) == CM_HIR_OK);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_TRAIT_ALIAS, test_span(381u, 399u),
        &mixed_alias_definition) == CM_HIR_OK);
    ordinary_supertrait.trait_type.definition = mixed_alias_definition;
    ordinary_supertrait.span = test_span(365u, 375u);
    init_test_item(&item, CM_HIR_ITEM_TRAIT, mixed_trait_definition,
        root_id, cm_hir_def_id_none(), cm_hir_intern(&context, "MixedTrait"),
        test_span(361u, 380u));
    item.data.trait_item.safety = CM_HIR_SAFE;
    item.data.trait_item.supertraits = &ordinary_supertrait;
    item.data.trait_item.supertrait_count = 1u;
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_OK);
    cycle_bound.span = test_span(385u, 395u);
    cycle_bound.data.trait_bound.span = cycle_bound.span;
    cycle_bound.data.trait_bound.trait_type.definition =
        mixed_trait_definition;
    init_test_item(&item, CM_HIR_ITEM_TRAIT_ALIAS,
        mixed_alias_definition, root_id, cm_hir_def_id_none(),
        cm_hir_intern(&context, "MixedAlias"), test_span(381u, 399u));
    item.data.trait_alias_item.bounds = &cycle_bound;
    item.data.trait_alias_item.bound_count = 1u;
    item_count = context.items.len;
    arena_bytes = cm_arena_bytes_used(&context.storage);
    assert(cm_hir_add_item(&context, &item, &item_id) == CM_HIR_INVALID_ID);
    assert(item_id == CM_HIR_ITEM_NONE && context.items.len == item_count
        && cm_arena_bytes_used(&context.storage) == arena_bytes);
    stored_definition = cm_hir_lookup_definition(&context,
        mixed_alias_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL && second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(strstr(first_dump,
        "generic#2 owner=1:6 index=1 kind=1 name=\"T\" "
        "declared=ty#0 relaxed-sized=0 default=ty#2") != NULL);
    first_bound = strstr(first_dump,
        "trait-alias-bound item#5 index=0 kind=trait modifier=required "
        "trait=1:2 equalities=1 span=1:110..131\n");
    lifetime_bound = strstr(first_dump,
        "trait-alias-bound item#5 index=1 kind=lifetime "
        "lifetime=early($1) span=1:132..134\n");
    const_bound = strstr(first_dump,
        "trait-alias-bound item#5 index=2 kind=trait "
        "modifier=const-if-const trait=1:4 equalities=0 "
        "span=1:135..150\n");
    alias_bound = strstr(first_dump,
        "trait-alias-bound item#5 index=3 kind=trait modifier=required "
        "trait=1:5 equalities=0 span=1:151..165\n");
    assert(first_bound != NULL && lifetime_bound != NULL
        && const_bound != NULL && alias_bound != NULL
        && first_bound < lifetime_bound && lifetime_bound < const_bound
        && const_bound < alias_bound);
    assert(strstr(first_dump,
        "trait-alias-associated-type-equality item#5 bound=0 index=0 "
        "associated=1:3 value=ty#2 span=1:120..130\n") != NULL);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);
    cm_hir_context_destroy(&context);
}

int main(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_id;
    CmHirModuleId child_id;
    CmHirDefId node_definition;
    CmHirDefId alias_definition;
    CmHirDefId malformed_signature_definition;
    CmHirDefId function_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId type_parameter_id;
    CmHirGenericParamId duplicate_parameter_id;
    CmHirGenericParamId alias_parameter_id;
    CmHirGenericParamId malformed_default_id;
    CmHirGenericParamId malformed_required_id;
    CmHirGenericArg generic_default;
    CmHirType type;
    CmHirTypeId unit_type;
    CmHirTypeId u8_type;
    CmHirTypeId usize_type;
    CmHirTypeId parameter_type;
    CmHirTypeId alias_self_type;
    CmHirTypeId node_type;
    CmHirTypeId node_ref_type;
    CmHirTypeId array_type;
    CmHirTypeId tuple_type;
    CmHirTypeId raw_pointer_type;
    CmHirTypeId slice_type;
    CmHirTypeId fn_pointer_type;
    CmHirTypeId inference_type;
    CmHirTypeId alias_application_type;
    CmHirTypeId bad_type_id;
    CmHirGenericArg node_arguments[1];
    CmHirGenericArg alias_arguments[1];
    CmHirTypeId tuple_elements[3];
    CmHirTypeId fn_parameters[2];
    CmHirLocal locals[1];
    CmHirBody body;
    CmHirBodyId body_id;
    CmHirField fields[2];
    CmHirItem item;
    CmHirItemId node_item_id;
    CmHirItemId alias_item_id;
    CmHirItemId malformed_item_id;
    CmHirItemId function_item_id;
    CmHirFunctionParameter function_parameters[1];
    const CmHirType *stored_tuple;
    const CmHirItem *stored_node;
    const CmHirItem *stored_alias;
    const CmHirType *stored_alias_application;
    const CmHirDefinition *stored_definition;
    FILE *first_file;
    FILE *second_file;
    char *first_dump;
    char *second_dump;
    CmInternId rust_abi;
    CmInternId empty_name;
    CmInternId reason;
    CmHirAttribute crate_attributes[1];
    CmHirAttribute module_outer_attributes[1];
    CmHirAttribute module_attributes[1];
    const CmHirCrate *stored_crate;
    const CmHirModule *stored_module;

    test_known_trait_projection_model();
    cm_hir_context_init(&context);
    empty_name = cm_hir_intern(&context, "schema_test");
    rust_abi = cm_hir_intern(&context, "Rust");
    reason = cm_hir_intern(&context, "type mismatch");
    assert(empty_name != CM_INTERN_ID_NONE);
    assert(cm_hir_create_crate(&context, empty_name, CM_HIR_EDITION_2024,
        test_span(0u, 200u), &crate_id, &root_id) == CM_HIR_OK);
    assert(crate_id == 1u);
    assert(root_id == 1u);
    assert(cm_hir_add_module(&context, crate_id, root_id,
        cm_hir_intern(&context, "inner"), test_span(10u, 190u),
        &child_id) == CM_HIR_OK);
    memset(crate_attributes, 0, sizeof(crate_attributes));
    crate_attributes[0].metadata = cm_hir_intern(&context, "no_core");
    crate_attributes[0].span = test_span(0u, 11u);
    crate_attributes[0].source_attribute = 1u;
    crate_attributes[0].expansion_depth = 1u;
    assert(cm_hir_set_crate_inner_attributes(&context, crate_id,
        crate_attributes, 1u) == CM_HIR_OK);
    memset(module_outer_attributes, 0, sizeof(module_outer_attributes));
    module_outer_attributes[0].metadata = cm_hir_intern(&context,
        "stable(feature = \"module_attrs\", since = \"1.0.0\")");
    module_outer_attributes[0].span = test_span(5u, 9u);
    module_outer_attributes[0].source_attribute = 3u;
    assert(cm_hir_set_module_outer_attributes(&context, child_id,
        module_outer_attributes, 1u) == CM_HIR_OK);
    memset(module_attributes, 0, sizeof(module_attributes));
    module_attributes[0].metadata = cm_hir_intern(&context,
        "allow(dead_code)");
    module_attributes[0].span = test_span(12u, 31u);
    module_attributes[0].source_attribute = 2u;
    assert(cm_hir_set_module_inner_attributes(&context, child_id,
        module_attributes, 1u) == CM_HIR_OK);
    crate_attributes[0].source_attribute = 99u;
    module_outer_attributes[0].source_attribute = 99u;
    module_attributes[0].source_attribute = 99u;
    stored_crate = cm_hir_get_crate(&context, crate_id);
    stored_module = cm_hir_get_module(&context, child_id);
    assert(stored_crate != NULL && stored_crate->inner_attribute_count == 1u
        && stored_crate->inner_attributes[0].source_attribute == 1u);
    assert(stored_module != NULL
        && stored_module->outer_attribute_count == 1u
        && stored_module->outer_attributes[0].source_attribute == 3u
        && stored_module->inner_attribute_count == 1u
        && stored_module->inner_attributes[0].source_attribute == 2u);
    assert(cm_hir_set_crate_inner_attributes(&context, crate_id,
        crate_attributes, 1u) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_set_module_inner_attributes(&context, child_id,
        NULL, 1u) == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_set_module_outer_attributes(&context, child_id,
        module_outer_attributes, 1u) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_set_module_outer_attributes(&context, root_id,
        module_outer_attributes, 1u) == CM_HIR_INVALID_ARGUMENT);

    unit_type = add_simple_type(&context, CM_HIR_TYPE_UNIT_KIND,
        test_span(1u, 3u));
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = test_span(4u, 6u);
    type.data.integer_type.kind = CM_HIR_INT_U8;
    assert(cm_hir_add_type(&context, &type, &u8_type) == CM_HIR_OK);
    type.data.integer_type.kind = CM_HIR_INT_USIZE;
    assert(cm_hir_add_type(&context, &type, &usize_type) == CM_HIR_OK);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(20u, 100u), &node_definition) == CM_HIR_OK);
    stored_definition = cm_hir_lookup_definition(&context, node_definition);
    assert(stored_definition != NULL);
    assert(stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = node_definition;
    parameter.name = cm_hir_intern(&context, "T");
    parameter.span = test_span(25u, 26u);
    parameter.has_default = 1;
    parameter.default_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    parameter.default_argument.data.type = u8_type;
    assert(cm_hir_add_generic_param(&context, &parameter,
        &duplicate_parameter_id) == CM_HIR_INVALID_ARGUMENT);
    parameter.has_default = 0;
    memset(&parameter.default_argument, 0,
        sizeof(parameter.default_argument));
    assert(cm_hir_add_generic_param(&context, &parameter,
        &type_parameter_id) == CM_HIR_OK);
    assert(type_parameter_id == 1u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &duplicate_parameter_id) == CM_HIR_INVARIANT_VIOLATION);
    assert(duplicate_parameter_id == CM_HIR_GENERIC_PARAM_NONE);

    memset(&generic_default, 0, sizeof(generic_default));
    generic_default.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    generic_default.data.lifetime.kind = CM_HIR_REGION_STATIC;
    assert(cm_hir_set_generic_param_default(NULL, type_parameter_id,
        &generic_default) == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        NULL) == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &generic_default) == CM_HIR_INVALID_ARGUMENT);
    assert(!cm_hir_get_generic_param(&context,
        type_parameter_id)->has_default);
    generic_default.kind = (CmHirGenericArgKind)99;
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &generic_default) == CM_HIR_INVALID_ARGUMENT);
    assert(!cm_hir_get_generic_param(&context,
        type_parameter_id)->has_default);
    generic_default.kind = CM_HIR_GENERIC_ARG_TYPE;
    generic_default.data.type = (CmHirTypeId)9999u;
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &generic_default) == CM_HIR_INVALID_ID);
    assert(!cm_hir_get_generic_param(&context,
        type_parameter_id)->has_default);
    generic_default.data.type = u8_type;
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &generic_default) == CM_HIR_OK);
    assert(cm_hir_get_generic_param(&context, type_parameter_id)->has_default
        && cm_hir_get_generic_param(&context,
            type_parameter_id)->default_argument.kind
            == CM_HIR_GENERIC_ARG_TYPE
        && cm_hir_get_generic_param(&context,
            type_parameter_id)->default_argument.data.type == u8_type);
    generic_default.data.type = usize_type;
    assert(cm_hir_set_generic_param_default(&context, type_parameter_id,
        &generic_default) == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_get_generic_param(&context,
        type_parameter_id)->default_argument.data.type == u8_type);
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "U");
    assert(cm_hir_add_generic_param(&context, &parameter,
        &duplicate_parameter_id) == CM_HIR_INVARIANT_VIOLATION);
    assert(duplicate_parameter_id == CM_HIR_GENERIC_PARAM_NONE);
    assert(cm_hir_set_generic_param_default(&context,
        CM_HIR_GENERIC_PARAM_NONE, &generic_default)
        == CM_HIR_INVALID_ARGUMENT);
    assert(cm_hir_set_generic_param_default(&context,
        (CmHirGenericParamId)9999u, &generic_default)
        == CM_HIR_INVALID_ID);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(25u, 26u);
    type.data.parameter_type.parameter = 1u;
    assert(cm_hir_add_type(&context, &type, &parameter_type) == CM_HIR_OK);

    memset(node_arguments, 0, sizeof(node_arguments));
    node_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    node_arguments[0].data.type = parameter_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(30u, 37u);
    type.data.named_type.definition = node_definition;
    type.data.named_type.arguments = node_arguments;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &node_type) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(30u, 38u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = node_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    assert(cm_hir_add_type(&context, &type, &node_ref_type) == CM_HIR_OK);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ARRAY_KIND;
    type.span = test_span(40u, 48u);
    type.data.array_type.element = u8_type;
    type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
    type.data.array_type.length.type = usize_type;
    type.data.array_type.length.data.value.low_bits = 16u;
    type.data.array_type.length.data.value.high_bits = 1u;
    assert(cm_hir_add_type(&context, &type, &array_type) == CM_HIR_OK);

    tuple_elements[0] = parameter_type;
    tuple_elements[1] = node_ref_type;
    tuple_elements[2] = array_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = test_span(50u, 70u);
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 3u;
    assert(cm_hir_add_type(&context, &type, &tuple_type) == CM_HIR_OK);
    tuple_elements[0] = CM_HIR_TYPE_NONE;
    stored_tuple = cm_hir_get_type(&context, tuple_type);
    assert(stored_tuple != NULL);
    assert(stored_tuple->data.tuple_type.elements[0] == parameter_type);

    memset(&fields, 0, sizeof(fields));
    fields[0].name = cm_hir_intern(&context, "next");
    fields[0].type = node_ref_type;
    fields[0].span = test_span(40u, 48u);
    fields[1].name = cm_hir_intern(&context, "payload");
    fields[1].type = parameter_type;
    fields[1].span = test_span(50u, 60u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = node_definition;
    item.owner_module = child_id;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "Node");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(20u, 100u);
    item.generic_parameter_start = 1u;
    item.generic_parameter_count = 2u;
    item.data.aggregate_item.form = CM_HIR_AGGREGATE_NAMED;
    item.data.aggregate_item.fields = fields;
    item.data.aggregate_item.field_count = 2u;
    assert(cm_hir_add_item(&context, &item, &node_item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(node_item_id == CM_HIR_ITEM_NONE);
    item.generic_parameter_count = 1u;
    assert(cm_hir_add_item(&context, &item, &node_item_id) == CM_HIR_OK);
    fields[0].type = CM_HIR_TYPE_NONE;
    stored_node = cm_hir_get_item(&context, node_item_id);
    assert(stored_node != NULL);
    assert(stored_node->generic_parameter_start == 1u
        && stored_node->generic_parameter_count == 1u);
    assert(stored_node->data.aggregate_item.fields[0].type == node_ref_type);
    stored_definition = cm_hir_lookup_definition(&context, node_definition);
    assert(stored_definition->state == CM_HIR_DEFINITION_BOUND);
    assert(stored_definition->entity.item_id == node_item_id);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(101u, 109u), &alias_definition) == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = alias_definition;
    parameter.name = cm_hir_intern(&context, "U");
    parameter.span = test_span(102u, 103u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &alias_parameter_id) == CM_HIR_OK);
    assert(alias_parameter_id == 2u);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(102u, 103u);
    type.data.parameter_type.parameter = alias_parameter_id;
    assert(cm_hir_add_type(&context, &type, &alias_self_type) == CM_HIR_OK);
    memset(&generic_default, 0, sizeof(generic_default));
    generic_default.kind = CM_HIR_GENERIC_ARG_TYPE;
    generic_default.data.type = alias_self_type;
    assert(cm_hir_set_generic_param_default(&context, alias_parameter_id,
        &generic_default) == CM_HIR_INVARIANT_VIOLATION);
    assert(!cm_hir_get_generic_param(&context,
        alias_parameter_id)->has_default);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = alias_definition;
    item.owner_module = child_id;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "Alias");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(101u, 109u);
    item.generic_parameter_start = alias_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = node_type;
    assert(cm_hir_add_item(&context, &item, &alias_item_id) == CM_HIR_OK);
    stored_alias = cm_hir_get_item(&context, alias_item_id);
    assert(stored_alias != NULL
        && stored_alias->generic_parameter_start == alias_parameter_id
        && stored_alias->generic_parameter_count == 1u);
    generic_default.data.type = u8_type;
    assert(cm_hir_set_generic_param_default(&context, alias_parameter_id,
        &generic_default) == CM_HIR_INVARIANT_VIOLATION);
    assert(!cm_hir_get_generic_param(&context,
        alias_parameter_id)->has_default);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(105u, 109u), &malformed_signature_definition)
        == CM_HIR_OK);
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = malformed_signature_definition;
    parameter.name = cm_hir_intern(&context, "D");
    parameter.span = test_span(105u, 106u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &malformed_default_id) == CM_HIR_OK);
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&context, "R");
    parameter.span = test_span(106u, 107u);
    assert(cm_hir_add_generic_param(&context, &parameter,
        &malformed_required_id) == CM_HIR_OK);
    generic_default.kind = CM_HIR_GENERIC_ARG_TYPE;
    generic_default.data.type = u8_type;
    assert(cm_hir_set_generic_param_default(&context,
        malformed_default_id, &generic_default) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    item.definition = malformed_signature_definition;
    item.owner_module = child_id;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "MalformedDefaults");
    item.visibility.kind = CM_HIR_VIS_PRIVATE;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(105u, 109u);
    item.generic_parameter_start = malformed_default_id;
    item.generic_parameter_count = 2u;
    item.data.type_alias_item.target = u8_type;
    assert(cm_hir_add_item(&context, &item, &malformed_item_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(malformed_item_id == CM_HIR_ITEM_NONE);
    stored_definition = cm_hir_lookup_definition(&context,
        malformed_signature_definition);
    assert(stored_definition != NULL
        && stored_definition->state == CM_HIR_DEFINITION_RESERVED);

    assert(cm_hir_reserve_item_definition(&context, crate_id,
        test_span(110u, 180u), &function_definition) == CM_HIR_OK);
    memset(locals, 0, sizeof(locals));
    locals[0].name = cm_hir_intern(&context, "value");
    locals[0].type = tuple_type;
    locals[0].span = test_span(120u, 125u);
    memset(&body, 0, sizeof(body));
    body.owner = function_definition;
    body.origin = cm_hir_body_origin_item_source(function_definition);
    body.state = CM_HIR_BODY_UNLOWERED;
    body.expected_type = unit_type;
    body.locals = locals;
    body.local_count = 1u;
    body.parameter_count = 1u;
    body.source = 0u;
    body.source_expression_id = 77u;
    body.span = test_span(125u, 180u);
    assert(cm_hir_add_body(&context, &body, &body_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(body_id == CM_HIR_BODY_NONE);
    body.source = 1u;
    assert(cm_hir_add_body(&context, &body, &body_id) == CM_HIR_OK);
    locals[0].type = CM_HIR_TYPE_NONE;
    assert(cm_hir_get_body(&context, body_id)->locals[0].type == tuple_type);

    memset(function_parameters, 0, sizeof(function_parameters));
    function_parameters[0].name = cm_hir_intern(&context, "value");
    function_parameters[0].type = tuple_type;
    function_parameters[0].span = test_span(115u, 124u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = function_definition;
    item.owner_module = child_id;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&context, "consume");
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(110u, 180u);
    item.data.function_item.signature.parameters = function_parameters;
    item.data.function_item.signature.parameter_count = 1u;
    item.data.function_item.signature.return_type = unit_type;
    item.data.function_item.signature.abi = rust_abi;
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    assert(cm_hir_add_item(&context, &item, &function_item_id) == CM_HIR_OK);
    assert(function_item_id != CM_HIR_ITEM_NONE);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = test_span(181u, 184u);
    type.data.raw_pointer_type.pointee = u8_type;
    type.data.raw_pointer_type.mutability = CM_HIR_MUTABLE;
    assert(cm_hir_add_type(&context, &type, &raw_pointer_type)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SLICE_KIND;
    type.span = test_span(184u, 187u);
    type.data.slice_type.element = u8_type;
    assert(cm_hir_add_type(&context, &type, &slice_type) == CM_HIR_OK);
    fn_parameters[0] = raw_pointer_type;
    fn_parameters[1] = slice_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_FN_POINTER_KIND;
    type.span = test_span(181u, 189u);
    type.data.fn_pointer_type.parameters = fn_parameters;
    type.data.fn_pointer_type.parameter_count = 2u;
    type.data.fn_pointer_type.return_type = unit_type;
    type.data.fn_pointer_type.abi = cm_hir_intern(&context, "C");
    type.data.fn_pointer_type.safety = CM_HIR_UNSAFE;
    type.data.fn_pointer_type.is_variadic = 1;
    assert(cm_hir_add_type(&context, &type, &fn_pointer_type) == CM_HIR_OK);
    fn_parameters[0] = CM_HIR_TYPE_NONE;
    assert(cm_hir_get_type(&context, fn_pointer_type)
        ->data.fn_pointer_type.parameters[0] == raw_pointer_type);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INFER_KIND;
    type.span = test_span(189u, 190u);
    type.data.infer_type.kind = CM_HIR_INFER_INTEGER;
    type.data.infer_type.variable = 42u;
    assert(cm_hir_add_type(&context, &type, &inference_type) == CM_HIR_OK);
    assert(inference_type != CM_HIR_TYPE_NONE);

    memset(alias_arguments, 0, sizeof(alias_arguments));
    alias_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    alias_arguments[0].data.type = u8_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = test_span(189u, 190u);
    type.data.named_type.definition = alias_definition;
    type.data.named_type.arguments = alias_arguments;
    type.data.named_type.argument_count = 1u;
    assert(cm_hir_add_type(&context, &type, &alias_application_type)
        == CM_HIR_OK);
    alias_arguments[0].data.type = CM_HIR_TYPE_NONE;
    stored_alias_application = cm_hir_get_type(&context,
        alias_application_type);
    assert(stored_alias_application != NULL
        && stored_alias_application->data.named_type.arguments[0].data.type
            == u8_type);

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = test_span(190u, 191u);
    type.data.reference_type.region.kind = CM_HIR_REGION_STATIC;
    type.data.reference_type.pointee = CM_HIR_TYPE_NONE;
    assert(cm_hir_add_type(&context, &type, &bad_type_id)
        == CM_HIR_INVALID_ID);
    assert(bad_type_id == CM_HIR_TYPE_NONE);
    type.data.reference_type.pointee = u8_type;
    type.data.reference_type.mutability = (CmHirMutability)99;
    assert(cm_hir_add_type(&context, &type, &bad_type_id)
        == CM_HIR_INVALID_ID);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ADT_KIND;
    type.span = test_span(190u, 191u);
    type.data.named_type.definition.crate_id = crate_id;
    type.data.named_type.definition.index = 999u;
    assert(cm_hir_add_type(&context, &type, &bad_type_id)
        == CM_HIR_INVALID_ID);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ERROR_KIND;
    type.span = test_span(190u, 191u);
    assert(cm_hir_add_type(&context, &type, &bad_type_id)
        == CM_HIR_INVALID_ID);
    type.data.error_type.reason = reason;
    assert(cm_hir_add_type(&context, &type, &bad_type_id) == CM_HIR_OK);

    first_file = tmpfile();
    second_file = tmpfile();
    assert(first_file != NULL);
    assert(second_file != NULL);
    assert(cm_hir_dump(first_file, &context) == 0);
    assert(cm_hir_dump(second_file, &context) == 0);
    first_dump = read_dump(first_file);
    second_dump = read_dump(second_file);
    assert(strcmp(first_dump, second_dump) == 0);
    assert(strstr(first_dump, "type#2 u8") != NULL);
    assert(strstr(first_dump, "type#3 usize") != NULL);
    assert(strstr(first_dump, "adt 1:3<ty#4>") != NULL);
    assert(strstr(first_dump, "alias-app 1:4<ty#2>") != NULL);
    assert(strstr(first_dump,
        "[ty#2;bits=0x10000000000000010:ty#3]") != NULL);
    assert(strstr(first_dump, "state=unlowered") != NULL);
    assert(strstr(first_dump, "*mut ty#2") != NULL);
    assert(strstr(first_dump, "unsafe fn[\"C\"]") != NULL);
    assert(strncmp(first_dump, "hir-v35\n", strlen("hir-v35\n")) == 0);
    assert(strstr(first_dump, "source-expr=1:77") != NULL);
    assert(strstr(first_dump, "infer[1]?42") != NULL);
    assert(strstr(first_dump,
        "item#1 struct def=1:3 module#2 parent=none name=\"Node\" "
        "generics=1..2") != NULL);
    assert(strstr(first_dump,
        "crate-attr crate#1 source-attr=1 depth=1 meta=\"no_core\"")
        != NULL);
    assert(strstr(first_dump,
        "module-outer-attr module#2 source-attr=3 depth=0 "
        "meta=\"stable(feature = \\\"module_attrs\\\", since = "
        "\\\"1.0.0\\\")\"") != NULL);
    assert(strstr(first_dump,
        "module-inner-attr module#2 source-attr=2 depth=0 "
        "meta=\"allow(dead_code)\"") != NULL);
    free(second_dump);
    free(first_dump);
    assert(fclose(second_file) == 0);
    assert(fclose(first_file) == 0);

    assert(cm_hir_get_crate(&context, CM_HIR_CRATE_NONE) == NULL);
    assert(cm_hir_get_type(&context, UINT32_MAX) == NULL);
    cm_hir_context_destroy(&context);
    assert(context.types.data == NULL);
    test_method_and_item_attribute_model();
    test_structural_import_model();
    test_enum_variant_definition_model();
    test_macro_definition_model();
    test_function_pointer_lifetime_binder_model();
    test_scoped_self_and_receiver_invariants();
    test_body_public_invariants();
    test_metadata_recipe_body_origin();
    test_discard_parameter_model();
    test_tuple_parameter_model();
    test_unary_rust_call_tuple_parameter_model();
    test_newtype_parameter_model();
    test_deref_shared_parameter_model();
    test_supertrait_model_invariants();
    test_default_body_value_model_invariants();
    test_trait_alias_model_invariants();
    test_static_supertrait_model_invariants();
    test_supertrait_equality_model_invariants();
    test_self_root_ingress_invariants();
    test_boundless_associated_type_prebinding();
    test_associated_type_bound_model_invariants();
    test_item_trait_predicate_model_invariants();
    test_trait_predicate_lifetime_binder_model();
    test_trait_predicate_equality_model_invariants();
    test_reference_expression_model();
    test_method_call_expression_model();
    test_aggregate_expression_model();
    test_context_transaction_marks();
    test_closure_hir_model();
    test_closure_capture_semantic_mark();
    test_closure_capture_semantic_mark_failures();
    test_context_generation_exhaustion_boundary();
    test_adt_generic_default_model();
    test_generic_default_resource_limits();
    test_generic_default_nominal_depth_boundary();
    test_const_generic_default_type_invariants();
    test_const_generic_trait_method_model();
    test_auto_trait_and_negative_impl_model();
    return 0;
}
