#include "../../src/hir/library_internal.h"

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

int main(void)
{
    test_owned_restore_is_transactional();
    return 0;
}
