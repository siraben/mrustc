#include "../../src/hir/library_internal.h"
#include "cm/hir/lower.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct TestFixture {
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId root_definition;
    CmHirModuleId child_module;
    CmHirDefId child_definition;
    CmHirDefId api_definition;
    CmHirDefId replacement_definition;
} TestFixture;

typedef struct OwnedDataView {
    unsigned char *modules_data;
    size_t module_count;
    unsigned char *entries_data;
    size_t entry_count;
    unsigned char *names_data;
    size_t name_count;
} OwnedDataView;

typedef struct TestEnum {
    CmHirDefId definition;
    CmHirDefId variant_definition;
    CmHirItemId item_id;
} TestEnum;

static CmSpan test_span(uint32_t start, uint32_t end)
{
    CmSpan span;

    span.source = 1u;
    span.start = start;
    span.end = end;
    return span;
}

static CmHirDefId add_public_extern_type(TestFixture *fixture,
    const char *name, uint32_t start)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;

    assert(cm_hir_reserve_item_definition_as(&fixture->context,
        fixture->crate_id, CM_HIR_ITEM_EXTERN_TYPE,
        test_span(start, start + 1u), &definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_EXTERN_TYPE;
    item.definition = definition;
    item.owner_module = fixture->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&fixture->context, name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 1u);
    assert(cm_hir_add_item(&fixture->context, &item, &item_id)
        == CM_HIR_OK);
    assert(item_id != CM_HIR_ITEM_NONE);
    return definition;
}

static CmHirTypeId add_bool_type(CmHirContext *context, uint32_t start)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = test_span(start, start + 1u);
    assert(cm_hir_add_type(context, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static CmHirDefId add_public_struct(TestFixture *fixture, const char *name,
    CmHirAggregateForm form, uint32_t start, int non_exhaustive)
{
    CmHirDefId definition;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirField field;
    CmHirAttribute attribute;

    assert(cm_hir_reserve_item_definition_as(&fixture->context,
        fixture->crate_id, CM_HIR_ITEM_STRUCT, test_span(start, start + 5u),
        &definition) == CM_HIR_OK);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_STRUCT;
    item.definition = definition;
    item.owner_module = fixture->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&fixture->context, name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 5u);
    item.data.aggregate_item.form = form;
    if (non_exhaustive) {
        memset(&attribute, 0, sizeof(attribute));
        attribute.metadata = cm_hir_intern(&fixture->context,
            "non_exhaustive");
        attribute.span = test_span(start, start + 1u);
        attribute.source_attribute = 1u;
        item.attributes = &attribute;
        item.attribute_count = 1u;
    }
    if (form != CM_HIR_AGGREGATE_UNIT) {
        memset(&field, 0, sizeof(field));
        field.name = form == CM_HIR_AGGREGATE_NAMED
            ? cm_hir_intern(&fixture->context, "field")
            : CM_INTERN_ID_NONE;
        field.type = add_bool_type(&fixture->context, start + 1u);
        field.visibility.kind = CM_HIR_VIS_PUBLIC;
        field.visibility.restriction = cm_hir_def_id_none();
        field.span = test_span(start + 1u, start + 2u);
        item.data.aggregate_item.fields = &field;
        item.data.aggregate_item.field_count = 1u;
    }
    assert(cm_hir_add_item(&fixture->context, &item, &item_id) == CM_HIR_OK);
    assert(item_id != CM_HIR_ITEM_NONE);
    return definition;
}

static TestEnum add_public_unit_enum(TestFixture *fixture, const char *name,
    const char *variant_name, uint32_t start)
{
    TestEnum result;
    CmHirVariant variant;
    CmHirItem item;

    memset(&result, 0, sizeof(result));
    assert(cm_hir_reserve_item_definition_as(&fixture->context,
        fixture->crate_id, CM_HIR_ITEM_ENUM, test_span(start, start + 9u),
        &result.definition) == CM_HIR_OK);
    assert(cm_hir_reserve_enum_variant_definition(&fixture->context,
        fixture->crate_id, test_span(start + 2u, start + 6u),
        &result.variant_definition) == CM_HIR_OK);
    memset(&variant, 0, sizeof(variant));
    variant.definition = result.variant_definition;
    variant.name = cm_hir_intern(&fixture->context, variant_name);
    variant.form = CM_HIR_AGGREGATE_UNIT;
    variant.span = test_span(start + 2u, start + 6u);
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_ENUM;
    item.definition = result.definition;
    item.owner_module = fixture->root_module;
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_hir_intern(&fixture->context, name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = test_span(start, start + 9u);
    item.data.enum_item.variants = &variant;
    item.data.enum_item.variant_count = 1u;
    assert(cm_hir_add_item(&fixture->context, &item, &result.item_id)
        == CM_HIR_OK);
    assert(result.item_id != CM_HIR_ITEM_NONE);
    return result;
}

static void fixture_init(TestFixture *fixture)
{
    const CmHirModule *module;

    memset(fixture, 0, sizeof(*fixture));
    cm_hir_context_init(&fixture->context);
    assert(cm_hir_create_crate(&fixture->context,
        cm_hir_intern(&fixture->context, "library_restore"),
        CM_HIR_EDITION_2024, test_span(0u, 100u), &fixture->crate_id,
        &fixture->root_module) == CM_HIR_OK);
    module = cm_hir_get_module(&fixture->context, fixture->root_module);
    assert(module != NULL);
    fixture->root_definition = module->definition;
    assert(cm_hir_add_module(&fixture->context, fixture->crate_id,
        fixture->root_module, cm_hir_intern(&fixture->context, "child"),
        test_span(10u, 20u), &fixture->child_module) == CM_HIR_OK);
    module = cm_hir_get_module(&fixture->context, fixture->child_module);
    assert(module != NULL);
    fixture->child_definition = module->definition;
    fixture->api_definition = add_public_extern_type(fixture, "Api", 30u);
    fixture->replacement_definition = add_public_extern_type(fixture,
        "Replacement", 40u);
}

static CmHirLibraryBinding type_binding(CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_FOREIGN_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    return binding;
}

static CmHirLibraryBinding adt_binding(CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_ADT_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    return binding;
}

static CmHirLibraryBinding constructor_binding(CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR;
    binding.definition = definition;
    binding.type_kind = CM_HIR_TYPE_ADT_KIND;
    binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
    binding.value_kind = CM_HIR_LIBRARY_VALUE_NONE;
    return binding;
}

static size_t add_snapshot_module(CmHirLibraryOwnedData *data,
    CmHirDefId definition)
{
    size_t module_index;

    assert(cm_hir_library_owned_data_add_module(data, definition,
        &module_index) == CM_HIR_LIBRARY_OK);
    return module_index;
}

static void add_snapshot_entry(CmHirLibraryOwnedData *data,
    size_t module_index, const char *name, CmHirDefId definition)
{
    CmHirLibraryBinding binding;

    binding = type_binding(definition);
    assert(cm_hir_library_owned_data_add_entry(data, module_index,
        (const unsigned char *)name, strlen(name), &binding)
        == CM_HIR_LIBRARY_OK);
}

static void snapshot_init_one(CmHirLibraryOwnedData *data,
    CmHirDefId module_definition, const char *name,
    CmHirDefId target_definition)
{
    size_t module_index;

    cm_hir_library_owned_data_init(data);
    module_index = add_snapshot_module(data, module_definition);
    add_snapshot_entry(data, module_index, name, target_definition);
}

static OwnedDataView owned_data_view(const CmHirLibraryOwnedData *data)
{
    OwnedDataView view;
    const CmHirLibraryOwnedModule *module;

    memset(&view, 0, sizeof(view));
    view.modules_data = data->modules.data;
    view.module_count = data->modules.len;
    view.names_data = data->names.entries.data;
    view.name_count = cm_interner_length(&data->names);
    module = (const CmHirLibraryOwnedModule *)cm_vec_at_const(
        &data->modules, 0u);
    if (module != NULL) {
        view.entries_data = module->entries.data;
        view.entry_count = module->entries.len;
    }
    return view;
}

static void assert_owned_data_unchanged(const CmHirLibraryOwnedData *data,
    OwnedDataView before)
{
    OwnedDataView after;

    after = owned_data_view(data);
    assert(after.modules_data == before.modules_data);
    assert(after.module_count == before.module_count);
    assert(after.entries_data == before.entries_data);
    assert(after.entry_count == before.entry_count);
    assert(after.names_data == before.names_data);
    assert(after.name_count == before.name_count);
}

static void assert_artifact_entry(const CmHirLibraryArtifact *artifact,
    const char *extern_name, const char *entry_name,
    CmHirDefId expected_definition)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;

    path[0].bytes = (const unsigned char *)extern_name;
    path[0].length = strlen(extern_name);
    path[1].bytes = (const unsigned char *)entry_name;
    path[1].length = strlen(entry_name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_TYPE);
    assert(binding.type_kind == CM_HIR_TYPE_FOREIGN_KIND);
    assert(binding.primitive_kind == CM_HIR_PRIMITIVE_NONE);
    assert(cm_hir_def_id_equal(binding.definition, expected_definition));
}

static void assert_failed_restore(CmHirLibraryArtifact *artifact,
    const TestFixture *fixture, CmHirLibraryOwnedData *candidate,
    CmHirCrateId crate_id, CmHirDefId root_definition,
    const char *extern_name, CmHirLibraryStatus expected_status,
    const char *stable_extern_name, CmHirDefId stable_definition)
{
    CmHirLibraryArtifactIdentity before_identity;
    CmHirLibraryArtifactIdentity after_identity;
    CmHirLibraryArtifactResult result;
    OwnedDataView before_candidate;

    assert(cm_hir_library_artifact_identity(artifact, &before_identity));
    before_candidate = owned_data_view(candidate);
    result = cm_hir_library_artifact_restore_owned(artifact,
        &fixture->context, crate_id, root_definition, extern_name, candidate);
    assert(result.status == expected_status);
    assert(result.module_count == 0u);
    assert(result.public_type_entry_count == 0u);
    assert(result.public_value_entry_count == 0u);
    assert_owned_data_unchanged(candidate, before_candidate);
    assert(cm_hir_library_artifact_identity(artifact, &after_identity));
    assert(after_identity.context == before_identity.context);
    assert(after_identity.crate_id == before_identity.crate_id);
    assert(cm_hir_def_id_equal(after_identity.root_definition,
        before_identity.root_definition));
    assert(after_identity.extern_name == before_identity.extern_name);
    assert_artifact_entry(artifact, stable_extern_name, "Api",
        stable_definition);
}

static void add_binding_entry(CmHirLibraryOwnedData *data,
    size_t module_index, const char *name,
    const CmHirLibraryBinding *binding)
{
    assert(cm_hir_library_owned_data_add_entry(data, module_index,
        (const unsigned char *)name, strlen(name), binding)
        == CM_HIR_LIBRARY_OK);
}

static void assert_value_binding(const CmHirLibraryArtifact *artifact,
    const char *extern_name, const char *name, CmHirLibraryBindingKind kind,
    CmHirDefId definition)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;

    path[0].bytes = (const unsigned char *)extern_name;
    path[0].length = strlen(extern_name);
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == kind);
    assert(binding.type_kind == CM_HIR_TYPE_ADT_KIND);
    assert(binding.value_kind == CM_HIR_LIBRARY_VALUE_NONE);
    assert(cm_hir_def_id_equal(binding.definition, definition));
}

static void test_struct_constructor_restore(void)
{
    TestFixture fixture;
    TestFixture fresh;
    CmHirDefId unit_definition;
    CmHirDefId tuple_definition;
    CmHirDefId named_definition;
    CmHirDefId non_exhaustive_definition;
    CmHirDefId fresh_unit_definition;
    CmHirLibraryArtifact artifact;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult result;
    CmHirLibraryBinding binding;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryValue value;
    CmHirLibraryValue fake_value;
    CmHirTypeId fake_return_type;
    size_t root_index;

    fixture_init(&fixture);
    unit_definition = add_public_struct(&fixture, "AllocError",
        CM_HIR_AGGREGATE_UNIT, 50u, 0);
    tuple_definition = add_public_struct(&fixture, "Tuple",
        CM_HIR_AGGREGATE_TUPLE, 60u, 0);
    named_definition = add_public_struct(&fixture, "Named",
        CM_HIR_AGGREGATE_NAMED, 70u, 0);
    non_exhaustive_definition = add_public_struct(&fixture,
        "NonExhaustive", CM_HIR_AGGREGATE_TUPLE, 75u, 1);
    cm_hir_library_artifact_init(&artifact);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = adt_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    binding = constructor_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    binding = adt_binding(tuple_definition);
    add_binding_entry(&owned, root_index, "Tuple", &binding);
    binding = constructor_binding(tuple_definition);
    add_binding_entry(&owned, root_index, "Tuple", &binding);
    binding = adt_binding(named_definition);
    add_binding_entry(&owned, root_index, "Named", &binding);
    binding = type_binding(fixture.api_definition);
    add_binding_entry(&owned, root_index, "Api", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact,
        &fixture.context, fixture.crate_id, fixture.root_definition,
        "dep", &owned);
    assert(result.status == CM_HIR_LIBRARY_OK);
    assert(result.public_type_entry_count == 4u);
    assert(result.public_value_entry_count == 2u);
    assert_value_binding(&artifact, "dep", "AllocError",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, unit_definition);
    assert_value_binding(&artifact, "dep", "Tuple",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, tuple_definition);
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)"Named";
    path[1].length = 5u;
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);
    path[1].bytes = (const unsigned char *)"AllocError";
    path[1].length = 10u;
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
    cm_hir_library_owned_data_destroy(&owned);

    /* A non_exhaustive constructor is crate-visible, not exportable. */
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = adt_binding(non_exhaustive_definition);
    add_binding_entry(&owned, root_index, "NonExhaustive", &binding);
    binding = constructor_binding(non_exhaustive_definition);
    add_binding_entry(&owned, root_index, "NonExhaustive", &binding);
    assert_failed_restore(&artifact, &fixture, &owned, fixture.crate_id,
        fixture.root_definition, "non_exhaustive",
        CM_HIR_LIBRARY_INVALID_HIR, "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* A constructor cannot be laundered into a fabricated free function. */
    fake_return_type = add_bool_type(&fixture.context, 80u);
    memset(&fake_value, 0, sizeof(fake_value));
    fake_value.definition = unit_definition;
    fake_value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    fake_value.data.function.return_type = fake_return_type;
    fake_value.data.function.generic_parameter_start
        = CM_HIR_GENERIC_PARAM_NONE;
    fake_value.data.function.abi = cm_hir_intern(&fixture.context, "Rust");
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = adt_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    assert(cm_hir_library_owned_data_add_value(&owned, &fake_value)
        == CM_HIR_LIBRARY_OK);
    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
    binding.definition = unit_definition;
    binding.value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    assert_failed_restore(&artifact, &fixture, &owned, fixture.crate_id,
        fixture.root_definition, "fake_function",
        CM_HIR_LIBRARY_INVALID_HIR, "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* Missing the same-name TYPE mate is an unauthenticated constructor. */
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = constructor_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    assert_failed_restore(&artifact, &fixture, &owned, fixture.crate_id,
        fixture.root_definition, "missing", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* A named-field struct cannot be authenticated as a constructor. */
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = adt_binding(named_definition);
    add_binding_entry(&owned, root_index, "Named", &binding);
    binding = constructor_binding(named_definition);
    add_binding_entry(&owned, root_index, "Named", &binding);
    assert_failed_restore(&artifact, &fixture, &owned, fixture.crate_id,
        fixture.root_definition, "fake", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* The constructor DefId must equal its exact same-name TYPE target. */
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = adt_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    binding = constructor_binding(tuple_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    assert_failed_restore(&artifact, &fixture, &owned, fixture.crate_id,
        fixture.root_definition, "wrong_def", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* Producer-local DefIds do not authenticate against a shifted context. */
    fixture_init(&fresh);
    (void)add_public_extern_type(&fresh, "Shift", 45u);
    fresh_unit_definition = add_public_struct(&fresh, "AllocError",
        CM_HIR_AGGREGATE_UNIT, 50u, 0);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fresh.root_definition);
    binding = adt_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    binding = constructor_binding(unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact, &fresh.context,
        fresh.crate_id, fresh.root_definition, "fresh_bad", &owned);
    assert(result.status == CM_HIR_LIBRARY_INVALID_HIR);
    assert(owned.modules.len == 1u);
    assert_artifact_entry(&artifact, "dep", "Api", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&owned);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fresh.root_definition);
    binding = adt_binding(fresh_unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    binding = constructor_binding(fresh_unit_definition);
    add_binding_entry(&owned, root_index, "AllocError", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact, &fresh.context,
        fresh.crate_id, fresh.root_definition, "fresh", &owned);
    assert(result.status == CM_HIR_LIBRARY_OK);
    assert_value_binding(&artifact, "fresh", "AllocError",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, fresh_unit_definition);
    cm_hir_library_owned_data_destroy(&owned);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&fresh.context);
    cm_hir_context_destroy(&fixture.context);
}

static const CmHirItem *find_item_named(const CmHirContext *context,
    const char *name)
{
    size_t index;
    size_t name_length;

    name_length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        item_name = item == NULL ? NULL
            : cm_interner_get(&context->strings, item->name);
        if (item_name != NULL && item_name->len == name_length
            && memcmp(item_name->bytes, name, name_length) == 0) return item;
    }
    return NULL;
}

static CmHirLowerResult lower_test_graph(CmHirContext *context,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    CmHirModuleMap *map, const CmHirLowerOptions *options)
{
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirLowerResult result;

    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, graph, revision);
    assert(import_result.error_count == 0u);
    result = cm_hir_lower_module_graph(context, graph, revision, &imports,
        map, options);
    cm_import_resolver_destroy(&imports);
    return result;
}

static void test_struct_constructor_direct_capture(void)
{
    static const unsigned char source_text[] =
        "pub struct AllocError;\n"
        "pub struct Tuple(pub u8);\n"
        "pub struct CrateTuple(pub(crate) u8);\n"
        "pub struct PrivateTuple(u8);\n"
        "#[non_exhaustive] pub struct NonExhaustive(pub u8);\n"
        "pub struct Named { pub field: u8 }\n"
        "pub use Tuple as TupleAlias;\n"
        "pub use PrivateTuple as PrivateAlias;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmHirContext context;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult artifact_result;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    const CmHirItem *unit_item;
    const CmHirItem *tuple_item;
    const CmHirItem *crate_tuple_item;
    const CmHirItem *private_tuple_item;
    const CmHirItem *non_exhaustive_item;
    const CmHirItem *named_item;

    cm_source_set_init(&sources);
    cm_cfg_set_init(&cfg);
    cm_module_graph_init(&graph);
    cm_hir_context_init(&context);
    cm_hir_module_map_init(&map);
    cm_hir_library_artifact_init(&artifact);
    assert(cm_source_add_memory(&sources, "constructor.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "constructor";
    lower_result = lower_test_graph(&context, &graph, graph_result.revision,
        &map, &lower_options);
    assert(lower_result.error_count == 0u);
    unit_item = find_item_named(&context, "AllocError");
    tuple_item = find_item_named(&context, "Tuple");
    crate_tuple_item = find_item_named(&context, "CrateTuple");
    private_tuple_item = find_item_named(&context, "PrivateTuple");
    non_exhaustive_item = find_item_named(&context, "NonExhaustive");
    named_item = find_item_named(&context, "Named");
    assert(unit_item != NULL && unit_item->kind == CM_HIR_ITEM_STRUCT
        && unit_item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT);
    assert(tuple_item != NULL && tuple_item->kind == CM_HIR_ITEM_STRUCT
        && tuple_item->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE);
    assert(crate_tuple_item != NULL
        && crate_tuple_item->kind == CM_HIR_ITEM_STRUCT
        && private_tuple_item != NULL
        && private_tuple_item->kind == CM_HIR_ITEM_STRUCT
        && non_exhaustive_item != NULL
        && non_exhaustive_item->kind == CM_HIR_ITEM_STRUCT);
    assert(named_item != NULL && named_item->kind == CM_HIR_ITEM_STRUCT
        && named_item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED);
    artifact_result = cm_hir_library_declaration_artifact_build(&artifact,
        &context, lower_result.crate_id, &graph, graph_result.revision, &map,
        "dep");
    assert(artifact_result.status == CM_HIR_LIBRARY_OK);
    assert(artifact_result.public_type_entry_count == 8u);
    assert(artifact_result.public_value_entry_count == 3u);
    assert_value_binding(&artifact, "dep", "AllocError",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, unit_item->definition);
    assert_value_binding(&artifact, "dep", "Tuple",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, tuple_item->definition);
    assert_value_binding(&artifact, "dep", "TupleAlias",
        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR, tuple_item->definition);
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)"AllocError";
    path[1].length = 10u;
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(&artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.kind == CM_HIR_TYPE_ADT_KIND);
    assert(cm_hir_def_id_equal(type.definition, unit_item->definition));
    path[1].bytes = (const unsigned char *)"Named";
    path[1].length = 5u;
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);
    path[1].bytes = (const unsigned char *)"PrivateAlias";
    path[1].length = 12u;
    assert(cm_hir_library_artifact_lookup_type(&artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);
    path[1].bytes = (const unsigned char *)"CrateTuple";
    path[1].length = 10u;
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);
    path[1].bytes = (const unsigned char *)"NonExhaustive";
    path[1].length = 13u;
    assert(cm_hir_library_artifact_lookup_value_binding(&artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_NOT_FOUND);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&context);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_unit_variant_binding(
    const CmHirLibraryArtifact *artifact, const char *enum_name,
    CmHirDefId enum_definition, CmHirDefId variant_definition)
{
    CmHirLibraryPathSegment path[3];
    CmHirLibraryBinding binding;
    CmHirLibraryType type;
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)enum_name;
    path[1].length = strlen(enum_name);
    path[2].bytes = (const unsigned char *)"Null";
    path[2].length = 4u;
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && binding.primitive_kind == CM_HIR_PRIMITIVE_NONE
        && binding.value_kind == CM_HIR_LIBRARY_VALUE_NONE
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition)
        && binding.enum_variant_index == 0u
        && binding.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(artifact, path, 3u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && type.primitive_kind == CM_HIR_PRIMITIVE_NONE
        && cm_hir_def_id_equal(type.definition, variant_definition)
        && cm_hir_def_id_equal(type.enum_definition, enum_definition)
        && type.enum_variant_index == 0u
        && type.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition));
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 3u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
}

static void test_enum_variant_restore_scope(void)
{
    TestFixture fixture;
    TestFixture fresh;
    TestEnum enumeration;
    TestEnum fresh_enumeration;
    CmHirLibraryArtifact artifact;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifactResult result;
    CmHirLibraryBinding binding;
    CmHirLibraryPathSegment path[3];
    CmHirDefinition *variant_definition;
    CmHirItem *enum_item;
    CmHirItemId saved_parent;
    uint32_t saved_index;
    CmHirDefinitionState saved_state;
    CmHirAggregateForm saved_form;
    OwnedDataView before;
    size_t root_index;

    fixture_init(&fixture);
    enumeration = add_public_unit_enum(&fixture, "Char", "Null", 90u);
    cm_hir_library_artifact_init(&artifact);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    binding = type_binding(fixture.api_definition);
    add_binding_entry(&owned, root_index, "Api", &binding);
    binding = adt_binding(enumeration.definition);
    add_binding_entry(&owned, root_index, "Char", &binding);
    add_binding_entry(&owned, root_index, "CharAlias", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact,
        &fixture.context, fixture.crate_id, fixture.root_definition,
        "dep", &owned);
    assert(result.status == CM_HIR_LIBRARY_OK
        && result.public_type_entry_count == 3u
        && result.public_value_entry_count == 0u);
    assert_unit_variant_binding(&artifact, "Char", enumeration.definition,
        enumeration.variant_definition);
    assert_unit_variant_binding(&artifact, "CharAlias",
        enumeration.definition, enumeration.variant_definition);
    cm_hir_library_owned_data_destroy(&owned);

    /* A module-level variant claim cannot bypass its parent enum scope. */
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fixture.root_definition);
    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_ENUM_VARIANT;
    binding.definition = enumeration.variant_definition;
    binding.type_kind = CM_HIR_TYPE_ADT_KIND;
    binding.enum_definition = enumeration.definition;
    binding.enum_variant_form = CM_HIR_AGGREGATE_UNIT;
    before = owned_data_view(&owned);
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Null", 4u, &binding)
        == CM_HIR_LIBRARY_UNSUPPORTED_IMPORT);
    assert_owned_data_unchanged(&owned, before);
    binding.enum_definition = cm_hir_def_id_none();
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Null", 4u, &binding)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    binding.enum_definition = enumeration.definition;
    binding.enum_variant_form = (CmHirAggregateForm)99;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Null", 4u, &binding)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    binding.enum_variant_form = CM_HIR_AGGREGATE_UNIT;
    binding.enum_definition = enumeration.variant_definition;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"Null", 4u, &binding)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    cm_hir_library_owned_data_destroy(&owned);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)"Char";
    path[1].length = 4u;
    path[2].bytes = (const unsigned char *)"Null";
    path[2].length = 4u;
    variant_definition = (CmHirDefinition *)cm_hir_lookup_definition(
        &fixture.context, enumeration.variant_definition);
    enum_item = (CmHirItem *)cm_hir_get_item(&fixture.context,
        enumeration.item_id);
    assert(variant_definition != NULL && enum_item != NULL);
    saved_parent = variant_definition->entity.enum_variant.enum_item_id;
    variant_definition->entity.enum_variant.enum_item_id = CM_HIR_ITEM_NONE;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_INVALID_HIR);
    variant_definition->entity.enum_variant.enum_item_id = saved_parent;
    saved_index = variant_definition->entity.enum_variant.variant_index;
    variant_definition->entity.enum_variant.variant_index = 1u;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_INVALID_HIR);
    variant_definition->entity.enum_variant.variant_index = saved_index;
    saved_state = variant_definition->state;
    variant_definition->state = CM_HIR_DEFINITION_RESERVED;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_INVALID_HIR);
    variant_definition->state = saved_state;
    saved_form = enum_item->data.enum_item.variants[0].form;
    enum_item->data.enum_item.variants[0].form = CM_HIR_AGGREGATE_TUPLE;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_UNSUPPORTED_IMPORT);
    enum_item->data.enum_item.variants[0].form = saved_form;

    /* Producer DefIds cannot be restored into a shifted fresh context. */
    fixture_init(&fresh);
    (void)add_public_extern_type(&fresh, "Shift", 45u);
    fresh_enumeration = add_public_unit_enum(&fresh, "Char", "Null", 90u);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fresh.root_definition);
    binding = adt_binding(enumeration.definition);
    add_binding_entry(&owned, root_index, "Char", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact, &fresh.context,
        fresh.crate_id, fresh.root_definition, "fresh_bad", &owned);
    assert(result.status == CM_HIR_LIBRARY_INVALID_HIR
        && owned.modules.len == 1u);
    assert_unit_variant_binding(&artifact, "Char", enumeration.definition,
        enumeration.variant_definition);
    cm_hir_library_owned_data_destroy(&owned);
    cm_hir_library_owned_data_init(&owned);
    root_index = add_snapshot_module(&owned, fresh.root_definition);
    binding = adt_binding(fresh_enumeration.definition);
    add_binding_entry(&owned, root_index, "Char", &binding);
    result = cm_hir_library_artifact_restore_owned(&artifact, &fresh.context,
        fresh.crate_id, fresh.root_definition, "dep", &owned);
    assert(result.status == CM_HIR_LIBRARY_OK);
    assert_unit_variant_binding(&artifact, "Char",
        fresh_enumeration.definition, fresh_enumeration.variant_definition);
    cm_hir_library_owned_data_destroy(&owned);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&fresh.context);
    cm_hir_context_destroy(&fixture.context);
}

static void test_enum_variant_direct_capture_and_public_reexport_rejection(
    void)
{
    static const unsigned char source_text[] =
        "pub enum Char { Null }\n"
        "pub use Char as CharAlias;\n"
        "use Char::Null as PrivateNull;\n";
    static const unsigned char rejected_source_text[] =
        "pub enum OtherChar { Null }\n"
        "pub use OtherChar::*;\n";
    CmSourceSet sources;
    CmSourceSet rejected_sources;
    CmSourceId root_source;
    CmSourceId rejected_root_source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraph rejected_graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmModuleGraphResult rejected_graph_result;
    CmHirContext context;
    CmHirContext rejected_context;
    CmHirModuleMap map;
    CmHirModuleMap rejected_map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    CmHirLowerResult rejected_lower_result;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult artifact_result;
    const CmHirItem *item;
    CmHirLibraryPathSegment stable_path[3];
    CmHirLibraryBinding stable_binding;

    cm_source_set_init(&sources);
    cm_source_set_init(&rejected_sources);
    cm_cfg_set_init(&cfg);
    cm_module_graph_init(&graph);
    cm_module_graph_init(&rejected_graph);
    cm_hir_context_init(&context);
    cm_hir_context_init(&rejected_context);
    cm_hir_module_map_init(&map);
    cm_hir_module_map_init(&rejected_map);
    cm_hir_library_artifact_init(&artifact);
    assert(cm_source_add_memory(&sources, "enum.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "enum_scope";
    lower_result = lower_test_graph(&context, &graph,
        graph_result.revision, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    item = find_item_named(&context, "Char");
    assert(item != NULL && item->kind == CM_HIR_ITEM_ENUM
        && item->data.enum_item.variant_count == 1u);
    artifact_result = cm_hir_library_declaration_artifact_build(&artifact,
        &context, lower_result.crate_id, &graph, graph_result.revision, &map,
        "dep");
    assert(artifact_result.status == CM_HIR_LIBRARY_OK
        && artifact_result.public_type_entry_count == 2u
        && artifact_result.public_value_entry_count == 0u);
    assert_unit_variant_binding(&artifact, "Char", item->definition,
        item->data.enum_item.variants[0].definition);
    assert_unit_variant_binding(&artifact, "CharAlias", item->definition,
        item->data.enum_item.variants[0].definition);

    assert(cm_source_add_memory(&rejected_sources, "variant-reexport.rs",
        rejected_source_text, sizeof(rejected_source_text) - 1u,
        &rejected_root_source) == CM_SOURCE_OK);
    rejected_graph_result = cm_module_graph_build(&rejected_graph,
        &rejected_sources, rejected_root_source, &graph_options);
    assert(rejected_graph_result.error_count == 0u);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "rejected_enum_scope";
    rejected_lower_result = lower_test_graph(&rejected_context,
        &rejected_graph, rejected_graph_result.revision, &rejected_map,
        &lower_options);
    assert(rejected_lower_result.error_count == 0u);
    artifact_result = cm_hir_library_declaration_artifact_build(&artifact,
        &rejected_context, rejected_lower_result.crate_id, &rejected_graph,
        rejected_graph_result.revision, &rejected_map, "rejected");
    assert(artifact_result.status == CM_HIR_LIBRARY_INVALID_HIR);
    stable_path[0].bytes = (const unsigned char *)"dep";
    stable_path[0].length = 3u;
    stable_path[1].bytes = (const unsigned char *)"Char";
    stable_path[1].length = 4u;
    stable_path[2].bytes = (const unsigned char *)"Null";
    stable_path[2].length = 4u;
    assert(cm_hir_library_artifact_lookup_binding(&artifact, stable_path,
        3u, &stable_binding) == CM_HIR_LIBRARY_OK
        && stable_binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(stable_binding.definition,
            item->data.enum_item.variants[0].definition));

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_module_map_destroy(&rejected_map);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&rejected_context);
    cm_hir_context_destroy(&context);
    cm_module_graph_destroy(&rejected_graph);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&rejected_sources);
    cm_source_set_destroy(&sources);
}

static void test_owned_restore_is_transactional(void)
{
    TestFixture fixture;
    CmHirLibraryArtifact artifact;
    CmHirLibraryOwnedData candidate;
    CmHirLibraryArtifactResult result;
    CmHirLibraryBinding binding;
    CmHirDefId invalid_definition;
    size_t root_index;
    char copied_name[] = "Api";
    char copied_extern_name[] = "dep";

    fixture_init(&fixture);
    cm_hir_library_artifact_init(&artifact);

    cm_hir_library_owned_data_init(&candidate);
    root_index = add_snapshot_module(&candidate, fixture.root_definition);
    binding = type_binding(fixture.api_definition);
    assert(cm_hir_library_owned_data_add_entry(&candidate, root_index,
        (const unsigned char *)copied_name, strlen(copied_name), &binding)
        == CM_HIR_LIBRARY_OK);
    memcpy(copied_name, "Bad", sizeof(copied_name));
    result = cm_hir_library_artifact_restore_owned(&artifact,
        &fixture.context, fixture.crate_id, fixture.root_definition,
        copied_extern_name, &candidate);
    assert(result.status == CM_HIR_LIBRARY_OK);
    assert(result.module_count == 1u);
    assert(result.public_type_entry_count == 1u);
    assert(candidate.modules.len == 0u);
    assert(candidate.modules.elem_size == sizeof(CmHirLibraryOwnedModule));
    assert(cm_interner_length(&candidate.names) == 0u);
    memcpy(copied_extern_name, "bad", sizeof(copied_extern_name));
    assert_artifact_entry(&artifact, "dep", "Api", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    invalid_definition = fixture.api_definition;
    invalid_definition.index = UINT32_MAX;
    snapshot_init_one(&candidate, fixture.root_definition, "Broken",
        invalid_definition);
    assert_failed_restore(&artifact, &fixture, &candidate, fixture.crate_id,
        fixture.root_definition, "broken", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    cm_hir_library_owned_data_init(&candidate);
    (void)add_snapshot_module(&candidate, fixture.child_definition);
    assert_failed_restore(&artifact, &fixture, &candidate, fixture.crate_id,
        fixture.root_definition, "missing_root", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    cm_hir_library_owned_data_init(&candidate);
    root_index = add_snapshot_module(&candidate, fixture.root_definition);
    add_snapshot_entry(&candidate, root_index, "Conflict",
        fixture.api_definition);
    add_snapshot_entry(&candidate, root_index, "Conflict",
        fixture.replacement_definition);
    assert_failed_restore(&artifact, &fixture, &candidate, fixture.crate_id,
        fixture.root_definition, "conflict", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    snapshot_init_one(&candidate, fixture.root_definition, "Other",
        fixture.replacement_definition);
    assert_failed_restore(&artifact, &fixture, &candidate,
        (CmHirCrateId)(fixture.crate_id + 1u), fixture.root_definition,
        "wrong_crate", CM_HIR_LIBRARY_INVALID_ARGUMENT, "dep",
        fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    snapshot_init_one(&candidate, fixture.root_definition, "Other",
        fixture.replacement_definition);
    assert_failed_restore(&artifact, &fixture, &candidate, fixture.crate_id,
        fixture.child_definition, "wrong_root", CM_HIR_LIBRARY_INVALID_HIR,
        "dep", fixture.api_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    snapshot_init_one(&candidate, fixture.root_definition, "Other",
        fixture.replacement_definition);
    assert_failed_restore(&artifact, &fixture, &candidate, fixture.crate_id,
        fixture.root_definition, "not-valid", CM_HIR_LIBRARY_INVALID_ARGUMENT,
        "dep", fixture.api_definition);

    result = cm_hir_library_artifact_restore_owned(&artifact,
        &fixture.context, fixture.crate_id, fixture.root_definition,
        "replacement", &candidate);
    assert(result.status == CM_HIR_LIBRARY_OK);
    assert(result.module_count == 1u);
    assert(result.public_type_entry_count == 1u);
    assert_artifact_entry(&artifact, "replacement", "Other",
        fixture.replacement_definition);
    cm_hir_library_owned_data_destroy(&candidate);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&fixture.context);
}

static void test_owned_predicate_copy_and_equality(void)
{
    CmHirLibraryOwnedData data;
    CmHirLibraryValue value;
    CmHirPredicateScope scope;
    CmHirTraitPredicate predicates[2];
    CmHirOutlivesPredicate outlives;
    CmHirGenericArg arguments[2];
    CmHirAssociatedTypeEquality equality;
    CmInternId scope_lifetimes[1];
    CmInternId predicate_lifetimes[1];
    const CmHirLibraryOwnedValue *stored;
    CmHirDefId definition;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;

    definition.crate_id = 1u;
    definition.index = 10u;
    trait_definition.crate_id = 1u;
    trait_definition.index = 20u;
    associated_definition.crate_id = 1u;
    associated_definition.index = 21u;
    scope_lifetimes[0] = 31u;
    predicate_lifetimes[0] = 32u;
    memset(&scope, 0, sizeof(scope));
    scope.subject_kind = CM_HIR_OUTLIVES_TYPE;
    scope.subject.type = 4u;
    scope.binder.lifetimes = scope_lifetimes;
    scope.binder.lifetime_count = 1u;
    scope.binder.span = test_span(4u, 8u);
    scope.trait_predicate_count = 1u;
    scope.outlives_predicate_count = 1u;
    scope.span = test_span(3u, 9u);
    memset(arguments, 0, sizeof(arguments));
    arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    arguments[0].data.lifetime.kind = CM_HIR_REGION_LATE_BOUND;
    arguments[0].data.lifetime.data.binder_index = 0u;
    arguments[1].kind = CM_HIR_GENERIC_ARG_CONST;
    arguments[1].data.constant.kind = CM_HIR_CONST_VALUE;
    arguments[1].data.constant.type = 5u;
    arguments[1].data.constant.data.value.low_bits = UINT64_C(7);
    arguments[1].data.constant.data.value.high_bits = UINT64_C(9);
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = associated_definition;
    equality.value = 6u;
    equality.span = test_span(6u, 7u);
    memset(predicates, 0, sizeof(predicates));
    predicates[0].subject = 4u;
    predicates[0].trait_type.definition = trait_definition;
    predicates[0].trait_type.arguments = arguments;
    predicates[0].trait_type.argument_count = 2u;
    predicates[0].equalities = &equality;
    predicates[0].equality_count = 1u;
    predicates[0].scope = 1u;
    predicates[0].binder.span = test_span(3u, 9u);
    predicates[0].span = test_span(3u, 9u);
    predicates[0].modifier = CM_HIR_PREDICATE_CONST_IF_CONST;
    predicates[1].subject = 7u;
    predicates[1].trait_type.definition = trait_definition;
    predicates[1].binder.lifetimes = predicate_lifetimes;
    predicates[1].binder.lifetime_count = 1u;
    predicates[1].binder.span = test_span(10u, 14u);
    predicates[1].span = test_span(10u, 14u);
    predicates[1].modifier = CM_HIR_PREDICATE_REQUIRED;
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = 4u;
    outlives.bound.kind = CM_HIR_REGION_LATE_BOUND;
    outlives.bound.data.binder_index = 0u;
    outlives.scope = 1u;
    outlives.span = test_span(3u, 9u);
    memset(&value, 0, sizeof(value));
    value.definition = definition;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.return_type = 7u;
    value.data.function.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    value.data.function.predicate_scopes = &scope;
    value.data.function.predicate_scope_count = 1u;
    value.data.function.predicates = predicates;
    value.data.function.predicate_count = 2u;
    value.data.function.outlives_predicates = &outlives;
    value.data.function.outlives_predicate_count = 1u;
    value.data.function.abi = 1u;

    cm_hir_library_owned_data_init(&data);
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_OK);
    stored = (const CmHirLibraryOwnedValue *)cm_vec_at_const(&data.values, 0u);
    assert(stored != NULL
        && stored->declaration.data.function.predicate_scopes != &scope
        && stored->declaration.data.function.predicates != predicates
        && stored->declaration.data.function.outlives_predicates != &outlives
        && stored->declaration.data.function.predicate_scopes[0]
            .binder.lifetimes != scope_lifetimes
        && stored->declaration.data.function.predicates[0]
            .trait_type.arguments != arguments
        && stored->declaration.data.function.predicates[0].equalities
            != &equality
        && stored->declaration.data.function.predicates[1].binder.lifetimes
            != predicate_lifetimes
        && stored->declaration.data.function.predicates[0]
            .trait_type.arguments[1].data.constant.data.value.high_bits
            == UINT64_C(9));
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_OK);
    scope.span.end += 1u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    scope.span.end -= 1u;
    predicates[0].modifier = CM_HIR_PREDICATE_CONST;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    predicates[0].modifier = CM_HIR_PREDICATE_CONST_IF_CONST;
    arguments[1].data.constant.data.value.high_bits = UINT64_C(10);
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    arguments[1].data.constant.data.value.high_bits = UINT64_C(9);
    equality.span.start += 1u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    equality.span.start -= 1u;
    predicate_lifetimes[0] += 1u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    predicate_lifetimes[0] -= 1u;
    outlives.bound.data.binder_index = 1u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_HIR);
    outlives.bound.data.binder_index = 0u;
    scope.binder.lifetimes = NULL;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    scope.binder.lifetimes = scope_lifetimes;
    predicates[0].trait_type.argument_count = 0u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    predicates[0].trait_type.argument_count = 2u;
    value.data.function.predicate_count = 0u;
    assert(cm_hir_library_owned_data_add_value(&data, &value)
        == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    value.data.function.predicate_count = 2u;
    cm_hir_library_owned_data_destroy(&data);
}

static void test_reserved_predicate_value_rejected(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    const CmHirModule *root;
    CmHirDefId function_definition;
    CmHirType type;
    CmHirTypeId bool_type;
    CmHirOutlivesPredicate outlives;
    CmHirLibraryValue value;
    CmHirLibraryBinding binding;
    CmHirLibraryOwnedData owned;
    size_t root_index;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult result;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context, "reserved"),
        CM_HIR_EDITION_2024, test_span(1u, 20u), &crate_id, &root_module)
        == CM_HIR_OK);
    root = cm_hir_get_module(&context, root_module);
    assert(root != NULL);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(3u, 8u), &function_definition)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_BOOL_KIND;
    type.span = test_span(4u, 5u);
    assert(cm_hir_add_type(&context, &type, &bool_type) == CM_HIR_OK);
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = bool_type;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    outlives.span = test_span(4u, 6u);
    memset(&value, 0, sizeof(value));
    value.definition = function_definition;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.return_type = bool_type;
    value.data.function.outlives_predicates = &outlives;
    value.data.function.outlives_predicate_count = 1u;
    value.data.function.abi = cm_hir_intern(&context, "Rust");
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned, root->definition,
        &root_index) == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&owned, &value)
        == CM_HIR_LIBRARY_OK);
    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
    binding.definition = function_definition;
    binding.value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"future", 6u, &binding)
        == CM_HIR_LIBRARY_OK);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_library_artifact_restore_owned(&artifact, &context,
        crate_id, root->definition, "reserved", &owned);
    assert(result.status == CM_HIR_LIBRARY_INVALID_HIR
        && owned.values.len == 1u && owned.modules.len == 1u);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_library_owned_data_destroy(&owned);
    cm_hir_context_destroy(&context);
}

static void test_reserved_parameter_outlives_value_accepted(void)
{
    CmHirContext context;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    const CmHirModule *root;
    CmHirDefId function_definition;
    CmHirGenericParam generic;
    CmHirGenericParamId generic_id;
    CmHirType type;
    CmHirTypeId parameter_type;
    CmHirOutlivesPredicate outlives;
    CmHirLibraryValue value;
    CmHirLibraryBinding binding;
    CmHirLibraryOwnedData owned;
    size_t root_index;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactResult result;

    cm_hir_context_init(&context);
    assert(cm_hir_create_crate(&context, cm_hir_intern(&context, "reserved"),
        CM_HIR_EDITION_2024, test_span(1u, 20u), &crate_id, &root_module)
        == CM_HIR_OK);
    root = cm_hir_get_module(&context, root_module);
    assert(root != NULL);
    assert(cm_hir_reserve_item_definition_as(&context, crate_id,
        CM_HIR_ITEM_FUNCTION, test_span(3u, 12u), &function_definition)
        == CM_HIR_OK);
    memset(&generic, 0, sizeof(generic));
    generic.kind = CM_HIR_GENERIC_TYPE;
    generic.owner = function_definition;
    generic.name = cm_hir_intern(&context, "T");
    generic.span = test_span(4u, 5u);
    assert(cm_hir_add_generic_param(&context, &generic, &generic_id)
        == CM_HIR_OK);
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = test_span(5u, 6u);
    type.data.parameter_type.parameter = generic_id;
    assert(cm_hir_add_type(&context, &type, &parameter_type) == CM_HIR_OK);
    memset(&outlives, 0, sizeof(outlives));
    outlives.subject_kind = CM_HIR_OUTLIVES_TYPE;
    outlives.subject.type = parameter_type;
    outlives.bound.kind = CM_HIR_REGION_STATIC;
    outlives.span = test_span(5u, 8u);
    memset(&value, 0, sizeof(value));
    value.definition = function_definition;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.return_type = parameter_type;
    value.data.function.generic_parameter_start = generic_id;
    value.data.function.generic_parameter_count = 1u;
    value.data.function.outlives_predicates = &outlives;
    value.data.function.outlives_predicate_count = 1u;
    value.data.function.abi = cm_hir_intern(&context, "Rust");
    cm_hir_library_owned_data_init(&owned);
    assert(cm_hir_library_owned_data_add_module(&owned, root->definition,
        &root_index) == CM_HIR_LIBRARY_OK);
    assert(cm_hir_library_owned_data_add_value(&owned, &value)
        == CM_HIR_LIBRARY_OK);
    memset(&binding, 0, sizeof(binding));
    binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
    binding.definition = function_definition;
    binding.value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    assert(cm_hir_library_owned_data_add_entry(&owned, root_index,
        (const unsigned char *)"future", 6u, &binding)
        == CM_HIR_LIBRARY_OK);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_library_artifact_restore_owned(&artifact, &context,
        crate_id, root->definition, "reserved", &owned);
    assert(result.status == CM_HIR_LIBRARY_OK && owned.values.len == 0u
        && owned.modules.len == 0u);
    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_library_owned_data_destroy(&owned);
    cm_hir_context_destroy(&context);
}

int main(void)
{
    test_owned_restore_is_transactional();
    test_struct_constructor_restore();
    test_struct_constructor_direct_capture();
    test_enum_variant_restore_scope();
    test_enum_variant_direct_capture_and_public_reexport_rejection();
    test_owned_predicate_copy_and_equality();
    test_reserved_predicate_value_rejected();
    test_reserved_parameter_outlives_value_accepted();
    return 0;
}
