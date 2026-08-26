#include "cm/hir/declaration_capture.h"

#include "cm/alloc.h"
#include "library_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmDeclCaptureModule {
    CmResolveModuleInfo graph;
    CmHirModuleId hir_id;
    const CmHirModule *hir;
    unsigned char *path;
    size_t path_length;
    uint32_t local;
} CmDeclCaptureModule;

typedef struct CmDeclCaptureNamespace {
    uint32_t owner_module;
    uint8_t namespace_kind;
    unsigned char *name;
    size_t name_length;
    CmHirLibraryBinding target;
    CmResolveItemRef declaration;
    CmResolveItemRef introduced_by;
    CmAstItemKind item_kind;
    uint32_t export_ordinal;
    uint32_t source_attribute_count;
    CmSpan introduction_span;
    int source_is_generated;
    int is_import;
} CmDeclCaptureNamespace;

typedef struct CmDeclCaptureItem {
    const CmHirItem *item;
    CmHirItemId id;
    uint32_t owner_module;
    uint32_t source_ordinal;
    uint32_t local;
    uint8_t aggregate_repr;
    uint8_t enum_repr;
    uint8_t has_static_outlives;
    uint8_t trait_flags;
    uint16_t aggregate_flags;
    const unsigned char *lang_item;
    size_t lang_item_length;
    const unsigned char *diagnostic_item;
    size_t diagnostic_item_length;
    uint32_t associated_start;
    uint32_t associated_count;
} CmDeclCaptureItem;

typedef struct CmDeclTypeCandidate {
    CmHirTypeId id;
    uint32_t depth;
    uint8_t kind;
} CmDeclTypeCandidate;

typedef struct CmDeclTraitLocalPair {
    CmHirDefId definition;
    uint32_t local;
} CmDeclTraitLocalPair;

typedef struct CmDeclCaptureState {
    const CmHirDeclarationCaptureInput *input;
    const CmHirContext *hir;
    const CmHirCrate *crate_value;
    const CmHirLibraryOwnedData *owned;
    CmDeclCaptureModule *modules;
    size_t module_count;
    CmDeclCaptureNamespace *namespace_values;
    size_t namespace_count;
    size_t namespace_capacity;
    CmDeclCaptureItem *traits;
    size_t trait_count;
    CmDeclCaptureItem *associated_items;
    size_t associated_count;
    CmDeclCaptureItem *items;
    size_t item_count;
    size_t item_capacity;
    CmDeclCaptureItem *values;
    size_t value_count;
    size_t projected_semantic_attribute_count;
    uint32_t *generic_locals;
    unsigned char primitive_types[CM_HIR_DECL_PRIMITIVE_F64 + 1u];
    unsigned char *self_types;
    unsigned char *generic_types;
    unsigned char *named_item_types;
    unsigned char *application_types;
    unsigned char *compound_types;
    unsigned char *type_visits;
    uint32_t *type_depths;
    uint32_t *canonical_type_locals;
    uint32_t primitive_type_locals[CM_HIR_DECL_PRIMITIVE_F64 + 1u];
    uint32_t *generic_type_locals;
    uint32_t *named_type_locals;
    uint32_t *self_type_locals;
    uint32_t *item_locals_by_hir_id;
    CmDeclTraitLocalPair *trait_local_pairs;
} CmDeclCaptureState;

static CmHirDeclarationCaptureResult cm_decl_capture_result(
    CmHirDeclarationCaptureStatus status)
{
    CmHirDeclarationCaptureResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.metadata_status = CM_HIR_DECL_METADATA_OK;
    result.library_status = CM_HIR_LIBRARY_OK;
    return result;
}

static int cm_decl_capture_fail(CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureStage stage,
    CmHirDeclarationCaptureReason reason)
{
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = stage;
        result->failure_reason = reason;
    }
    return 0;
}

static void cm_decl_capture_binding_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason, const CmResolvedBinding *binding,
    const CmHirLibraryBinding *target, CmResolveItemRef source_item,
    const CmResolveEffectiveItem *effective)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE;
    result->failure_reason = reason;
    result->has_rejected_binding = 1;
    result->rejected_ast_item_kind = binding->item_kind;
    result->rejected_namespace_kind = binding->namespace_kind;
    result->rejected_source_item = source_item;
    if (target != NULL) {
        result->has_rejected_target = 1;
        result->rejected_binding_kind = target->kind;
        result->rejected_definition = target->definition;
    }
    if (effective != NULL) {
        result->has_rejected_span = 1;
        result->rejected_span = effective->span;
    }
}

static void cm_decl_capture_item_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason, const CmDeclCaptureNamespace *entry,
    const CmHirItem *item, CmHirItemId item_id)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_ITEMS;
    result->failure_reason = reason;
    result->rejected_item = item_id;
    if (entry != NULL) {
        result->has_rejected_binding = 1;
        result->has_rejected_target = 1;
        result->rejected_binding_kind = entry->target.kind;
        result->rejected_ast_item_kind = entry->item_kind;
        result->rejected_namespace_kind = entry->namespace_kind
                == CM_HIR_DECL_NAMESPACE_TYPE
            ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
        result->rejected_definition = entry->target.definition;
        result->rejected_source_item = entry->introduced_by;
    }
    if (item != NULL) {
        result->has_rejected_span = 1;
        result->rejected_span = item->span;
    }
}

static int cm_decl_capture_reexport_failure(
    CmHirDeclarationCaptureResult *result,
    CmHirDeclarationCaptureReason reason,
    const CmDeclCaptureNamespace *entry, CmSpan span)
{
    if (result->failure_reason != CM_HIR_DECL_CAPTURE_REASON_NONE) return 0;
    result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE;
    result->failure_reason = reason;
    if (entry != NULL) {
        result->has_rejected_binding = 1;
        result->has_rejected_target = 1;
        result->rejected_binding_kind = entry->target.kind;
        result->rejected_ast_item_kind = entry->item_kind;
        result->rejected_namespace_kind = entry->namespace_kind
                == CM_HIR_DECL_NAMESPACE_TYPE
            ? CM_RESOLVE_NAMESPACE_TYPE : CM_RESOLVE_NAMESPACE_VALUE;
        result->rejected_definition = entry->target.definition;
        result->rejected_source_item = entry->introduced_by;
    }
    if (span.source == 0u && entry != NULL)
        span = entry->introduction_span;
    if (span.source != 0u) {
        result->has_rejected_span = 1;
        result->rejected_span = span;
    }
    return 0;
}

static int cm_decl_bytes_equal(const unsigned char *left, size_t left_length,
    const unsigned char *right, size_t right_length)
{
    return left_length == right_length
        && (left_length == 0u || memcmp(left, right, left_length) == 0);
}

static int cm_decl_bytes_compare(const unsigned char *left,
    size_t left_length, const unsigned char *right, size_t right_length)
{
    size_t common;
    int order;
    common = left_length < right_length ? left_length : right_length;
    order = common == 0u ? 0 : memcmp(left, right, common);
    if (order != 0) return order;
    return left_length < right_length ? -1 : left_length > right_length;
}

static int cm_decl_target_read_u32(const unsigned char *bytes, size_t length,
    size_t *position, uint32_t *out)
{
    size_t offset;
    if (bytes == NULL || position == NULL || out == NULL
        || *position > length || length - *position < 4u) return 0;
    offset = *position;
    *out = ((uint32_t)bytes[offset] << 24)
        | ((uint32_t)bytes[offset + 1u] << 16)
        | ((uint32_t)bytes[offset + 2u] << 8)
        | (uint32_t)bytes[offset + 3u];
    *position += 4u;
    return 1;
}

static int cm_decl_target_read_string(const unsigned char *bytes,
    size_t length, size_t *position, const unsigned char **out_bytes,
    size_t *out_length)
{
    uint32_t string_length;
    if (!cm_decl_target_read_u32(bytes, length, position, &string_length)
        || *position > length
        || (size_t)string_length > length - *position) return 0;
    *out_bytes = bytes + *position;
    *out_length = (size_t)string_length;
    *position += (size_t)string_length;
    return 1;
}

static unsigned char cm_decl_target_rendered_byte(size_t position,
    const unsigned char *name, size_t name_length,
    const unsigned char *value, size_t value_length, int has_value)
{
    if (position < name_length) return name[position];
    position -= name_length;
    if (!has_value) return 0u;
    if (position == 0u) return (unsigned char)'=';
    if (position == 1u) return (unsigned char)'"';
    position -= 2u;
    if (position < value_length) return value[position];
    return (unsigned char)'"';
}

static int cm_decl_target_rendered_compare(const unsigned char *bytes,
    size_t length, const unsigned char *name, size_t name_length,
    const unsigned char *value, size_t value_length, int has_value,
    size_t expected_length)
{
    size_t common = length < expected_length ? length : expected_length;
    size_t position;
    for (position = 0u; position < common; ++position) {
        unsigned char expected = cm_decl_target_rendered_byte(position,
            name, name_length, value, value_length, has_value);
        if (bytes[position] != expected)
            return bytes[position] < expected ? -1 : 1;
    }
    return length < expected_length ? -1 : length > expected_length;
}

static int cm_decl_target_cfg_has_rendered(
    const CmHirArtifactConfig *config, const unsigned char *name,
    size_t name_length, const unsigned char *value, size_t value_length,
    int has_value)
{
    size_t expected_length;
    size_t lower;
    size_t upper;
    size_t index;
    if (config == NULL || name == NULL || name_length == 0u
        || config->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || (config->cfg_count != 0u && config->cfgs == NULL)
        || (has_value && value == NULL)
        || !cm_size_add(name_length, has_value ? 3u : 0u,
            &expected_length)
        || !cm_size_add(expected_length, has_value ? value_length : 0u,
            &expected_length)) return 0;
    if (has_value) {
        for (index = 0u; index < value_length; ++index) {
            unsigned char byte = value[index];
            if (byte < UINT8_C(0x20) || byte > UINT8_C(0x7e)
                || byte == (unsigned char)'"'
                || byte == (unsigned char)'\\') return 0;
        }
    }
    lower = 0u;
    upper = config->cfg_count;
    while (lower < upper) {
        size_t middle = lower + (upper - lower) / 2u;
        const CmHirArtifactBytes *cfg = &config->cfgs[middle];
        const unsigned char *cfg_bytes =
            (const unsigned char *)cfg->data;
        int comparison;
        if (cfg_bytes == NULL) return 0;
        comparison = cm_decl_target_rendered_compare(cfg_bytes, cfg->length,
            name, name_length, value, value_length, has_value,
            expected_length);
        if (comparison < 0) lower = middle + 1u;
        else upper = middle;
    }
    if (lower == config->cfg_count
        || config->cfgs[lower].data == NULL) return 0;
    return cm_decl_target_rendered_compare(
        (const unsigned char *)config->cfgs[lower].data,
        config->cfgs[lower].length, name, name_length, value, value_length,
        has_value, expected_length) == 0;
}

static int cm_decl_target_descriptor_matches(
    const CmHirDeclarationCaptureInput *input)
{
    static const unsigned char header[] = "cmrustc-target-v1";
    static const char *const cfg_fields[7] = {
        NULL, "target_arch", "target_os", "target_env", "target_abi",
        "target_vendor", "target_family"
    };
    static const unsigned char target_feature[] = "target_feature";
    static const unsigned char target_endian[] = "target_endian";
    static const unsigned char little[] = "little";
    static const unsigned char big[] = "big";
    const CmHirArtifactConfig *config = input->configuration;
    const unsigned char *bytes;
    const unsigned char *field;
    const unsigned char *prior = NULL;
    const unsigned char *prior_value = NULL;
    size_t length;
    size_t field_length;
    const unsigned char *family = NULL;
    size_t family_length = 0u;
    size_t prior_length = 0u;
    size_t prior_value_length = 0u;
    size_t position;
    uint32_t count;
    uint32_t pointer_bits;
    uint32_t index;
    unsigned char prior_has_value = 0u;
    if (config->target_descriptor.data == NULL
        || config->target_descriptor.data != config->descriptor_storage.data
        || config->target_descriptor.length != config->descriptor_storage.len
        || config->target_descriptor.length < sizeof(header)
        || memcmp(config->target_descriptor.data, header,
            sizeof(header)) != 0) return 0;
    bytes = config->target_descriptor.data;
    length = config->target_descriptor.length;
    position = sizeof(header);
    for (index = 0u; index < 7u; ++index) {
        if (!cm_decl_target_read_string(bytes, length, &position, &field,
                &field_length)
            || (index == 0u
                && !cm_decl_bytes_equal(field, field_length,
                    input->target_triple.data,
                    input->target_triple.length))
            || (index != 0u && !cm_decl_target_cfg_has_rendered(config,
                (const unsigned char *)cfg_fields[index],
                strlen(cfg_fields[index]), field, field_length, 1))) return 0;
        if (index == 6u) {
            family = field;
            family_length = field_length;
        }
    }
    if (!cm_decl_target_read_u32(bytes, length, &position, &pointer_bits)
        || pointer_bits != input->target_pointer_bits
        || position >= length || bytes[position] > UINT8_C(1)) return 0;
    if (!cm_decl_target_cfg_has_rendered(config, target_endian,
            sizeof(target_endian) - 1u,
            bytes[position] == 0u ? little : big,
            bytes[position] == 0u ? sizeof(little) - 1u : sizeof(big) - 1u,
            1)
        || family == NULL || family_length == 0u
        || !cm_decl_target_cfg_has_rendered(config, family, family_length,
            NULL, 0u, 0)) return 0;
    position += 1u;
    if (!cm_decl_target_read_u32(bytes, length, &position, &count)
        || count > CM_HIR_ARTIFACT_MAX_CFG_COUNT) return 0;
    for (index = 0u; index < count; ++index) {
        if (!cm_decl_target_read_string(bytes, length, &position, &field,
                &field_length)
            || field_length == 0u
            || (prior != NULL && cm_decl_bytes_compare(prior, prior_length,
                field, field_length) >= 0)
            || !cm_decl_target_cfg_has_rendered(config, target_feature,
                sizeof(target_feature) - 1u, field, field_length, 1)) return 0;
        prior = field;
        prior_length = field_length;
    }
    if (!cm_decl_target_read_u32(bytes, length, &position, &count)
        || count > CM_HIR_ARTIFACT_MAX_CFG_COUNT) return 0;
    prior = NULL;
    prior_length = 0u;
    for (index = 0u; index < count; ++index) {
        const unsigned char *value = NULL;
        size_t value_length = 0u;
        unsigned char has_value;
        int comparison;
        if (!cm_decl_target_read_string(bytes, length, &position, &field,
                &field_length)
            || field_length == 0u || position >= length) return 0;
        has_value = bytes[position++];
        if (has_value > UINT8_C(1)) return 0;
        if (has_value != 0u
            && !cm_decl_target_read_string(bytes, length, &position, &value,
                &value_length)) return 0;
        if (!cm_decl_target_cfg_has_rendered(config, field, field_length,
                value, value_length, has_value != 0u)) return 0;
        comparison = prior == NULL ? -1 : cm_decl_bytes_compare(prior,
            prior_length, field, field_length);
        if (prior != NULL && comparison == 0) {
            comparison = prior_has_value < has_value ? -1
                : prior_has_value > has_value ? 1
                : has_value == 0u ? 0
                : cm_decl_bytes_compare(prior_value, prior_value_length,
                    value, value_length);
        }
        if (prior != NULL && comparison >= 0) return 0;
        prior = field;
        prior_length = field_length;
        prior_has_value = has_value;
        prior_value = value;
        prior_value_length = value_length;
    }
    return position == length;
}

static int cm_decl_target_cfg_matches(
    const CmHirDeclarationCaptureInput *input)
{
    static const unsigned char width32[] =
        "target_pointer_width=\"32\"";
    static const unsigned char width64[] =
        "target_pointer_width=\"64\"";
    const CmHirArtifactConfig *config = input->configuration;
    const unsigned char *expected = input->target_pointer_bits == 32u
        ? width32 : width64;
    size_t expected_length = input->target_pointer_bits == 32u
        ? sizeof(width32) - 1u : sizeof(width64) - 1u;
    size_t storage_offset = 0u;
    size_t matches = 0u;
    size_t index;
    if ((config->cfg_count == 0u) != (config->cfgs == NULL)
        || config->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || config->cfg_storage.len == 0u
        || config->cfg_storage.data == NULL) return 0;
    for (index = 0u; index < config->cfg_count; ++index) {
        const CmHirArtifactBytes *cfg = &config->cfgs[index];
        if (cfg->data == NULL || cfg->length == 0u
            || storage_offset > config->cfg_storage.len
            || cfg->length > config->cfg_storage.len - storage_offset
            || cfg->data != config->cfg_storage.data + storage_offset
            || (index != 0u && cm_decl_bytes_compare(
                config->cfgs[index - 1u].data,
                config->cfgs[index - 1u].length,
                cfg->data, cfg->length) >= 0)) return 0;
        storage_offset += cfg->length;
        if (cm_decl_bytes_equal(cfg->data, cfg->length, expected,
                expected_length)) matches += 1u;
    }
    return storage_offset == config->cfg_storage.len && matches == 1u;
}

static int cm_decl_target_configuration_matches(
    const CmHirDeclarationCaptureInput *input)
{
    return input != NULL && input->configuration != NULL
        && (input->target_pointer_bits == 32u
            || input->target_pointer_bits == 64u)
        && cm_decl_target_descriptor_matches(input)
        && cm_decl_target_cfg_matches(input);
}

static int cm_decl_copy_bytes(CmHirDeclarationString *out,
    const void *bytes, size_t length)
{
    out->data = length == 0u ? NULL : (unsigned char *)cm_alloc(length);
    out->length = length;
    if (length != 0u) memcpy(out->data, bytes, length);
    return 1;
}

static int cm_decl_copy_intern(CmHirDeclarationString *out,
    const CmInternedString *value)
{
    return value != NULL && value->len != 0u
        && cm_decl_copy_bytes(out, value->bytes, value->len);
}

static int cm_decl_copy_graph_string(const CmModuleGraph *graph,
    CmResolveStringId id, unsigned char **out, size_t *out_length)
{
    size_t length;
    unsigned char *bytes;
    *out = NULL;
    *out_length = 0u;
    length = cm_module_graph_string_length(graph, id);
    if (length == 0u || length == SIZE_MAX) return 0;
    bytes = (unsigned char *)cm_alloc(length + 1u);
    if (!cm_module_graph_copy_string(graph, id, (char *)bytes, length + 1u)) {
        cm_free(bytes);
        return 0;
    }
    *out = bytes;
    *out_length = length;
    return 1;
}

static int cm_decl_copy_import_string(const CmImportResolver *imports,
    CmResolveStringId id, unsigned char **out, size_t *out_length)
{
    size_t length;
    unsigned char *bytes;
    *out = NULL;
    *out_length = 0u;
    length = cm_import_string_length(imports, id);
    if (length == 0u || length == SIZE_MAX) return 0;
    bytes = (unsigned char *)cm_alloc(length + 1u);
    if (!cm_import_copy_string(imports, id, (char *)bytes, length + 1u)) {
        cm_free(bytes);
        return 0;
    }
    *out = bytes;
    *out_length = length;
    return 1;
}

static int cm_decl_module_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclCaptureModule *left =
        (const CmDeclCaptureModule *)left_value;
    const CmDeclCaptureModule *right =
        (const CmDeclCaptureModule *)right_value;
    return cm_decl_bytes_compare(left->path, left->path_length,
        right->path, right->path_length);
}

static int cm_decl_namespace_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclCaptureNamespace *left =
        (const CmDeclCaptureNamespace *)left_value;
    const CmDeclCaptureNamespace *right =
        (const CmDeclCaptureNamespace *)right_value;
    int order;
    if (left->owner_module != right->owner_module)
        return left->owner_module < right->owner_module ? -1 : 1;
    if (left->namespace_kind != right->namespace_kind)
        return left->namespace_kind < right->namespace_kind ? -1 : 1;
    order = cm_decl_bytes_compare(left->name, left->name_length,
        right->name, right->name_length);
    if (order != 0) return order;
    return left->export_ordinal < right->export_ordinal ? -1
        : left->export_ordinal > right->export_ordinal;
}

static const CmInternedString *cm_decl_item_name(
    const CmDeclCaptureState *state, const CmHirItem *item)
{
    return item == NULL ? NULL
        : cm_interner_get(&state->hir->strings, item->name);
}

static int cm_decl_item_compare(const void *left_value,
    const void *right_value, const CmDeclCaptureState *state)
{
    const CmDeclCaptureItem *left = (const CmDeclCaptureItem *)left_value;
    const CmDeclCaptureItem *right = (const CmDeclCaptureItem *)right_value;
    const CmInternedString *left_name = cm_decl_item_name(state, left->item);
    const CmInternedString *right_name = cm_decl_item_name(state, right->item);
    int order;
    if (left->owner_module != right->owner_module)
        return left->owner_module < right->owner_module ? -1 : 1;
    order = cm_decl_bytes_compare(left_name->bytes, left_name->len,
        right_name->bytes, right_name->len);
    if (order != 0) return order;
    return left->source_ordinal < right->source_ordinal ? -1
        : left->source_ordinal > right->source_ordinal;
}

static void cm_decl_sort_items(CmDeclCaptureItem *items, size_t count,
    const CmDeclCaptureState *state)
{
    size_t index;
    for (index = 1u; index < count; ++index) {
        CmDeclCaptureItem value = items[index];
        size_t cursor = index;
        while (cursor != 0u
            && cm_decl_item_compare(&items[cursor - 1u], &value,
                state) > 0) {
            items[cursor] = items[cursor - 1u];
            cursor -= 1u;
        }
        items[cursor] = value;
    }
}

static CmDeclCaptureModule *cm_decl_module_by_graph(CmDeclCaptureState *state,
    CmModuleId id)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].graph.id == id) return &state->modules[index];
    return NULL;
}

static CmDeclCaptureModule *cm_decl_module_by_definition(
    CmDeclCaptureState *state, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (cm_hir_def_id_equal(state->modules[index].hir->definition,
                definition)) return &state->modules[index];
    return NULL;
}

static CmDeclCaptureModule *cm_decl_module_by_local(
    CmDeclCaptureState *state, uint32_t local)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].local == local) return &state->modules[index];
    return NULL;
}

static CmDeclCaptureModule *cm_decl_module_by_hir(
    CmDeclCaptureState *state, CmHirModuleId id)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].hir_id == id) return &state->modules[index];
    return NULL;
}

static int cm_decl_project_module_attributes(CmDeclCaptureState *state,
    const CmHirAttribute *attributes, uint32_t attribute_count,
    CmHirDeclarationCaptureResult *result)
{
    uint32_t index;
    if ((attribute_count == 0u) != (attributes == NULL))
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
    if ((size_t)attribute_count > SIZE_MAX
            - state->projected_semantic_attribute_count)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
    for (index = 0u; index < attribute_count; ++index) {
        const CmHirAttribute *attribute = &attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        uint32_t prior;
        if (metadata == NULL || metadata->len == 0u
            || attribute->source_attribute == 0u
            || attribute->span.source == 0u
            || attribute->span.start > attribute->span.end) {
            if (result->failure_reason
                    == CM_HIR_DECL_CAPTURE_REASON_NONE) {
                result->has_rejected_span = attribute->span.source != 0u;
                result->rejected_span = attribute->span;
            }
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_MODULES,
                CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
        }
        for (prior = 0u; prior < index; ++prior) {
            if (attributes[prior].span.source == attribute->span.source
                && attributes[prior].source_attribute
                    == attribute->source_attribute) {
                result->has_rejected_span = 1;
                result->rejected_span = attribute->span;
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_MODULES,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID);
            }
        }
    }
    state->projected_semantic_attribute_count += attribute_count;
    return 1;
}

static int cm_decl_collect_modules(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    const CmModuleGraph *graph = state->input->graph;
    size_t index;
    if (!cm_decl_project_module_attributes(state,
            state->crate_value->inner_attributes,
            state->crate_value->inner_attribute_count, result)) return 0;
    state->module_count = cm_module_graph_module_count(graph);
    if (state->module_count == 0u
        || state->module_count > CM_HIR_DECL_METADATA_MAX_MODULES
        || cm_hir_module_map_count(state->input->modules)
            != state->module_count) return 0;
    state->modules = (CmDeclCaptureModule *)cm_alloc_zeroed(
        state->module_count, sizeof(*state->modules));
    for (index = 0u; index < state->module_count; ++index) {
        CmDeclCaptureModule *module = &state->modules[index];
        if (!cm_module_graph_get_module_at(graph, index, &module->graph)
            || cm_hir_module_map_lookup_hir(state->input->modules, graph,
                state->input->revision, module->graph.id, state->hir,
                &module->hir_id) != CM_HIR_MODULE_MAP_OK
            || (module->hir = cm_hir_get_module(state->hir,
                module->hir_id)) == NULL
            || module->hir->crate_id != state->input->crate_id
            || !cm_decl_copy_graph_string(graph, module->graph.absolute_path,
                &module->path, &module->path_length)) return 0;
        if (!cm_decl_project_module_attributes(state,
                module->hir->outer_attributes,
                module->hir->outer_attribute_count, result)
            || !cm_decl_project_module_attributes(state,
                module->hir->inner_attributes,
                module->hir->inner_attribute_count, result)) return 0;
    }
    qsort(state->modules, state->module_count, sizeof(*state->modules),
        cm_decl_module_compare);
    for (index = 0u; index < state->module_count; ++index) {
        CmDeclCaptureModule *module = &state->modules[index];
        CmDeclCaptureModule *parent;
        module->local = (uint32_t)(index + 1u);
        if (module->graph.parent == CM_MODULE_NONE) {
            if (module->hir_id != state->crate_value->root_module) return 0;
        } else {
            parent = cm_decl_module_by_graph(state, module->graph.parent);
            if (parent == NULL || parent->local == 0u
                || parent->local >= module->local
                || module->hir->parent != parent->hir_id) return 0;
        }
    }
    return 1;
}

static const CmHirLibraryOwnedModule *cm_decl_owned_module(
    const CmHirLibraryOwnedData *owned, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < owned->modules.len; ++index) {
        const CmHirLibraryOwnedModule *module =
            (const CmHirLibraryOwnedModule *)cm_vec_at_const(
                &owned->modules, index);
        if (module != NULL && cm_hir_def_id_equal(module->definition,
                definition)) return module;
    }
    return NULL;
}

static const CmHirLibraryOwnedValue *cm_decl_owned_value(
    const CmHirLibraryOwnedData *owned, CmHirDefId definition)
{
    size_t index;
    if (owned == NULL) return NULL;
    for (index = 0u; index < owned->values.len; ++index) {
        const CmHirLibraryOwnedValue *value =
            (const CmHirLibraryOwnedValue *)cm_vec_at_const(
                &owned->values, index);
        if (value != NULL && cm_hir_def_id_equal(
                value->declaration.definition, definition)) return value;
    }
    return NULL;
}

static int cm_decl_library_binding(const CmHirLibraryOwnedData *owned,
    const CmHirLibraryOwnedModule *module, int value_namespace,
    const unsigned char *name, size_t name_length,
    CmHirLibraryBinding *out)
{
    size_t index;
    size_t matches = 0u;
    memset(out, 0, sizeof(*out));
    for (index = 0u; index < module->entries.len; ++index) {
        const CmHirLibraryOwnedEntry *entry =
            (const CmHirLibraryOwnedEntry *)cm_vec_at_const(
                &module->entries, index);
        const CmInternedString *entry_name = entry == NULL ? NULL
            : cm_interner_get(&owned->names, entry->name);
        int entry_value = entry != NULL
            && (entry->kind == CM_HIR_LIBRARY_BINDING_VALUE
                || entry->kind
                    == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
                || (entry->kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
                    && entry->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE));
        if (entry_name != NULL && entry_value == value_namespace
            && cm_decl_bytes_equal(entry_name->bytes, entry_name->len,
                name, name_length)) {
            out->kind = entry->kind;
            out->definition = entry->target;
            out->type_kind = entry->type_kind;
            out->primitive_kind = entry->primitive_kind;
            out->value_kind = entry->value_kind;
            out->enum_definition = entry->enum_definition;
            out->enum_variant_index = entry->enum_variant_index;
            out->enum_variant_form = entry->enum_variant_form;
            out->enum_variant_namespace = entry->enum_variant_namespace;
            matches += 1u;
        }
    }
    return matches == 1u;
}

static int cm_decl_item_ref_equal(CmResolveItemRef left,
    CmResolveItemRef right)
{
    return left.source == right.source && left.item == right.item;
}

static int cm_decl_effective_ordinal(const CmDeclCaptureState *state,
    CmModuleId module, CmResolveItemRef declaration, uint32_t *out_ordinal,
    CmResolveEffectiveItem *out_effective)
{
    CmResolveModuleInfo information;
    uint32_t index;
    size_t matches = 0u;
    if (!cm_module_graph_get_module(state->input->graph, module,
            &information)) return 0;
    for (index = 0u; index < information.effective_item_count; ++index) {
        CmResolveEffectiveItem effective;
        if (cm_module_graph_get_effective_item(state->input->graph,
                state->input->revision, module, index, &effective)
                != CM_RESOLVE_VIEW_OK) return 0;
        if (cm_decl_item_ref_equal(effective.declaration, declaration)) {
            if (out_effective != NULL) *out_effective = effective;
            *out_ordinal = index;
            matches += 1u;
        }
    }
    return matches == 1u;
}

static uint8_t cm_decl_resolver_primitive(
    CmResolvePrimitiveKind primitive)
{
    switch (primitive) {
    case CM_RESOLVE_PRIMITIVE_BOOL: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_RESOLVE_PRIMITIVE_CHAR: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_RESOLVE_PRIMITIVE_STR: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_RESOLVE_PRIMITIVE_I8: return CM_HIR_DECL_PRIMITIVE_I8;
    case CM_RESOLVE_PRIMITIVE_I16: return CM_HIR_DECL_PRIMITIVE_I16;
    case CM_RESOLVE_PRIMITIVE_I32: return CM_HIR_DECL_PRIMITIVE_I32;
    case CM_RESOLVE_PRIMITIVE_I64: return CM_HIR_DECL_PRIMITIVE_I64;
    case CM_RESOLVE_PRIMITIVE_I128: return CM_HIR_DECL_PRIMITIVE_I128;
    case CM_RESOLVE_PRIMITIVE_ISIZE: return CM_HIR_DECL_PRIMITIVE_ISIZE;
    case CM_RESOLVE_PRIMITIVE_U8: return CM_HIR_DECL_PRIMITIVE_U8;
    case CM_RESOLVE_PRIMITIVE_U16: return CM_HIR_DECL_PRIMITIVE_U16;
    case CM_RESOLVE_PRIMITIVE_U32: return CM_HIR_DECL_PRIMITIVE_U32;
    case CM_RESOLVE_PRIMITIVE_U64: return CM_HIR_DECL_PRIMITIVE_U64;
    case CM_RESOLVE_PRIMITIVE_U128: return CM_HIR_DECL_PRIMITIVE_U128;
    case CM_RESOLVE_PRIMITIVE_USIZE: return CM_HIR_DECL_PRIMITIVE_USIZE;
    case CM_RESOLVE_PRIMITIVE_F32: return CM_HIR_DECL_PRIMITIVE_F32;
    case CM_RESOLVE_PRIMITIVE_F64: return CM_HIR_DECL_PRIMITIVE_F64;
    default: return 0u;
    }
}

static uint8_t cm_decl_library_primitive(CmHirPrimitiveKind primitive)
{
    switch (primitive) {
    case CM_HIR_PRIMITIVE_BOOL: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_HIR_PRIMITIVE_CHAR: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_HIR_PRIMITIVE_STR: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_HIR_PRIMITIVE_I8: return CM_HIR_DECL_PRIMITIVE_I8;
    case CM_HIR_PRIMITIVE_I16: return CM_HIR_DECL_PRIMITIVE_I16;
    case CM_HIR_PRIMITIVE_I32: return CM_HIR_DECL_PRIMITIVE_I32;
    case CM_HIR_PRIMITIVE_I64: return CM_HIR_DECL_PRIMITIVE_I64;
    case CM_HIR_PRIMITIVE_I128: return CM_HIR_DECL_PRIMITIVE_I128;
    case CM_HIR_PRIMITIVE_ISIZE: return CM_HIR_DECL_PRIMITIVE_ISIZE;
    case CM_HIR_PRIMITIVE_U8: return CM_HIR_DECL_PRIMITIVE_U8;
    case CM_HIR_PRIMITIVE_U16: return CM_HIR_DECL_PRIMITIVE_U16;
    case CM_HIR_PRIMITIVE_U32: return CM_HIR_DECL_PRIMITIVE_U32;
    case CM_HIR_PRIMITIVE_U64: return CM_HIR_DECL_PRIMITIVE_U64;
    case CM_HIR_PRIMITIVE_U128: return CM_HIR_DECL_PRIMITIVE_U128;
    case CM_HIR_PRIMITIVE_USIZE: return CM_HIR_DECL_PRIMITIVE_USIZE;
    case CM_HIR_PRIMITIVE_F32: return CM_HIR_DECL_PRIMITIVE_F32;
    case CM_HIR_PRIMITIVE_F64: return CM_HIR_DECL_PRIMITIVE_F64;
    default: return 0u;
    }
}

static int cm_decl_namespace_target_shape(const CmResolvedBinding *binding,
    const CmHirLibraryBinding *target)
{
    if (target->kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
        uint8_t resolver_primitive = cm_decl_resolver_primitive(
            binding->primitive_kind);
        uint8_t library_primitive = cm_decl_library_primitive(
            target->primitive_kind);
        /* A builtin primitive has no AST declaration.  CmAstItemKind's zero
         * value happens to print as FUNCTION, so it is deliberately not an
         * authority for this already-authenticated synthetic binding. */
        return resolver_primitive != 0u
            && resolver_primitive == library_primitive
            && binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->target_module == CM_MODULE_NONE
            && binding->declaration.source == 0u
            && binding->declaration.item == CM_AST_ITEM_NONE
            && binding->variant.enumeration.source == 0u
            && binding->variant.enumeration.item == CM_AST_ITEM_NONE
            && binding->variant.index == 0u
            && cm_hir_def_id_is_none(target->definition)
            && target->type_kind == CM_HIR_TYPE_ERROR_KIND
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    }
    if (target->kind == CM_HIR_LIBRARY_BINDING_MODULE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_MODULE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_TRAIT)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && binding->item_kind == CM_AST_ITEM_TRAIT;
    if (target->kind == CM_HIR_LIBRARY_BINDING_VALUE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && ((binding->item_kind == CM_AST_ITEM_FUNCTION
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_FUNCTION)
                || (binding->item_kind == CM_AST_ITEM_CONST
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_CONST)
                || (binding->item_kind == CM_AST_ITEM_STATIC
                    && target->value_kind == CM_HIR_LIBRARY_VALUE_STATIC));
    if (target->kind == CM_HIR_LIBRARY_BINDING_TYPE)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && (binding->item_kind == CM_AST_ITEM_STRUCT
                || binding->item_kind == CM_AST_ITEM_UNION
                || binding->item_kind == CM_AST_ITEM_ENUM
                || binding->item_kind == CM_AST_ITEM_TYPE_ALIAS)
            && target->type_kind == (binding->item_kind == CM_AST_ITEM_STRUCT
                    || binding->item_kind == CM_AST_ITEM_UNION
                    || binding->item_kind == CM_AST_ITEM_ENUM
                ? CM_HIR_TYPE_ADT_KIND : CM_HIR_TYPE_ALIAS_APPLICATION_KIND)
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR)
        return binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
            && binding->item_kind == CM_AST_ITEM_STRUCT
            && target->type_kind == CM_HIR_TYPE_ADT_KIND
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE;
    if (target->kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT)
        return binding->item_kind == CM_AST_ITEM_ENUM
            && target->type_kind == CM_HIR_TYPE_ADT_KIND
            && target->primitive_kind == CM_HIR_PRIMITIVE_NONE
            && target->value_kind == CM_HIR_LIBRARY_VALUE_NONE
            && (target->enum_variant_form == CM_HIR_AGGREGATE_UNIT
                || target->enum_variant_form == CM_HIR_AGGREGATE_TUPLE)
            && ((binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
                    && target->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_TYPE)
                || (binding->namespace_kind == CM_RESOLVE_NAMESPACE_VALUE
                    && target->enum_variant_namespace
                        == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE));
    return 0;
}

static int cm_decl_primitive_reexport_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureNamespace *entry);

static const CmHirItem *cm_decl_enum_variant_parent(
    const CmDeclCaptureState *state, const CmHirLibraryBinding *target,
    CmHirItemId *out_item_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;
    const CmHirVariant *variant;
    if (target == NULL
        || target->kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        || cm_hir_def_id_is_none(target->definition)
        || cm_hir_def_id_is_none(target->enum_definition)) return NULL;
    definition = cm_hir_lookup_definition(state->hir, target->definition);
    if (definition == NULL || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->kind != CM_HIR_DEFINITION_ENUM_VARIANT
        || definition->id.crate_id != state->input->crate_id
        || definition->entity.enum_variant.variant_index
            != target->enum_variant_index) return NULL;
    *out_item_id = definition->entity.enum_variant.enum_item_id;
    item = cm_hir_get_item(state->hir, *out_item_id);
    if (item == NULL || item->kind != CM_HIR_ITEM_ENUM
        || !cm_hir_def_id_equal(item->definition, target->enum_definition)
        || item->definition.crate_id != state->input->crate_id
        || target->enum_variant_index >= item->data.enum_item.variant_count
        || item->data.enum_item.variants == NULL) return NULL;
    variant = &item->data.enum_item.variants[target->enum_variant_index];
    return cm_hir_def_id_equal(variant->definition, target->definition)
            && variant->form == target->enum_variant_form
            && (variant->form == CM_HIR_AGGREGATE_UNIT
                || variant->form == CM_HIR_AGGREGATE_TUPLE)
        ? item : NULL;
}

static int cm_decl_enum_variant_mate(const CmDeclCaptureState *state,
    size_t entry_index)
{
    const CmDeclCaptureNamespace *entry =
        &state->namespace_values[entry_index];
    size_t index;
    size_t matches = 0u;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *mate = &state->namespace_values[index];
        if (mate->target.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
            && mate->owner_module == entry->owner_module
            && mate->namespace_kind != entry->namespace_kind
            && mate->export_ordinal == entry->export_ordinal
            && mate->source_attribute_count == entry->source_attribute_count
            && mate->source_is_generated == entry->source_is_generated
            && mate->is_import == entry->is_import
            && mate->item_kind == entry->item_kind
            && mate->introduction_span.source
                == entry->introduction_span.source
            && mate->introduction_span.start
                == entry->introduction_span.start
            && mate->introduction_span.end == entry->introduction_span.end
            && cm_hir_def_id_equal(mate->target.definition,
                entry->target.definition)
            && cm_hir_def_id_equal(mate->target.enum_definition,
                entry->target.enum_definition)
            && mate->target.enum_variant_index
                == entry->target.enum_variant_index
            && mate->target.enum_variant_form
                == entry->target.enum_variant_form
            && mate->target.enum_variant_namespace
                != entry->target.enum_variant_namespace
            && cm_decl_item_ref_equal(mate->declaration,
                entry->declaration)
            && cm_decl_item_ref_equal(mate->introduced_by,
                entry->introduced_by)
            && cm_decl_bytes_equal(mate->name, mate->name_length,
                entry->name, entry->name_length)) matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_collect_namespace(CmDeclCaptureState *state,
    const CmHirLibraryOwnedData *owned,
    CmHirDeclarationCaptureResult *result)
{
    size_t module_index;
    size_t capacity = 0u;
    size_t owned_entry_count = 0u;
    for (module_index = 0u; module_index < state->module_count;
            ++module_index) {
        const CmDeclCaptureModule *module = &state->modules[module_index];
        const CmHirLibraryOwnedModule *owned_module =
            cm_decl_owned_module(owned, module->hir->definition);
        int namespace_index;
        if (owned_module == NULL)
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_MODULE_MISSING);
        if (owned_module->entries.len > SIZE_MAX - owned_entry_count)
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
        owned_entry_count += owned_module->entries.len;
        for (namespace_index = 0; namespace_index < 2; ++namespace_index) {
            CmResolveNamespace namespace_kind =
                (CmResolveNamespace)namespace_index;
            size_t binding_count = cm_import_binding_count(
                state->input->imports, module->graph.id, namespace_kind);
            uint32_t binding_index;
            if (binding_count > (size_t)UINT32_MAX)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
            for (binding_index = 0u; (size_t)binding_index < binding_count;
                    ++binding_index) {
                CmResolvedBinding binding;
                if (!cm_import_get_binding(state->input->imports,
                        module->graph.id, namespace_kind, binding_index,
                        &binding))
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED);
                if (!binding.is_public) continue;
                if (capacity == CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
                capacity += 1u;
            }
        }
    }
    state->namespace_values = capacity == 0u ? NULL
        : (CmDeclCaptureNamespace *)cm_alloc_zeroed(capacity,
            sizeof(*state->namespace_values));
    state->namespace_capacity = capacity;
    for (module_index = 0u; module_index < state->module_count;
            ++module_index) {
        const CmDeclCaptureModule *module = &state->modules[module_index];
        const CmHirLibraryOwnedModule *owned_module =
            cm_decl_owned_module(owned, module->hir->definition);
        int namespace_index;
        for (namespace_index = 0; namespace_index < 2; ++namespace_index) {
            CmResolveNamespace namespace_kind =
                (CmResolveNamespace)namespace_index;
            size_t binding_count = cm_import_binding_count(
                state->input->imports, module->graph.id, namespace_kind);
            uint32_t binding_index;
            if (binding_count > (size_t)UINT32_MAX)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                    CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT);
            for (binding_index = 0u; (size_t)binding_index < binding_count;
                    ++binding_index) {
                CmResolvedBinding binding;
                CmDeclCaptureNamespace *entry;
                CmResolveItemRef introduced;
                CmResolveEffectiveItem effective;
                uint32_t effective_index = 0u;
                int has_effective;
                if (!cm_import_get_binding(state->input->imports,
                        module->graph.id, namespace_kind, binding_index,
                        &binding))
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED);
                if (!binding.is_public) continue;
                introduced = binding.is_import ? binding.import_declaration
                    : binding.declaration;
                has_effective = introduced.source != 0u
                    && introduced.item != CM_AST_ITEM_NONE
                    && cm_decl_effective_ordinal(state, module->graph.id,
                        introduced, &effective_index, &effective);
                if (binding.is_ambiguous || binding.is_anonymous
                    || binding.revision != state->input->revision
                    || binding.module != module->graph.id) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_AUTHORITY_INVALID,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (state->namespace_count >= capacity) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                entry = &state->namespace_values[state->namespace_count];
                if (!cm_decl_copy_import_string(state->input->imports,
                        binding.name, &entry->name, &entry->name_length)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_NAME_INVALID,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (!cm_decl_library_binding(owned, owned_module,
                        namespace_kind == CM_RESOLVE_NAMESPACE_VALUE,
                        entry->name, entry->name_length, &entry->target)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_LIBRARY_MISMATCH,
                        &binding, NULL, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (!cm_decl_namespace_target_shape(&binding,
                        &entry->target)) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                        &binding, &entry->target, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                if (introduced.source == 0u
                    || introduced.item == CM_AST_ITEM_NONE
                    || !has_effective) {
                    cm_decl_capture_binding_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_BINDING_INTRODUCTION_INVALID,
                        &binding, &entry->target, introduced,
                        has_effective ? &effective : NULL);
                    return 0;
                }
                entry->owner_module = module->local;
                entry->namespace_kind = namespace_kind
                        == CM_RESOLVE_NAMESPACE_TYPE
                    ? CM_HIR_DECL_NAMESPACE_TYPE
                    : CM_HIR_DECL_NAMESPACE_VALUE;
                entry->declaration = binding.declaration;
                entry->introduced_by = introduced;
                entry->item_kind = binding.item_kind;
                entry->export_ordinal = effective_index;
                entry->source_attribute_count = effective.attribute_count;
                entry->introduction_span = effective.span;
                entry->source_is_generated = effective.is_generated;
                entry->is_import = binding.is_import;
                state->namespace_count += 1u;
            }
        }
    }
    /* The owned artifact has one entry for every effective public binding. */
    if (owned_entry_count != state->namespace_count)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
            CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH);
    qsort(state->namespace_values, state->namespace_count,
        sizeof(*state->namespace_values), cm_decl_namespace_compare);
    for (module_index = 1u; module_index < state->namespace_count;
            ++module_index) {
        CmDeclCaptureNamespace *prior =
            &state->namespace_values[module_index - 1u];
        CmDeclCaptureNamespace *entry =
            &state->namespace_values[module_index];
        if (prior->owner_module == entry->owner_module
            && prior->namespace_kind == entry->namespace_kind
            && cm_decl_bytes_equal(prior->name, prior->name_length,
                entry->name, entry->name_length))
            return cm_decl_capture_fail(result,
                CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_DUPLICATE);
    }
    for (module_index = 0u; module_index < state->namespace_count;
            ++module_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[module_index];
        CmHirItemId ignored_item;
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
            if (!cm_decl_primitive_reexport_provenance(state, entry)) {
                cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                    entry, entry->introduction_span);
                return 0;
            }
            continue;
        }
        if (entry->target.kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT)
            continue;
        if (!entry->is_import || entry->source_is_generated
            || cm_decl_enum_variant_parent(state, &entry->target,
                &ignored_item) == NULL
            || !cm_decl_enum_variant_mate(state, module_index)) {
            cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                entry, entry->introduction_span);
            return 0;
        }
    }
    return 1;
}

static int cm_decl_struct_entry_mate(const CmDeclCaptureState *state,
    size_t entry_index)
{
    const CmDeclCaptureNamespace *entry =
        &state->namespace_values[entry_index];
    CmHirLibraryBindingKind mate_kind = entry->target.kind
            == CM_HIR_LIBRARY_BINDING_TYPE
        ? CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        : CM_HIR_LIBRARY_BINDING_TYPE;
    size_t index;
    size_t matches = 0u;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *mate = &state->namespace_values[index];
        if (mate->target.kind == mate_kind
            && mate->owner_module == entry->owner_module
            && mate->namespace_kind != entry->namespace_kind
            && mate->export_ordinal == entry->export_ordinal
            && mate->source_attribute_count
                == entry->source_attribute_count
            && mate->source_is_generated == entry->source_is_generated
            && mate->is_import == entry->is_import
            && mate->item_kind == CM_AST_ITEM_STRUCT
            && cm_hir_def_id_equal(mate->target.definition,
                entry->target.definition)
            && cm_decl_item_ref_equal(mate->declaration,
                entry->declaration)
            && cm_decl_item_ref_equal(mate->introduced_by,
                entry->introduced_by)
            && cm_decl_bytes_equal(mate->name, mate->name_length,
                entry->name, entry->name_length)) matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_struct_source(const CmDeclCaptureState *state,
    const CmHirItem *item, int non_exhaustive, uint32_t *out_module,
    uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *type_source = NULL;
    const CmDeclCaptureNamespace *constructor_source = NULL;
    size_t index;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != CM_AST_ITEM_STRUCT
            || (entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
                && entry->target.kind
                    != CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR)) return 0;
        if (non_exhaustive) {
            if (entry->target.kind
                    == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
                || cm_decl_struct_entry_mate(state, index)) return 0;
        } else if (!cm_decl_struct_entry_mate(state, index)) return 0;
        if (entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (type_source != NULL) return 0;
                type_source = entry;
            } else {
                if (constructor_source != NULL) return 0;
                constructor_source = entry;
            }
        }
    }
    if (type_source == NULL
        || (non_exhaustive && constructor_source != NULL)
        || (!non_exhaustive && (constructor_source == NULL
            || type_source->owner_module != constructor_source->owner_module
            || type_source->export_ordinal
                != constructor_source->export_ordinal
            || !cm_decl_item_ref_equal(type_source->declaration,
                constructor_source->declaration)
            || !cm_decl_item_ref_equal(type_source->introduced_by,
                constructor_source->introduced_by)))) return 0;
    *out_module = type_source->owner_module;
    *out_ordinal = type_source->export_ordinal;
    return 1;
}

static int cm_decl_named_aggregate_source(const CmDeclCaptureState *state,
    const CmHirItem *item, CmAstItemKind expected_kind,
    uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u
        || (expected_kind != CM_AST_ITEM_STRUCT
            && expected_kind != CM_AST_ITEM_UNION)) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != expected_kind
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->target.type_kind != CM_HIR_TYPE_ADT_KIND
            || entry->target.primitive_kind != CM_HIR_PRIMITIVE_NONE
            || entry->target.value_kind != CM_HIR_LIBRARY_VALUE_NONE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_alias_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != CM_AST_ITEM_TYPE_ALIAS
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_enum_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        /* ENUM identities are module TYPE bindings only. Variants are owned
         * by the ITEM and never fabricate a module VALUE mate. */
        if (entry->item_kind != CM_AST_ITEM_ENUM
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_TYPE
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_free_value_source(const CmDeclCaptureState *state,
    const CmHirItem *item, CmAstItemKind ast_kind,
    CmHirLibraryValueKind library_kind, uint32_t *out_module,
    uint32_t *out_ordinal)
{
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    size_t index;
    size_t direct_count = 0u;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (entry->item_kind != ast_kind
            || entry->target.kind != CM_HIR_LIBRARY_BINDING_VALUE
            || entry->target.value_kind != library_kind
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_VALUE
            || entry->source_is_generated) return 0;
        if (!entry->is_import) {
            if (!cm_decl_item_ref_equal(entry->declaration,
                    entry->introduced_by)
                || !cm_decl_bytes_equal(entry->name, entry->name_length,
                    item_name->bytes, item_name->len)) return 0;
            source = entry;
            direct_count += 1u;
        }
    }
    if (direct_count != 1u || source == NULL) return 0;
    *out_module = source->owner_module;
    *out_ordinal = source->export_ordinal;
    return 1;
}

static int cm_decl_const_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    return cm_decl_free_value_source(state, item, CM_AST_ITEM_CONST,
        CM_HIR_LIBRARY_VALUE_CONST, out_module, out_ordinal);
}

static int cm_decl_static_source(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal)
{
    return cm_decl_free_value_source(state, item, CM_AST_ITEM_STATIC,
        CM_HIR_LIBRARY_VALUE_STATIC, out_module, out_ordinal);
}

static int cm_decl_effective_attribute_matches_hir(
    const CmDeclCaptureState *state,
    const CmResolveEffectiveAttribute *effective,
    const CmHirAttribute *attribute, CmResolveItemRef owner)
{
    unsigned char *metadata = NULL;
    size_t metadata_length = 0u;
    const CmInternedString *hir_metadata = cm_interner_get(
        &state->hir->strings, attribute->metadata);
    int equal;
    if (effective->source != owner.source
        || !cm_decl_item_ref_equal(effective->owner, owner)
        || effective->style != CM_AST_ATTR_OUTER
        || effective->source_attribute != attribute->source_attribute
        || effective->expansion_depth != attribute->expansion_depth
        || effective->span.source != attribute->span.source
        || effective->span.start != attribute->span.start
        || effective->span.end != attribute->span.end
        || hir_metadata == NULL || hir_metadata->len == 0u
        || !cm_decl_copy_graph_string(state->input->graph,
            effective->metadata, &metadata, &metadata_length)) return 0;
    equal = cm_decl_bytes_equal(metadata, metadata_length,
        hir_metadata->bytes, hir_metadata->len);
    cm_free(metadata);
    return equal;
}

static int cm_decl_item_source_view(const CmDeclCaptureState *state,
    const CmHirItem *item, CmAstItemKind expected_kind,
    CmDeclCaptureModule **out_module, uint32_t *out_ordinal,
    CmResolveEffectiveItem *out_effective, const CmAst **out_ast,
    const CmAstItem **out_ast_item)
{
    CmDeclCaptureModule *module;
    const CmHirDefinition *definition;
    const CmInternedString *item_name;
    uint32_t index;
    size_t matches = 0u;
    if (state == NULL || item == NULL || out_module == NULL
        || out_ordinal == NULL || out_effective == NULL || out_ast == NULL
        || out_ast_item == NULL
        || (item->visibility.kind != CM_HIR_VIS_PUBLIC
            && item->visibility.kind != CM_HIR_VIS_PRIVATE
            && item->visibility.kind != CM_HIR_VIS_CRATE
            && item->visibility.kind != CM_HIR_VIS_RESTRICTED)
        || item->owner_module == CM_HIR_MODULE_NONE
        || item->definition.crate_id != state->input->crate_id
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->span.source == 0u || item->span.start > item->span.end
        || (item_name = cm_interner_get(&state->hir->strings,
            item->name)) == NULL || item_name->len == 0u
        || (definition = cm_hir_lookup_definition(state->hir,
            item->definition)) == NULL
        || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->span.source != item->span.source
        || definition->span.start != item->span.start
        || definition->span.end != item->span.end
        || (module = cm_decl_module_by_hir((CmDeclCaptureState *)state,
            item->owner_module)) == NULL) return 0;
    for (index = 0u; index < module->graph.effective_item_count; ++index) {
        CmResolveEffectiveItem effective;
        const CmAst *ast = NULL;
        const CmAstItem *ast_item;
        const CmInternedString *ast_name;
        CmAstVisibilityKind expected_visibility;
        if (item->visibility.kind == CM_HIR_VIS_PUBLIC)
            expected_visibility = CM_AST_VIS_PUBLIC;
        else if (item->visibility.kind == CM_HIR_VIS_PRIVATE)
            expected_visibility = CM_AST_VIS_INHERITED;
        else if (item->visibility.kind == CM_HIR_VIS_CRATE)
            expected_visibility = CM_AST_VIS_CRATE;
        else expected_visibility = CM_AST_VIS_SUPER;
        if (cm_module_graph_get_effective_item(state->input->graph,
                state->input->revision, module->graph.id, index,
                &effective) != CM_RESOLVE_VIEW_OK
            || effective.is_generated || effective.item_kind != expected_kind
            || effective.visibility != expected_visibility
            || effective.declaration.source != item->span.source
            || effective.span.source != item->span.source
            || effective.span.start != item->span.start
            || effective.span.end != item->span.end
            || !cm_module_graph_borrow_item_ast(state->input->graph,
                module->graph.id, effective.declaration, &ast)
            || ast == NULL
            || (ast_item = cm_ast_get_item(ast,
                effective.declaration.item)) == NULL
            || ast_item->kind != expected_kind
            || ast_item->visibility.kind != expected_visibility
            || ast_item->visibility.restriction != CM_AST_PATH_NONE
            || ast_item->span.start != item->span.start
            || ast_item->span.end != item->span.end
            || (ast_name = cm_ast_get_string(ast, ast_item->name)) == NULL
            || !cm_decl_bytes_equal(ast_name->bytes, ast_name->len,
                item_name->bytes, item_name->len)) continue;
        if (item->visibility.kind == CM_HIR_VIS_RESTRICTED) {
            CmDeclCaptureModule *parent = module->graph.parent
                    == CM_MODULE_NONE
                ? NULL : cm_decl_module_by_graph(
                    (CmDeclCaptureState *)state, module->graph.parent);
            if (parent == NULL || !cm_hir_def_id_equal(
                    item->visibility.restriction,
                    parent->hir->definition)) continue;
        } else if (!cm_hir_def_id_is_none(item->visibility.restriction)) {
            continue;
        }
        *out_module = module;
        *out_ordinal = index;
        *out_effective = effective;
        *out_ast = ast;
        *out_ast_item = ast_item;
        matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_item_attribute_provenance(
    const CmDeclCaptureState *state, const CmHirItem *item,
    CmAstItemKind expected_kind,
    CmHirLibraryBindingKind expected_binding_kind)
{
    const CmDeclCaptureNamespace *source = NULL;
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem effective;
    uint32_t index;
    size_t namespace_index;
    size_t matches = 0u;
    for (namespace_index = 0u; namespace_index < state->namespace_count;
            ++namespace_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[namespace_index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition)) continue;
        if (!entry->is_import
            && entry->source_attribute_count != item->attribute_count)
            return 0;
        if (!entry->is_import
            && entry->target.kind == expected_binding_kind
            && entry->item_kind == expected_kind) {
            source = entry;
            matches += 1u;
        }
    }
    if (item->visibility.kind == CM_HIR_VIS_PUBLIC) {
        if (matches != 1u || source == NULL || source->source_is_generated
            || !cm_decl_item_ref_equal(source->declaration,
                source->introduced_by)) return 0;
        module = cm_decl_module_by_local((CmDeclCaptureState *)state,
            source->owner_module);
        if (module == NULL
            || cm_module_graph_get_effective_item(state->input->graph,
                state->input->revision, module->graph.id,
                source->export_ordinal, &effective) != CM_RESOLVE_VIEW_OK
            || effective.is_generated || effective.item_kind != expected_kind
            || !cm_decl_item_ref_equal(effective.declaration,
                source->declaration)) return 0;
    } else {
        const CmAst *ast = NULL;
        const CmAstItem *ast_item = NULL;
        uint32_t ordinal = 0u;
        if ((item->visibility.kind != CM_HIR_VIS_PRIVATE
                && item->visibility.kind != CM_HIR_VIS_CRATE
                && item->visibility.kind != CM_HIR_VIS_RESTRICTED)
            || matches != 0u
            || !cm_decl_item_source_view(state, item, expected_kind, &module,
                &ordinal, &effective, &ast, &ast_item)) return 0;
        (void)ordinal;
        (void)ast;
        (void)ast_item;
    }
    if (effective.attribute_count != item->attribute_count) return 0;
    for (index = 0u; index < effective.attribute_count; ++index) {
        CmResolveEffectiveAttribute graph_attribute;
        if (cm_module_graph_get_effective_item_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, effective.id, index, &graph_attribute)
                != CM_RESOLVE_VIEW_OK
            || !cm_decl_effective_attribute_matches_hir(state,
                &graph_attribute, &item->attributes[index],
                effective.declaration)) return 0;
    }
    return 1;
}

enum {
    CM_DECL_ATTR_STABLE = 1u << 0,
    CM_DECL_ATTR_UNSTABLE = 1u << 1,
    CM_DECL_ATTR_DEPRECATED = 1u << 2,
    CM_DECL_ATTR_DERIVE = 1u << 3,
    CM_DECL_ATTR_NON_EXHAUSTIVE = 1u << 4,
    CM_DECL_ATTR_ALLOW = 1u << 5,
    CM_DECL_ATTR_DOC_ALIAS = 1u << 6,
    CM_DECL_ATTR_REPR_U8 = 1u << 7,
    CM_DECL_ATTR_DIAGNOSTIC_ITEM = 1u << 8,
    CM_DECL_ATTR_LANG_ITEM = 1u << 9,
    CM_DECL_ATTR_REPR_TRANSPARENT = 1u << 10,
    CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT = 1u << 11,
    CM_DECL_ATTR_DOC_HIDDEN = 1u << 12,
    CM_DECL_ATTR_DOC_NO_INLINE = 1u << 13,
    CM_DECL_ATTR_DOC_SEARCH_UNBOX = 1u << 14,
    CM_DECL_ATTR_MUST_USE = 1u << 15,
    CM_DECL_ATTR_RUSTFMT_SKIP = 1u << 16,
    CM_DECL_ATTR_DOC_INLINE = 1u << 17,
    CM_DECL_ATTR_CONST_TRAIT = 1u << 18,
    CM_DECL_ATTR_INLINE_HINT = 1u << 19,
    CM_DECL_ATTR_REPR_U16 = 1u << 20,
    CM_DECL_ATTR_REPR_U32 = 1u << 21,
    CM_DECL_ATTR_REPR_U64 = 1u << 22,
    CM_DECL_ATTR_BARE_MUST_USE = 1u << 23,
    CM_DECL_ATTR_RUSTC_CONST_UNSTABLE = 1u << 24,
    CM_DECL_ATTR_RUSTC_INSIGNIFICANT_DTOR = 1u << 25,
    CM_DECL_ATTR_RUSTC_PAREN_SUGAR = 1u << 26,
    CM_DECL_ATTR_ON_UNIMPLEMENTED = 1u << 27,
    CM_DECL_ATTR_FUNDAMENTAL = 1u << 28,
    CM_DECL_ATTR_RUSTC_DENY_EXPLICIT_IMPL = 1u << 29,
    CM_DECL_ATTR_RUSTC_DO_NOT_IMPLEMENT_VIA_OBJECT = 1u << 30
};

enum {
    CM_DECL_ENUM_PROFILE_EXPLICIT = 0,
    CM_DECL_ENUM_PROFILE_DEFAULT_UNIT = 1,
    CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC = 2
};

static int cm_decl_plain_visibility(const CmHirItem *item);
static unsigned int cm_decl_attribute_kind(
    const CmInternedString *metadata);
static uint8_t cm_decl_primitive(const CmHirType *type);
static int cm_decl_ascii_identifier(const unsigned char *bytes,
    size_t length);
static int cm_decl_ast_ordinary_enum_generics(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item);
static int cm_decl_ast_generic_shape(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstItem *ast_item, const CmHirItem *item);
static int cm_decl_ast_type_matches_hir_field(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner, size_t depth);
static int cm_decl_ast_type_matches_hir_primitive(const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type);
static int cm_decl_ast_path_resolves_item(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstPath *path, const CmHirItem *owner,
    const CmHirItem *target);
static int cm_decl_field_visibility_matches(CmAstVisibility ast_visibility,
    CmHirVisibility hir_visibility);
static int cm_decl_string_is(const CmHirContext *hir, CmInternId id,
    const char *text);
static uint32_t cm_decl_trait_local(const CmDeclCaptureState *state,
    CmHirDefId definition);
static uint32_t cm_decl_item_local(const CmDeclCaptureState *state,
    CmHirDefId definition);

static int cm_decl_graph_string_matches_intern(
    const CmDeclCaptureState *state, CmResolveStringId graph_id,
    CmInternId hir_id)
{
    unsigned char *graph_bytes = NULL;
    size_t graph_length = 0u;
    const CmInternedString *hir_value = cm_interner_get(
        &state->hir->strings, hir_id);
    int equal;
    if (hir_value == NULL || hir_value->len == 0u
        || !cm_decl_copy_graph_string(state->input->graph, graph_id,
            &graph_bytes, &graph_length)) return 0;
    equal = cm_decl_bytes_equal(graph_bytes, graph_length,
        hir_value->bytes, hir_value->len);
    cm_free(graph_bytes);
    return equal;
}

static int cm_decl_diagnostic_item_name(const CmInternedString *metadata,
    const unsigned char **out_name, size_t *out_length)
{
    static const unsigned char prefix[] = "rustc_diagnostic_item = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t length;
    if (out_name != NULL) *out_name = NULL;
    if (out_length != NULL) *out_length = 0u;
    if (metadata == NULL || metadata->len <= prefix_length + 1u
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || metadata->bytes[metadata->len - 1u] != (unsigned char)'\"')
        return 0;
    length = metadata->len - prefix_length - 1u;
    if (!cm_decl_ascii_identifier(metadata->bytes + prefix_length, length))
        return 0;
    if (out_name != NULL) *out_name = metadata->bytes + prefix_length;
    if (out_length != NULL) *out_length = length;
    return 1;
}

static int cm_decl_lang_item_name(const CmInternedString *metadata,
    const unsigned char **out_name, size_t *out_length)
{
    static const unsigned char prefix[] = "lang = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t length;
    if (out_name != NULL) *out_name = NULL;
    if (out_length != NULL) *out_length = 0u;
    if (metadata == NULL || metadata->len <= prefix_length + 1u
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || metadata->bytes[metadata->len - 1u] != (unsigned char)'\"')
        return 0;
    length = metadata->len - prefix_length - 1u;
    if (!cm_decl_ascii_identifier(metadata->bytes + prefix_length, length))
        return 0;
    if (out_name != NULL) *out_name = metadata->bytes + prefix_length;
    if (out_length != NULL) *out_length = length;
    return 1;
}

static int cm_decl_enum_item_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, size_t *out_projected_count,
    int *out_profile, const unsigned char **out_lang,
    size_t *out_lang_length, uint8_t *out_repr)
{
    uint32_t index;
    unsigned int seen = 0u;
    const unsigned char *lang = NULL;
    size_t lang_length = 0u;
    size_t retained = 0u;
    *out_profile = CM_DECL_ENUM_PROFILE_EXPLICIT;
    *out_lang = NULL;
    *out_lang_length = 0u;
    *out_repr = CM_HIR_DECL_ENUM_REPR_RUST;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_UNSTABLE
                && kind != CM_DECL_ATTR_DEPRECATED
                && kind != CM_DECL_ATTR_DERIVE
                && kind != CM_DECL_ATTR_ALLOW
                && kind != CM_DECL_ATTR_REPR_U8
                && kind != CM_DECL_ATTR_REPR_U16
                && kind != CM_DECL_ATTR_REPR_U32
                && kind != CM_DECL_ATTR_REPR_U64
                && kind != CM_DECL_ATTR_DIAGNOSTIC_ITEM
                && kind != CM_DECL_ATTR_LANG_ITEM
                && kind != CM_DECL_ATTR_DOC_SEARCH_UNBOX
                && kind != CM_DECL_ATTR_MUST_USE)
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        if (kind == CM_DECL_ATTR_LANG_ITEM
            && !cm_decl_lang_item_name(metadata, &lang, &lang_length))
            return 0;
        seen |= kind;
    }
    if (!cm_decl_item_attribute_provenance(state, item,
            CM_AST_ITEM_ENUM, CM_HIR_LIBRARY_BINDING_TYPE)) return 0;
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    if (item->generic_parameter_count == 0u
        && seen == CM_DECL_ATTR_DIAGNOSTIC_ITEM) {
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, item->attributes[0].metadata);
        if (!cm_decl_diagnostic_item_name(metadata, NULL, NULL)) return 0;
        /* The diagnostic identity is retained in ITEM, not projected away. */
        *out_projected_count = 0u;
        *out_profile = CM_DECL_ENUM_PROFILE_DEFAULT_UNIT;
    } else if (item->generic_parameter_count == 0u
        && (seen & CM_DECL_ATTR_DERIVE) != 0u
        && ((seen & CM_DECL_ATTR_REPR_U8) != 0u
            || (seen & CM_DECL_ATTR_REPR_U16) != 0u
            || (seen & CM_DECL_ATTR_REPR_U32) != 0u
            || (seen & CM_DECL_ATTR_REPR_U64) != 0u)
        && (seen & ~(unsigned int)(CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_STABLE
                | CM_DECL_ATTR_UNSTABLE | CM_DECL_ATTR_DEPRECATED
                | CM_DECL_ATTR_REPR_U8 | CM_DECL_ATTR_REPR_U16
                | CM_DECL_ATTR_REPR_U32 | CM_DECL_ATTR_REPR_U64)) == 0u
        && (((seen & CM_DECL_ATTR_REPR_U8) != 0u)
            + ((seen & CM_DECL_ATTR_REPR_U16) != 0u)
            + ((seen & CM_DECL_ATTR_REPR_U32) != 0u)
            + ((seen & CM_DECL_ATTR_REPR_U64) != 0u)) == 1) {
        if ((seen & CM_DECL_ATTR_REPR_U8) != 0u)
            *out_repr = CM_HIR_DECL_ENUM_REPR_U8;
        else if ((seen & CM_DECL_ATTR_REPR_U16) != 0u)
            *out_repr = CM_HIR_DECL_ENUM_REPR_U16;
        else if ((seen & CM_DECL_ATTR_REPR_U32) != 0u)
            *out_repr = CM_HIR_DECL_ENUM_REPR_U32;
        else *out_repr = CM_HIR_DECL_ENUM_REPR_U64;
        /* The repr is structural; every other allowlisted attribute is
         * projected and counted. */
        *out_projected_count = (size_t)item->attribute_count - 1u;
    } else if (item->generic_parameter_count != 0u
        && (seen & (CM_DECL_ATTR_DIAGNOSTIC_ITEM
                | CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_DOC_SEARCH_UNBOX))
            == (CM_DECL_ATTR_DIAGNOSTIC_ITEM
                | CM_DECL_ATTR_DERIVE | CM_DECL_ATTR_DOC_SEARCH_UNBOX)
        && ((seen & CM_DECL_ATTR_STABLE) != 0u
            || (seen & CM_DECL_ATTR_UNSTABLE) != 0u)
        && (seen & (CM_DECL_ATTR_REPR_U8 | CM_DECL_ATTR_REPR_U16
                | CM_DECL_ATTR_REPR_U32 | CM_DECL_ATTR_REPR_U64)) == 0u) {
        for (index = 0u; index < item->attribute_count; ++index) {
            const CmInternedString *metadata = cm_interner_get(
                &state->hir->strings, item->attributes[index].metadata);
            if (cm_decl_attribute_kind(metadata)
                    == CM_DECL_ATTR_DIAGNOSTIC_ITEM
                && !cm_decl_diagnostic_item_name(metadata, NULL, NULL))
                return 0;
        }
        retained = 1u;
        if ((seen & CM_DECL_ATTR_LANG_ITEM) != 0u) retained += 1u;
        if ((size_t)item->attribute_count < retained) return 0;
        *out_projected_count = (size_t)item->attribute_count - retained;
        *out_profile = CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC;
        *out_lang = lang;
        *out_lang_length = lang_length;
    } else {
        return 0;
    }
    return 1;
}

static int cm_decl_enum_variant_projected_attributes(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    const CmResolveEffectiveItem *enumeration,
    const CmResolveEffectiveVariant *effective, uint32_t variant_index,
    const CmAst *ast, const CmAstVariant *ast_variant,
    size_t *out_projected_count, unsigned int *out_kinds)
{
    unsigned int seen = 0u;
    uint32_t index;
    *out_projected_count = 0u;
    *out_kinds = 0u;
    if (ast == NULL || ast_variant == NULL
        || effective->attribute_count != ast_variant->attribute_count
        || ((effective->attribute_count == 0u)
            != (ast_variant->attributes == NULL))) return 0;
    for (index = 0u; index < effective->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        const CmAstAttribute *source_attribute;
        const CmInternedString *source_metadata;
        unsigned char *graph_metadata = NULL;
        size_t graph_metadata_length = 0u;
        CmInternedString metadata_view;
        unsigned int kind;
        int valid;
        if (cm_module_graph_get_effective_variant_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, enumeration->id, variant_index, index,
                &attribute) != CM_RESOLVE_VIEW_OK
            || attribute.source == 0u || attribute.source_attribute == 0u
            || attribute.style != CM_AST_ATTR_OUTER
            || attribute.expansion_depth != 0u
            || attribute.span.source != attribute.source
            || attribute.span.source != effective->span.source
            || attribute.span.start > attribute.span.end
            || attribute.span.start < effective->span.start
            || attribute.span.end > effective->span.end
            || !cm_decl_item_ref_equal(attribute.owner,
                enumeration->declaration)
            || !cm_decl_item_ref_equal(attribute.owner_variant.enumeration,
                effective->declaration.enumeration)
            || attribute.owner_variant.index != effective->declaration.index
            || ast_variant->attributes[index] != attribute.source_attribute
            || (source_attribute = cm_ast_get_attribute(ast,
                attribute.source_attribute)) == NULL
            || source_attribute->style != CM_AST_ATTR_OUTER
            || source_attribute->span.start > attribute.span.start
            || source_attribute->span.end < attribute.span.end
            || (source_metadata = cm_ast_get_string(ast,
                source_attribute->text)) == NULL
            || source_metadata->len == 0u
            || !cm_decl_copy_graph_string(state->input->graph,
                attribute.metadata, &graph_metadata,
                &graph_metadata_length)) return 0;
        metadata_view.bytes = graph_metadata;
        metadata_view.len = graph_metadata_length;
        kind = cm_decl_attribute_kind(&metadata_view);
        valid = graph_metadata_length <= SIZE_MAX - 3u
            && source_metadata->len == graph_metadata_length + 3u
            && source_metadata->bytes[0] == (unsigned char)'#'
            && source_metadata->bytes[1] == (unsigned char)'['
            && source_metadata->bytes[source_metadata->len - 1u]
                == (unsigned char)']'
            && memcmp(source_metadata->bytes + 2u, graph_metadata,
                graph_metadata_length) == 0
            && (kind == CM_DECL_ATTR_STABLE
                || kind == CM_DECL_ATTR_UNSTABLE
                || kind == CM_DECL_ATTR_DEPRECATED)
            && (seen & kind) == 0u;
        cm_free(graph_metadata);
        if (!valid) return 0;
        seen |= kind;
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    *out_projected_count = effective->attribute_count;
    *out_kinds = seen;
    return 1;
}

static int cm_decl_enum_generic_variant_attributes(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    const CmResolveEffectiveItem *enumeration,
    const CmResolveEffectiveVariant *effective, uint32_t variant_index,
    const CmAst *ast, const CmAstVariant *ast_variant,
    CmInternId hir_lang_item, CmHirDeclarationString *out_lang)
{
    const CmInternedString *hir_lang = cm_interner_get(&state->hir->strings,
        hir_lang_item);
    unsigned int seen = 0u;
    uint32_t index;
    if (out_lang != NULL) {
        out_lang->data = NULL;
        out_lang->length = 0u;
    }
    if (hir_lang == NULL || hir_lang->len == 0u
        || effective->attribute_count != 2u || ast == NULL
        || ast_variant == NULL || ast_variant->attribute_count != 2u
        || ast_variant->attributes == NULL) return 0;
    for (index = 0u; index < effective->attribute_count; ++index) {
        CmResolveEffectiveAttribute attribute;
        const CmAstAttribute *source_attribute;
        const CmInternedString *source_metadata;
        unsigned char *graph_metadata = NULL;
        size_t graph_metadata_length = 0u;
        CmInternedString metadata_view;
        unsigned int kind;
        const unsigned char *lang = NULL;
        size_t lang_length = 0u;
        int valid;
        if (cm_module_graph_get_effective_variant_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, enumeration->id, variant_index, index,
                &attribute) != CM_RESOLVE_VIEW_OK
            || attribute.source == 0u || attribute.source_attribute == 0u
            || attribute.style != CM_AST_ATTR_OUTER
            || attribute.expansion_depth != 0u
            || attribute.span.source != attribute.source
            || attribute.span.source != effective->span.source
            || attribute.span.start > attribute.span.end
            || attribute.span.start < effective->span.start
            || attribute.span.end > effective->span.end
            || !cm_decl_item_ref_equal(attribute.owner,
                enumeration->declaration)
            || !cm_decl_item_ref_equal(attribute.owner_variant.enumeration,
                effective->declaration.enumeration)
            || attribute.owner_variant.index != effective->declaration.index
            || ast_variant->attributes[index] != attribute.source_attribute
            || (source_attribute = cm_ast_get_attribute(ast,
                attribute.source_attribute)) == NULL
            || source_attribute->style != CM_AST_ATTR_OUTER
            || source_attribute->span.start > attribute.span.start
            || source_attribute->span.end < attribute.span.end
            || (source_metadata = cm_ast_get_string(ast,
                source_attribute->text)) == NULL
            || !cm_decl_copy_graph_string(state->input->graph,
                attribute.metadata, &graph_metadata,
                &graph_metadata_length)) return 0;
        metadata_view.bytes = graph_metadata;
        metadata_view.len = graph_metadata_length;
        kind = cm_decl_attribute_kind(&metadata_view);
        valid = graph_metadata_length <= SIZE_MAX - 3u
            && source_metadata->len == graph_metadata_length + 3u
            && source_metadata->bytes[0] == (unsigned char)'#'
            && source_metadata->bytes[1] == (unsigned char)'['
            && source_metadata->bytes[source_metadata->len - 1u]
                == (unsigned char)']'
            && memcmp(source_metadata->bytes + 2u, graph_metadata,
                graph_metadata_length) == 0
            && (kind == CM_DECL_ATTR_STABLE
                || kind == CM_DECL_ATTR_LANG_ITEM)
            && (seen & kind) == 0u;
        if (valid && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = cm_decl_lang_item_name(&metadata_view, &lang,
                &lang_length);
        if (valid && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = hir_lang->len == lang_length
                && memcmp(hir_lang->bytes, lang, lang_length) == 0;
        if (valid && out_lang != NULL && kind == CM_DECL_ATTR_LANG_ITEM)
            valid = cm_decl_copy_bytes(out_lang, lang, lang_length);
        cm_free(graph_metadata);
        if (!valid) return 0;
        seen |= kind;
    }
    return seen == (CM_DECL_ATTR_STABLE | CM_DECL_ATTR_LANG_ITEM);
}

static int cm_decl_enum_variant_has_no_attributes(
    const CmResolveEffectiveVariant *effective,
    const CmAstVariant *ast_variant)
{
    return effective != NULL && ast_variant != NULL
        && effective->attribute_count == 0u
        && ast_variant->attribute_count == 0u
        && ast_variant->attributes == NULL;
}

static int cm_decl_parse_u64_decimal(const CmInternedString *text,
    uint64_t *out_value)
{
    size_t index;
    uint64_t value = 0u;
    if (text == NULL || text->len == 0u) return 0;
    for (index = 0u; index < text->len; ++index) {
        unsigned int digit;
        if (text->bytes[index] < (unsigned char)'0'
            || text->bytes[index] > (unsigned char)'9') return 0;
        digit = (unsigned int)(text->bytes[index] - (unsigned char)'0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10)) return 0;
        value = value * UINT64_C(10) + digit;
    }
    *out_value = value;
    return 1;
}

static int cm_decl_parse_u64_shift(const CmInternedString *text,
    uint64_t *out_value)
{
    size_t begin = 0u;
    size_t end;
    size_t operator_index;
    CmInternedString left;
    CmInternedString right;
    uint64_t base;
    uint64_t shift;
    if (text == NULL) return 0;
    end = text->len;
    while (begin < end && (text->bytes[begin] == (unsigned char)' '
            || text->bytes[begin] == (unsigned char)'\t')) begin += 1u;
    while (end > begin && (text->bytes[end - 1u] == (unsigned char)' '
            || text->bytes[end - 1u] == (unsigned char)'\t')) end -= 1u;
    left.bytes = text->bytes + begin;
    left.len = end - begin;
    if (cm_decl_parse_u64_decimal(&left, out_value)) return 1;
    for (operator_index = begin; operator_index + 1u < end;
            ++operator_index) {
        size_t left_end;
        size_t right_begin;
        if (text->bytes[operator_index] != (unsigned char)'<'
            || text->bytes[operator_index + 1u] != (unsigned char)'<')
            continue;
        left_end = operator_index;
        while (left_end > begin
                && (text->bytes[left_end - 1u] == (unsigned char)' '
                    || text->bytes[left_end - 1u]
                        == (unsigned char)'\t')) left_end -= 1u;
        right_begin = operator_index + 2u;
        while (right_begin < end
                && (text->bytes[right_begin] == (unsigned char)' '
                    || text->bytes[right_begin]
                        == (unsigned char)'\t')) right_begin += 1u;
        left.bytes = text->bytes + begin;
        left.len = left_end - begin;
        right.bytes = text->bytes + right_begin;
        right.len = end - right_begin;
        if (left.len == 0u || right.len == 0u
            || !cm_decl_parse_u64_decimal(&left, &base)
            || !cm_decl_parse_u64_decimal(&right, &shift)
            || shift >= 64u || base > (UINT64_MAX >> shift)) return 0;
        *out_value = base << shift;
        return 1;
    }
    return 0;
}

static uint64_t cm_decl_enum_repr_max(uint8_t repr)
{
    if (repr == CM_HIR_DECL_ENUM_REPR_U8) return UINT64_C(255);
    if (repr == CM_HIR_DECL_ENUM_REPR_U16) return UINT64_C(65535);
    if (repr == CM_HIR_DECL_ENUM_REPR_U32) return UINT64_C(4294967295);
    if (repr == CM_HIR_DECL_ENUM_REPR_U64) return UINT64_MAX;
    return 0u;
}

static int cm_decl_enum_shape_and_variants(const CmDeclCaptureState *state,
    const CmHirItem *item, CmHirItemId item_id, uint32_t owner_module,
    uint32_t source_ordinal, int profile, uint8_t enum_repr,
    size_t *out_projected_count)
{
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem enumeration;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    uint32_t index;
    uint32_t prior_source_ordinal = 0u;
    unsigned int expected_variant_kinds = 0u;
    int saw_tuple = 0;
    if (item->kind != CM_HIR_ITEM_ENUM
        || (item->visibility.kind != CM_HIR_VIS_PUBLIC
            && item->visibility.kind != CM_HIR_VIS_PRIVATE
            && item->visibility.kind != CM_HIR_VIS_CRATE
            && item->visibility.kind != CM_HIR_VIS_RESTRICTED)
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC
            ? (item->generic_parameter_count == 0u
                || item->generic_parameter_start
                    == CM_HIR_GENERIC_PARAM_NONE)
            : (item->generic_parameter_start
                    != CM_HIR_GENERIC_PARAM_NONE
                || item->generic_parameter_count != 0u))
        || item->predicate_scopes != NULL
        || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->data.enum_item.variant_count == 0u
        || (size_t)item->data.enum_item.variant_count
            > CM_HIR_DECL_METADATA_MAX_VARIANTS
        || item->data.enum_item.variants == NULL) return 0;
    if (!cm_decl_item_source_view(state, item, CM_AST_ITEM_ENUM, &module,
            &index, &enumeration, &ast, &ast_item)
        || module->local != owner_module || index != source_ordinal
        || enumeration.variant_count != item->data.enum_item.variant_count
        || ast_item->data.enum_item.variant_count == 0u
        || ast_item->data.enum_item.variants == NULL
        || (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC
            && !cm_decl_ast_ordinary_enum_generics(state, ast, ast_item,
                item)))
        return 0;
    if (profile == CM_DECL_ENUM_PROFILE_EXPLICIT) {
        for (index = 0u; index < item->attribute_count; ++index) {
            const CmInternedString *attribute_metadata = cm_interner_get(
                &state->hir->strings, item->attributes[index].metadata);
            unsigned int kind = cm_decl_attribute_kind(attribute_metadata);
            if (kind == CM_DECL_ATTR_STABLE
                || kind == CM_DECL_ATTR_UNSTABLE
                || kind == CM_DECL_ATTR_DEPRECATED)
                expected_variant_kinds |= kind;
        }
    }
    for (index = 0u; index < item->data.enum_item.variant_count; ++index) {
        const CmHirVariant *variant = &item->data.enum_item.variants[index];
        const CmAstVariant *ast_variant;
        const CmInternedString *ast_discriminant;
        const CmInternedString *name = cm_interner_get(&state->hir->strings,
            variant->name);
        const CmHirDefinition *definition = cm_hir_lookup_definition(
            state->hir, variant->definition);
        const CmHirType *discriminant_type = NULL;
        CmResolveEffectiveVariant effective;
        uint64_t source_discriminant;
        uint32_t prior;
        if (cm_module_graph_get_effective_variant(state->input->graph,
                state->input->revision, module->graph.id, enumeration.id,
                index, &effective) != CM_RESOLVE_VIEW_OK
            || effective.declaration.index
                >= ast_item->data.enum_item.variant_count
            || (index != 0u && effective.declaration.index
                <= prior_source_ordinal)) return 0;
        ast_variant = &ast_item->data.enum_item.variants[
            effective.declaration.index];
        ast_discriminant = ast_variant->discriminant == CM_INTERN_ID_NONE
            ? NULL : cm_ast_get_string(ast, ast_variant->discriminant);
        if (name == NULL || name->len == 0u || definition == NULL
            || definition->kind != CM_HIR_DEFINITION_ENUM_VARIANT
            || definition->state != CM_HIR_DEFINITION_BOUND
            || definition->entity.enum_variant.enum_item_id != item_id
            || definition->entity.enum_variant.variant_index != index
            || definition->span.source != variant->span.source
            || definition->span.start != variant->span.start
            || definition->span.end != variant->span.end
            || variant->span.source != item->span.source
            || variant->span.start > variant->span.end
            || variant->span.start < item->span.start
            || variant->span.end > item->span.end
            || effective.is_generated
            || !cm_decl_item_ref_equal(effective.declaration.enumeration,
                enumeration.declaration)
            || effective.span.source != variant->span.source
            || effective.span.start != variant->span.start
            || effective.span.end != variant->span.end
            || !cm_decl_graph_string_matches_intern(state, effective.name,
                variant->name)) return 0;
        if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC) {
            uint32_t field_index;
            CmAstFieldForm ast_form = variant->form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_AST_FIELDS_UNIT : CM_AST_FIELDS_TUPLE;
            if ((variant->form != CM_HIR_AGGREGATE_UNIT
                    && variant->form != CM_HIR_AGGREGATE_TUPLE)
                || effective.form != ast_form || ast_variant->form != ast_form
                || effective.field_count != variant->field_count
                || ast_variant->field_count != variant->field_count
                || ((variant->field_count == 0u)
                    != (variant->fields == NULL))
                || ((ast_variant->field_count == 0u)
                    != (ast_variant->fields == NULL))
                || (variant->form == CM_HIR_AGGREGATE_UNIT
                    && variant->field_count != 0u)
                || (variant->form == CM_HIR_AGGREGATE_TUPLE
                    && variant->field_count == 0u)
                || ast_variant->discriminant != CM_INTERN_ID_NONE
                || ast_discriminant != NULL || variant->has_discriminant
                || variant->discriminant.kind != 0
                || variant->discriminant.type != CM_HIR_TYPE_NONE
                || variant->discriminant.data.value.low_bits != 0u
                || variant->discriminant.data.value.high_bits != 0u
                || !cm_decl_enum_generic_variant_attributes(state, module,
                    &enumeration, &effective, index, ast, ast_variant,
                    variant->lang_item, NULL))
                return 0;
            for (field_index = 0u; field_index < variant->field_count;
                    ++field_index) {
                const CmHirField *field = &variant->fields[field_index];
                const CmAstField *ast_field =
                    &ast_variant->fields[field_index];
                const CmAstType *ast_type = cm_ast_get_type(ast,
                    ast_field->type);
                const CmHirType *hir_type = cm_hir_get_type(state->hir,
                    field->type);
                if (field->name != CM_INTERN_ID_NONE
                    || !cm_decl_field_visibility_matches(
                        ast_field->visibility, field->visibility)
                    || field->visibility.kind != CM_HIR_VIS_PRIVATE
                    || hir_type == NULL
                    || hir_type->kind != CM_HIR_TYPE_PARAMETER_KIND
                    || field->span.source != item->span.source
                    || field->span.start != item->span.start
                    || field->span.end != item->span.end
                    || !cm_decl_ast_type_matches_hir_field(state, ast,
                        ast_type, hir_type, item, 0u)) return 0;
            }
            if (variant->form == CM_HIR_AGGREGATE_TUPLE) saw_tuple = 1;
        } else if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_UNIT) {
            if (variant->lang_item != CM_INTERN_ID_NONE
                || variant->form != CM_HIR_AGGREGATE_UNIT
                || variant->fields != NULL || variant->field_count != 0u
                || effective.form != CM_AST_FIELDS_UNIT
                || effective.field_count != 0u
                || ast_variant->form != CM_AST_FIELDS_UNIT
                || ast_variant->fields != NULL
                || ast_variant->field_count != 0u
                || ast_variant->discriminant != CM_INTERN_ID_NONE
                || ast_discriminant != NULL || variant->has_discriminant
                || variant->discriminant.kind != 0
                || variant->discriminant.type != CM_HIR_TYPE_NONE
                || variant->discriminant.data.value.low_bits != 0u
                || variant->discriminant.data.value.high_bits != 0u
                || !cm_decl_enum_variant_has_no_attributes(&effective,
                    ast_variant)) return 0;
        } else {
            size_t variant_projected_count;
            unsigned int variant_kinds;
            if (variant->lang_item != CM_INTERN_ID_NONE
                || variant->form != CM_HIR_AGGREGATE_UNIT
                || variant->fields != NULL || variant->field_count != 0u
                || effective.form != CM_AST_FIELDS_UNIT
                || effective.field_count != 0u
                || ast_variant->form != CM_AST_FIELDS_UNIT
                || ast_variant->fields != NULL
                || ast_variant->field_count != 0u
                || ast_variant->discriminant == CM_INTERN_ID_NONE
                || !cm_decl_parse_u64_shift(ast_discriminant,
                    &source_discriminant)
                || !variant->has_discriminant
                || variant->discriminant.kind != CM_HIR_CONST_VALUE
                || (discriminant_type = cm_hir_get_type(state->hir,
                    variant->discriminant.type)) == NULL
                || discriminant_type->kind != CM_HIR_TYPE_INTEGER_KIND
                || discriminant_type->data.integer_type.kind
                    != CM_HIR_INT_ISIZE
                || variant->discriminant.data.value.high_bits != 0u
                || enum_repr == CM_HIR_DECL_ENUM_REPR_RUST
                || variant->discriminant.data.value.low_bits
                    > cm_decl_enum_repr_max(enum_repr)
                || variant->discriminant.data.value.low_bits
                    != source_discriminant
                || !cm_decl_enum_variant_projected_attributes(state, module,
                    &enumeration, &effective, index, ast, ast_variant,
                    &variant_projected_count, &variant_kinds)
                || variant_kinds != expected_variant_kinds
                || variant_projected_count > SIZE_MAX
                    - *out_projected_count)
                return 0;
            *out_projected_count += variant_projected_count;
        }
        for (prior = 0u; prior < index; ++prior) {
            const CmHirVariant *prior_variant =
                &item->data.enum_item.variants[prior];
            if (prior_variant->name == variant->name
                || (profile == CM_DECL_ENUM_PROFILE_EXPLICIT
                    && prior_variant->discriminant.data.value.low_bits
                        == variant->discriminant.data.value.low_bits)) return 0;
        }
        prior_source_ordinal = effective.declaration.index;
    }
    if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC) {
        uint32_t generic_index;
        if (!saw_tuple) return 0;
        for (generic_index = 0u;
                generic_index < item->generic_parameter_count;
                ++generic_index) {
            CmHirGenericParamId expected = item->generic_parameter_start
                + generic_index;
            int used = 0;
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                uint32_t field_index;
                for (field_index = 0u; field_index < variant->field_count;
                        ++field_index) {
                    const CmHirType *field_type = cm_hir_get_type(state->hir,
                        variant->fields[field_index].type);
                    if (field_type != NULL
                        && field_type->kind == CM_HIR_TYPE_PARAMETER_KIND
                        && field_type->data.parameter_type.parameter
                            == expected) used = 1;
                }
            }
            if (!used) return 0;
        }
    }
    if (profile == CM_DECL_ENUM_PROFILE_DEFAULT_GENERIC) {
        if ((size_t)item->data.enum_item.variant_count > SIZE_MAX
                - *out_projected_count) return 0;
        *out_projected_count += item->data.enum_item.variant_count;
    }
    return 1;
}

static int cm_decl_item_already(const CmDeclCaptureItem *items,
    size_t count, CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < count; ++index)
        if (cm_hir_def_id_equal(items[index].item->definition, definition))
            return 1;
    return 0;
}

static const CmHirItem *cm_decl_bound_item(const CmHirContext *hir,
    CmHirDefId definition, CmHirItemId *out_id)
{
    const CmHirDefinition *resolved = cm_hir_lookup_definition(hir,
        definition);
    if (resolved == NULL || resolved->state != CM_HIR_DEFINITION_BOUND
        || resolved->kind != CM_HIR_DEFINITION_ITEM
        || resolved->entity.item_id == CM_HIR_ITEM_NONE) return NULL;
    *out_id = resolved->entity.item_id;
    return cm_hir_get_item(hir, *out_id);
}

static int cm_decl_plain_visibility(const CmHirItem *item)
{
    return item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction);
}

static int cm_decl_attribute_call_is(const CmInternedString *metadata,
    const char *head)
{
    size_t head_length = strlen(head);
    return metadata != NULL && metadata->len > head_length + 2u
        && memcmp(metadata->bytes, head, head_length) == 0
        && metadata->bytes[head_length] == (unsigned char)'('
        && metadata->bytes[metadata->len - 1u] == (unsigned char)')';
}

static int cm_decl_attribute_bare_is(const CmInternedString *metadata,
    const char *name)
{
    size_t length = strlen(name);
    return metadata != NULL && metadata->len == length
        && memcmp(metadata->bytes, name, length) == 0;
}

static int cm_decl_ascii_identifier(const unsigned char *bytes, size_t length)
{
    size_t index;
    if (bytes == NULL || length == 0u
        || !((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; index < length; ++index) {
        if (!((bytes[index] >= (unsigned char)'a'
                    && bytes[index] <= (unsigned char)'z')
                || (bytes[index] >= (unsigned char)'A'
                    && bytes[index] <= (unsigned char)'Z')
                || (bytes[index] >= (unsigned char)'0'
                    && bytes[index] <= (unsigned char)'9')
                || bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_decl_attribute_doc_alias_is(const CmInternedString *metadata)
{
    static const unsigned char prefix[] = "doc(alias(\"";
    static const unsigned char suffix[] = "\"))";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t suffix_length = sizeof(suffix) - 1u;
    size_t identifier_length;
    if (metadata == NULL
        || metadata->len <= prefix_length + suffix_length
        || memcmp(metadata->bytes, prefix, prefix_length) != 0
        || memcmp(metadata->bytes + metadata->len - suffix_length,
            suffix, suffix_length) != 0) return 0;
    identifier_length = metadata->len - prefix_length - suffix_length;
    return cm_decl_ascii_identifier(metadata->bytes + prefix_length,
        identifier_length);
}

static int cm_decl_attribute_must_use_is(const CmInternedString *metadata)
{
    static const unsigned char prefix[] = "must_use = \"";
    size_t prefix_length = sizeof(prefix) - 1u;
    return metadata != NULL && metadata->len > prefix_length + 1u
        && memcmp(metadata->bytes, prefix, prefix_length) == 0
        && metadata->bytes[metadata->len - 1u] == (unsigned char)'\"';
}

static int cm_decl_attribute_inline_hint_is(
    const CmInternedString *metadata)
{
    return cm_decl_attribute_bare_is(metadata, "inline")
        || cm_decl_attribute_bare_is(metadata, "inline(always)")
        || cm_decl_attribute_bare_is(metadata, "inline(never)");
}

static unsigned int cm_decl_attribute_kind(const CmInternedString *metadata)
{
    if (cm_decl_attribute_call_is(metadata, "stable"))
        return CM_DECL_ATTR_STABLE;
    if (cm_decl_attribute_call_is(metadata, "unstable"))
        return CM_DECL_ATTR_UNSTABLE;
    if (cm_decl_attribute_call_is(metadata, "deprecated"))
        return CM_DECL_ATTR_DEPRECATED;
    if (cm_decl_attribute_call_is(metadata, "derive"))
        return CM_DECL_ATTR_DERIVE;
    if (cm_decl_attribute_bare_is(metadata, "non_exhaustive"))
        return CM_DECL_ATTR_NON_EXHAUSTIVE;
    if (cm_decl_attribute_call_is(metadata, "allow"))
        return CM_DECL_ATTR_ALLOW;
    if (cm_decl_attribute_doc_alias_is(metadata))
        return CM_DECL_ATTR_DOC_ALIAS;
    if (cm_decl_attribute_bare_is(metadata, "repr(u8)"))
        return CM_DECL_ATTR_REPR_U8;
    if (cm_decl_attribute_bare_is(metadata, "repr(u16)"))
        return CM_DECL_ATTR_REPR_U16;
    if (cm_decl_attribute_bare_is(metadata, "repr(u32)"))
        return CM_DECL_ATTR_REPR_U32;
    if (cm_decl_attribute_bare_is(metadata, "repr(u64)"))
        return CM_DECL_ATTR_REPR_U64;
    if (cm_decl_diagnostic_item_name(metadata, NULL, NULL))
        return CM_DECL_ATTR_DIAGNOSTIC_ITEM;
    if (cm_decl_lang_item_name(metadata, NULL, NULL))
        return CM_DECL_ATTR_LANG_ITEM;
    if (cm_decl_attribute_bare_is(metadata, "repr(transparent)"))
        return CM_DECL_ATTR_REPR_TRANSPARENT;
    if (cm_decl_attribute_bare_is(metadata, "rustc_pub_transparent"))
        return CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT;
    if (cm_decl_attribute_bare_is(metadata, "doc(hidden)"))
        return CM_DECL_ATTR_DOC_HIDDEN;
    if (cm_decl_attribute_bare_is(metadata, "doc(no_inline)"))
        return CM_DECL_ATTR_DOC_NO_INLINE;
    if (cm_decl_attribute_bare_is(metadata, "doc(search_unbox)"))
        return CM_DECL_ATTR_DOC_SEARCH_UNBOX;
    if (cm_decl_attribute_must_use_is(metadata))
        return CM_DECL_ATTR_MUST_USE;
    if (cm_decl_attribute_bare_is(metadata, "must_use"))
        return CM_DECL_ATTR_BARE_MUST_USE;
    if (cm_decl_attribute_call_is(metadata, "rustc_const_unstable"))
        return CM_DECL_ATTR_RUSTC_CONST_UNSTABLE;
    if (cm_decl_attribute_bare_is(metadata, "rustc_insignificant_dtor"))
        return CM_DECL_ATTR_RUSTC_INSIGNIFICANT_DTOR;
    if (cm_decl_attribute_bare_is(metadata, "rustfmt::skip"))
        return CM_DECL_ATTR_RUSTFMT_SKIP;
    if (cm_decl_attribute_bare_is(metadata, "doc(inline)"))
        return CM_DECL_ATTR_DOC_INLINE;
    if (cm_decl_attribute_bare_is(metadata, "const_trait"))
        return CM_DECL_ATTR_CONST_TRAIT;
    if (cm_decl_attribute_inline_hint_is(metadata))
        return CM_DECL_ATTR_INLINE_HINT;
    if (cm_decl_attribute_bare_is(metadata, "rustc_paren_sugar"))
        return CM_DECL_ATTR_RUSTC_PAREN_SUGAR;
    if (cm_decl_attribute_call_is(metadata, "rustc_on_unimplemented"))
        return CM_DECL_ATTR_ON_UNIMPLEMENTED;
    if (cm_decl_attribute_bare_is(metadata, "fundamental"))
        return CM_DECL_ATTR_FUNDAMENTAL;
    if (cm_decl_attribute_call_is(metadata,
            "diagnostic::on_unimplemented"))
        return CM_DECL_ATTR_ON_UNIMPLEMENTED;
    if (cm_decl_attribute_bare_is(metadata, "rustc_deny_explicit_impl"))
        return CM_DECL_ATTR_RUSTC_DENY_EXPLICIT_IMPL;
    if (cm_decl_attribute_bare_is(metadata,
            "rustc_do_not_implement_via_object"))
        return CM_DECL_ATTR_RUSTC_DO_NOT_IMPLEMENT_VIA_OBJECT;
    return 0u;
}

static int cm_decl_project_item_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, unsigned int allowed,
    size_t *out_projected_count, int *out_non_exhaustive)
{
    uint32_t index;
    unsigned int seen = 0u;
    *out_projected_count = 0u;
    *out_non_exhaustive = 0;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || kind == 0u || (allowed & kind) == 0u
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        seen |= kind;
        *out_projected_count += 1u;
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    *out_non_exhaustive = (seen & CM_DECL_ATTR_NON_EXHAUSTIVE) != 0u;
    return 1;
}

static int cm_decl_static_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, size_t *out_projected_count)
{
    int non_exhaustive = 0;
    const CmInternedString *metadata;
    if (item->attribute_count != 1u || item->attributes == NULL
        || (metadata = cm_interner_get(&state->hir->strings,
            item->attributes[0].metadata)) == NULL
        || cm_decl_attribute_kind(metadata) != CM_DECL_ATTR_DOC_HIDDEN
        || !cm_decl_project_item_attributes(state, item,
            CM_DECL_ATTR_DOC_HIDDEN, out_projected_count, &non_exhaustive)
        || non_exhaustive || *out_projected_count != 1u) return 0;
    return 1;
}

static int cm_decl_aggregate_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, uint8_t *out_repr, uint16_t *out_flags,
    const unsigned char **out_lang, size_t *out_lang_length,
    const unsigned char **out_diagnostic, size_t *out_diagnostic_length,
    size_t *out_projected_count)
{
    uint32_t index;
    unsigned int seen = 0u;
    const unsigned char *lang = NULL;
    size_t lang_length = 0u;
    const unsigned char *diagnostic = NULL;
    size_t diagnostic_length = 0u;
    size_t retained = 0u;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_UNSTABLE
                && kind != CM_DECL_ATTR_DEPRECATED
                && kind != CM_DECL_ATTR_DERIVE
                && kind != CM_DECL_ATTR_LANG_ITEM
                && kind != CM_DECL_ATTR_REPR_TRANSPARENT
                && kind != CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT
                && kind != CM_DECL_ATTR_ALLOW
                && kind != CM_DECL_ATTR_DIAGNOSTIC_ITEM
                && kind != CM_DECL_ATTR_RUSTC_INSIGNIFICANT_DTOR)
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        if (kind == CM_DECL_ATTR_LANG_ITEM
            && !cm_decl_lang_item_name(metadata, &lang, &lang_length)) return 0;
        if (kind == CM_DECL_ATTR_DIAGNOSTIC_ITEM
            && !cm_decl_diagnostic_item_name(metadata, &diagnostic,
                &diagnostic_length)) return 0;
        seen |= kind;
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    if (item->visibility.kind == CM_HIR_VIS_PUBLIC) {
        if ((seen & CM_DECL_ATTR_ALLOW) != 0u
            || (seen & CM_DECL_ATTR_DERIVE) == 0u
            || ((seen & CM_DECL_ATTR_STABLE) == 0u
                && (seen & CM_DECL_ATTR_UNSTABLE) == 0u)) return 0;
    } else if ((seen & (CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
            | CM_DECL_ATTR_DEPRECATED | CM_DECL_ATTR_LANG_ITEM
            | CM_DECL_ATTR_REPR_TRANSPARENT
            | CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT
            | CM_DECL_ATTR_DIAGNOSTIC_ITEM
            | CM_DECL_ATTR_RUSTC_INSIGNIFICANT_DTOR)) != 0u) {
        return 0;
    }
    *out_repr = (seen & CM_DECL_ATTR_REPR_TRANSPARENT) != 0u
        ? CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        : CM_HIR_DECL_AGGREGATE_REPR_RUST;
    *out_flags = 0u;
    if ((seen & CM_DECL_ATTR_LANG_ITEM) != 0u) {
        if (lang == NULL || lang_length == 0u) return 0;
        *out_flags |= CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM;
        retained += 1u;
    }
    if ((seen & CM_DECL_ATTR_REPR_TRANSPARENT) != 0u) retained += 1u;
    if ((seen & CM_DECL_ATTR_RUSTC_PUB_TRANSPARENT) != 0u) {
        if (*out_repr != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT) return 0;
        *out_flags |= CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
        retained += 1u;
    }
    if ((seen & CM_DECL_ATTR_DIAGNOSTIC_ITEM) != 0u) {
        if (diagnostic == NULL || diagnostic_length == 0u) return 0;
        *out_flags |= CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM;
        retained += 1u;
    }
    if ((seen & CM_DECL_ATTR_RUSTC_INSIGNIFICANT_DTOR) != 0u) {
        *out_flags |= CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR;
        retained += 1u;
    }
    if ((size_t)item->attribute_count < retained) return 0;
    *out_lang = lang;
    *out_lang_length = lang_length;
    *out_diagnostic = diagnostic;
    *out_diagnostic_length = diagnostic_length;
    *out_projected_count = (size_t)item->attribute_count - retained;
    return 1;
}

static const CmHirImport *cm_decl_reexport_import(
    const CmDeclCaptureModule *module, CmResolveItemRef declaration)
{
    const CmHirImport *match = NULL;
    uint32_t index;
    if (module == NULL || module->hir == NULL) return NULL;
    for (index = 0u; index < module->hir->import_count; ++index) {
        const CmHirImport *candidate = &module->hir->imports[index];
        if (candidate->span.source == declaration.source
            && candidate->source_item == declaration.item) {
            if (match != NULL) return NULL;
            match = candidate;
        }
    }
    return match;
}

static int cm_decl_primitive_reexport_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureNamespace *entry)
{
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem effective;
    const CmHirImport *import;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmInternedString *ast_tree;
    const CmInternedString *hir_tree;
    uint8_t primitive;
    size_t leaf_count;
    size_t declaration_count;
    size_t matched_leaf_count = 0u;
    size_t matched_binding_count = 0u;
    size_t index;
    int matched_entry = 0;
    if (entry == NULL
        || entry->target.kind != CM_HIR_LIBRARY_BINDING_PRIMITIVE
        || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
        || !entry->is_import || entry->source_is_generated
        || entry->declaration.source != 0u
        || entry->declaration.item != CM_AST_ITEM_NONE
        || !cm_hir_def_id_is_none(entry->target.definition)
        || entry->target.type_kind != CM_HIR_TYPE_ERROR_KIND
        || entry->target.value_kind != CM_HIR_LIBRARY_VALUE_NONE
        || (primitive = cm_decl_library_primitive(
            entry->target.primitive_kind)) == 0u)
        return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        entry->owner_module);
    import = cm_decl_reexport_import(module, entry->introduced_by);
    if (module == NULL || import == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, entry->export_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != CM_AST_ITEM_USE
        || !cm_decl_item_ref_equal(effective.declaration,
            entry->introduced_by)
        || effective.span.source != entry->introduction_span.source
        || effective.span.start != entry->introduction_span.start
        || effective.span.end != entry->introduction_span.end
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, entry->introduced_by, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            entry->introduced_by.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_USE
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->span.start != effective.span.start
        || ast_item->span.end != effective.span.end
        || ast_item->generic_parameters != NULL
        || ast_item->generic_parameter_count != 0u
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || (ast_tree = cm_ast_get_string(ast,
            ast_item->data.use_item.tree)) == NULL
        || ast_tree->len == 0u
        || import->kind != CM_HIR_IMPORT_USE
        || import->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(import->visibility.restriction)
        || import->span.source != effective.span.source
        || import->span.start != effective.span.start
        || import->span.end != effective.span.end
        || import->source_item != entry->introduced_by.item
        || (hir_tree = cm_interner_get(&state->hir->strings,
            import->tree)) == NULL
        || !cm_decl_bytes_equal(hir_tree->bytes, hir_tree->len,
            ast_tree->bytes, ast_tree->len))
        return 0;
    declaration_count = cm_import_declaration_binding_count(
        state->input->imports, module->graph.id, entry->introduced_by);
    if (declaration_count == 0u || declaration_count > (size_t)UINT32_MAX
        || declaration_count != import->binding_count
        || import->bindings == NULL) return 0;
    leaf_count = cm_import_leaf_count(state->input->imports);
    if (leaf_count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < leaf_count; ++index) {
        CmImportLeafView leaf;
        CmResolvedBinding resolver_binding;
        const CmHirImportBinding *hir_binding;
        const CmInternedString *hir_name;
        unsigned char *resolver_name = NULL;
        size_t resolver_name_length = 0u;
        unsigned char *leaf_name = NULL;
        size_t leaf_name_length = 0u;
        uint8_t resolver_primitive = 0u;
        uint32_t segment_index;
        int valid;
        if (!cm_import_get_leaf(state->input->imports, (uint32_t)index,
                &leaf)) return 0;
        if (leaf.module != module->graph.id
            || !cm_decl_item_ref_equal(leaf.declaration,
                entry->introduced_by)) continue;
        valid = matched_binding_count < declaration_count
            && leaf.revision == state->input->revision
            && leaf.segment_count != 0u && leaf.binding_count == 1u
            && !leaf.is_glob && !leaf.is_anonymous && leaf.is_public
            && leaf.is_crate_visible && leaf.is_resolved
            && !leaf.saw_ambiguous
            && cm_import_get_declaration_binding(state->input->imports,
                module->graph.id, entry->introduced_by,
                (uint32_t)matched_binding_count, &resolver_binding)
            && resolver_binding.revision == state->input->revision
            && resolver_binding.module == module->graph.id
            && resolver_binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
            && (resolver_primitive = cm_decl_resolver_primitive(
                resolver_binding.primitive_kind)) != 0u
            && resolver_binding.declaration.source == 0u
            && resolver_binding.declaration.item == CM_AST_ITEM_NONE
            && resolver_binding.variant.enumeration.source == 0u
            && resolver_binding.variant.enumeration.item == CM_AST_ITEM_NONE
            && resolver_binding.variant.index == 0u
            && resolver_binding.target_module == CM_MODULE_NONE
            && cm_decl_item_ref_equal(resolver_binding.import_declaration,
                entry->introduced_by)
            && resolver_binding.is_public
            && resolver_binding.is_crate_visible
            && resolver_binding.is_import && resolver_binding.is_reexport
            && !resolver_binding.is_ambiguous
            && !resolver_binding.is_anonymous
            && cm_decl_copy_import_string(state->input->imports,
                resolver_binding.name, &resolver_name,
                &resolver_name_length)
            && cm_decl_copy_import_string(state->input->imports,
                leaf.import_name, &leaf_name, &leaf_name_length)
            && cm_decl_bytes_equal(resolver_name, resolver_name_length,
                leaf_name, leaf_name_length);
        hir_binding = valid ? &import->bindings[matched_binding_count] : NULL;
        hir_name = hir_binding == NULL ? NULL : cm_interner_get(
            &state->hir->strings, hir_binding->name);
        valid = valid && hir_binding->namespace_kind == CM_HIR_NAMESPACE_TYPE
            && cm_decl_library_primitive(hir_binding->primitive_kind)
                == resolver_primitive
            && cm_hir_def_id_is_none(hir_binding->target)
            && !hir_binding->is_anonymous && hir_binding->is_public
            && hir_binding->is_crate_visible && hir_name != NULL
            && cm_decl_bytes_equal(hir_name->bytes, hir_name->len,
                resolver_name, resolver_name_length);
        for (segment_index = 0u; valid
                && segment_index < leaf.segment_count; ++segment_index) {
            CmResolvePathSegmentView segment;
            valid = cm_import_get_leaf_segment(state->input->imports,
                    (uint32_t)index, segment_index, &segment)
                && segment.bytes != NULL && segment.length != 0u;
        }
        if (valid && resolver_primitive == primitive
            && cm_decl_bytes_equal(resolver_name, resolver_name_length,
                entry->name, entry->name_length)) matched_entry = 1;
        if (valid) matched_binding_count += 1u;
        cm_free(leaf_name);
        cm_free(resolver_name);
        if (!valid) return 0;
        matched_leaf_count += 1u;
    }
    return matched_leaf_count != 0u
        && matched_leaf_count == declaration_count
        && matched_binding_count == declaration_count && matched_entry;
}

/*
 * `doc(no_inline)` is admitted only for a source-authenticated public use
 * tree.  A declaration may be a group of named leaves or one glob leaf; a
 * mixed/multiple-glob tree is outside this bounded projection.  The resolver
 * snapshot is the authority for the parsed leaf shape and its complete
 * binding census.
 */
static int cm_decl_reexport_no_inline_provenance(
    const CmDeclCaptureState *state, const CmDeclCaptureModule *module,
    CmResolveItemRef declaration)
{
    size_t leaf_count = cm_import_leaf_count(state->input->imports);
    size_t declaration_count;
    size_t matched = 0u;
    size_t glob_count = 0u;
    size_t binding_total = 0u;
    size_t index;
    if (leaf_count > (size_t)UINT32_MAX) return 0;
    for (index = 0u; index < leaf_count; ++index) {
        CmImportLeafView leaf;
        if (!cm_import_get_leaf(state->input->imports, (uint32_t)index,
                &leaf)) return 0;
        if (leaf.module != module->graph.id
            || !cm_decl_item_ref_equal(leaf.declaration, declaration))
            continue;
        if (leaf.revision != state->input->revision || !leaf.is_public
            || !leaf.is_crate_visible || !leaf.is_resolved
            || leaf.saw_ambiguous || leaf.is_anonymous
            || leaf.binding_count == 0u
            || leaf.binding_count > SIZE_MAX - binding_total) return 0;
        binding_total += leaf.binding_count;
        matched += 1u;
        if (leaf.is_glob) glob_count += 1u;
    }
    declaration_count = cm_import_declaration_binding_count(
        state->input->imports, module->graph.id, declaration);
    if (matched == 0u || binding_total != declaration_count
        || (glob_count != 0u && (glob_count != 1u || matched != 1u)))
        return 0;
    for (index = 0u; index < declaration_count; ++index) {
        CmResolvedBinding binding;
        if (index > (size_t)UINT32_MAX
            || !cm_import_get_declaration_binding(state->input->imports,
                module->graph.id, declaration, (uint32_t)index, &binding)
            || binding.revision != state->input->revision
            || binding.module != module->graph.id
            || !cm_decl_item_ref_equal(binding.import_declaration,
                declaration)
            || !binding.is_import || !binding.is_public
            || !binding.is_reexport || binding.is_ambiguous
            || binding.is_anonymous) return 0;
    }
    return 1;
}

static int cm_decl_reexport_attributes(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    size_t entry_index;
    for (entry_index = 0u; entry_index < state->namespace_count;
            ++entry_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[entry_index];
        CmDeclCaptureModule *module;
        CmResolveEffectiveItem effective;
        const CmHirImport *import;
        unsigned int seen = 0u;
        uint32_t attribute_index;
        size_t prior;
        int first = 1;
        CmResolveViewStatus effective_status;
        if (!entry->is_import) continue;
        for (prior = 0u; prior < entry_index; ++prior) {
            if (state->namespace_values[prior].is_import
                && cm_decl_item_ref_equal(
                    state->namespace_values[prior].introduced_by,
                    entry->introduced_by)) {
                first = 0;
                break;
            }
        }
        module = cm_decl_module_by_local(state, entry->owner_module);
        import = cm_decl_reexport_import(module, entry->introduced_by);
        if (module == NULL || import == NULL) {
            CmSpan span = import == NULL
                ? (CmSpan){ 0u, 0u, 0u } : import->span;
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, span);
        }
        effective_status = cm_module_graph_get_effective_item(
            state->input->graph, state->input->revision, module->graph.id,
            entry->export_ordinal, &effective);
        if (effective_status != CM_RESOLVE_VIEW_OK) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        if (effective.is_generated) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, effective.span);
        }
        if (effective.item_kind != CM_AST_ITEM_USE
            || !cm_decl_item_ref_equal(effective.declaration,
                entry->introduced_by)
            || effective.attribute_count != entry->source_attribute_count
            || effective.attribute_count != import->attribute_count
            || ((import->attribute_count == 0u)
                != (import->attributes == NULL))) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        for (attribute_index = 0u;
                attribute_index < effective.attribute_count;
                ++attribute_index) {
            CmResolveEffectiveAttribute graph_attribute;
            const CmHirAttribute *hir_attribute =
                &import->attributes[attribute_index];
            const CmInternedString *metadata = cm_interner_get(
                &state->hir->strings, hir_attribute->metadata);
            unsigned int kind = cm_decl_attribute_kind(metadata);
            uint32_t duplicate_index;
            if (cm_module_graph_get_effective_item_attribute(
                    state->input->graph, state->input->revision,
                    module->graph.id, effective.id, attribute_index,
                    &graph_attribute) != CM_RESOLVE_VIEW_OK
                || !cm_decl_effective_attribute_matches_hir(state,
                    &graph_attribute, hir_attribute, entry->introduced_by)
                || hir_attribute->expansion_depth != 0u
                || (kind == CM_DECL_ATTR_STABLE
                    && (seen & CM_DECL_ATTR_UNSTABLE) != 0u)
                || (kind == CM_DECL_ATTR_UNSTABLE
                    && (seen & CM_DECL_ATTR_STABLE) != 0u)
                || (kind != CM_DECL_ATTR_STABLE
                    && kind != CM_DECL_ATTR_UNSTABLE
                    && kind != CM_DECL_ATTR_DEPRECATED
                    && kind != CM_DECL_ATTR_ALLOW
                    && kind != CM_DECL_ATTR_DOC_ALIAS
                    && kind != CM_DECL_ATTR_DOC_NO_INLINE
                    && kind != CM_DECL_ATTR_RUSTFMT_SKIP
                    && kind != CM_DECL_ATTR_DOC_INLINE
                    && kind != CM_DECL_ATTR_DOC_HIDDEN)
                || (seen & kind) != 0u) {
                return cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                    entry, hir_attribute->span);
            }
            for (duplicate_index = 0u;
                    duplicate_index < attribute_index; ++duplicate_index) {
                if (import->attributes[duplicate_index].span.source
                        == hir_attribute->span.source
                    && import->attributes[duplicate_index].source_attribute
                        == hir_attribute->source_attribute) {
                    return cm_decl_capture_reexport_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, hir_attribute->span);
                }
            }
            seen |= kind;
        }
        if ((seen & CM_DECL_ATTR_DOC_NO_INLINE) != 0u
            && !cm_decl_reexport_no_inline_provenance(state, module,
                entry->introduced_by)) {
            return cm_decl_capture_reexport_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                entry, import->span);
        }
        if (first) {
            if ((size_t)effective.attribute_count > SIZE_MAX
                    - state->projected_semantic_attribute_count) {
                return cm_decl_capture_reexport_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT,
                    entry, import->span);
            }
            state->projected_semantic_attribute_count +=
                effective.attribute_count;
        }
    }
    return 1;
}

static int cm_decl_unit_struct_shape(const CmHirItem *item)
{
    return item->kind == CM_HIR_ITEM_STRUCT
        && cm_decl_plain_visibility(item)
        && cm_hir_def_id_is_none(item->parent_definition)
        && !item->is_specializable
        && item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        && item->generic_parameter_count == 0u
        && item->predicate_scopes == NULL
        && item->predicate_scope_count == 0u
        && item->predicates == NULL
        && item->predicate_count == 0u
        && item->outlives_predicates == NULL
        && item->outlives_predicate_count == 0u
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT
        && item->data.aggregate_item.fields == NULL
        && item->data.aggregate_item.field_count == 0u;
}

static int cm_decl_type_alias_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    const CmHirType *target;
    const CmHirItem *target_item;
    CmHirItemId target_id;
    if (item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || !cm_hir_def_id_is_none(
            item->data.type_alias_item.trait_item_definition)
        || item->data.type_alias_item.bounds != NULL
        || item->data.type_alias_item.bound_count != 0u
        || (target = cm_hir_get_type(state->hir,
            item->data.type_alias_item.target)) == NULL
        || target->kind != CM_HIR_TYPE_ADT_KIND
        || target->data.named_type.argument_count != 0u
        || target->data.named_type.arguments != NULL
        || target->data.named_type.definition.crate_id
            != state->input->crate_id) return 0;
    target_item = cm_decl_bound_item(state->hir,
        target->data.named_type.definition, &target_id);
    return target_item != NULL && cm_decl_unit_struct_shape(target_item);
}

static int cm_decl_generics_shape(const CmDeclCaptureState *state,
    const CmHirItem *item)
{
    uint32_t index;
    if (item->generic_parameter_count == 0u)
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) return 0;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        CmHirGenericParamId id = item->generic_parameter_start + index;
        const CmHirGenericParam *generic;
        const CmHirType *declared_type;
        const CmInternedString *name;
        if (id < item->generic_parameter_start
            || (generic = cm_hir_get_generic_param(state->hir, id)) == NULL
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->index != index || generic->has_default
            || (name = cm_interner_get(&state->hir->strings,
                generic->name)) == NULL || name->len == 0u) return 0;
        if (generic->kind == CM_HIR_GENERIC_TYPE) {
            if (generic->declared_type != CM_HIR_TYPE_NONE) return 0;
        } else if (generic->kind == CM_HIR_GENERIC_CONST) {
            declared_type = cm_hir_get_type(state->hir,
                generic->declared_type);
            if ((item->kind != CM_HIR_ITEM_STRUCT
                    && item->kind != CM_HIR_ITEM_FUNCTION)
                || generic->is_relaxed_sized || declared_type == NULL
                || cm_decl_primitive(declared_type)
                    != CM_HIR_DECL_PRIMITIVE_USIZE) return 0;
        } else return 0;
    }
    return 1;
}

static uint32_t cm_decl_associated_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    uint32_t local = 0u;
    for (index = 0u; index < state->associated_count; ++index) {
        const CmHirItem *item = state->associated_items[index].item;
        if (item == NULL || !cm_hir_def_id_equal(item->definition,
                definition)) continue;
        if (local != 0u) return 0u;
        local = (uint32_t)(index + 1u);
    }
    return local;
}

static int cm_decl_callable_trait_shape(const CmDeclCaptureState *state,
    const CmDeclCaptureItem *capture)
{
    const uint8_t tuple_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint8_t callable_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    const CmHirItem *item = capture->item;
    const CmHirGenericParam *generic;
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmDeclCaptureItem *tuple_capture;
    uint32_t tuple_local;
    uint32_t index;
    if (item->kind != CM_HIR_ITEM_TRAIT
        || item->visibility.kind != CM_HIR_VIS_PUBLIC
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable || item->data.trait_item.is_auto
        || item->data.trait_item.safety != CM_HIR_SAFE
        || capture->lang_item == NULL || capture->lang_item_length == 0u
        || capture->diagnostic_item != NULL
        || capture->diagnostic_item_length != 0u
        || capture->has_static_outlives) return 0;
    if (capture->trait_flags == tuple_flags)
        return !item->data.trait_item.is_const
            && item->generic_parameter_count == 0u
            && item->predicate_count == 0u
            && item->data.trait_item.supertrait_count == 0u
            && capture->associated_count == 0u;
    if (capture->trait_flags != callable_flags
        || !item->data.trait_item.is_const
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || item->predicate_count != 1u || item->predicates == NULL
        || item->predicate_scope_count != 0u
        || item->outlives_predicate_count != 0u) return 0;
    generic = cm_hir_get_generic_param(state->hir,
        item->generic_parameter_start);
    predicate = &item->predicates[0];
    subject = cm_hir_get_type(state->hir, predicate->subject);
    tuple_local = cm_decl_trait_local(state,
        predicate->trait_type.definition);
    tuple_capture = tuple_local == 0u ? NULL
        : &state->traits[tuple_local - 1u];
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || generic->index != 0u
        || !cm_hir_def_id_equal(generic->owner, item->definition)
        || generic->is_relaxed_sized || generic->has_default
        || generic->declared_type != CM_HIR_TYPE_NONE
        || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->binder.lifetime_count != 0u
        || predicate->binder.lifetimes != NULL
        || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
        || predicate->trait_type.argument_count != 0u
        || predicate->trait_type.arguments != NULL
        || predicate->equality_count != 0u || predicate->equalities != NULL
        || subject == NULL || subject->kind != CM_HIR_TYPE_PARAMETER_KIND
        || subject->data.parameter_type.parameter
            != item->generic_parameter_start
        || tuple_capture == NULL || tuple_capture->trait_flags != tuple_flags)
        return 0;
    if (item->data.trait_item.supertrait_count > 1u
        || (item->data.trait_item.supertrait_count == 0u)
            != (item->data.trait_item.supertraits == NULL)) return 0;
    if (item->data.trait_item.supertrait_count == 0u) {
        if (capture->associated_count != 2u) return 0;
    } else {
        const CmHirSupertrait *supertrait =
            &item->data.trait_item.supertraits[0];
        const CmHirType *argument;
        uint32_t parent_local = cm_decl_trait_local(state,
            supertrait->trait_type.definition);
        const CmDeclCaptureItem *parent = parent_local == 0u ? NULL
            : &state->traits[parent_local - 1u];
        if (supertrait->modifier != CM_HIR_SUPERTRAIT_REQUIRED
            || supertrait->equality_count != 0u
            || supertrait->equalities != NULL
            || supertrait->trait_type.argument_count != 1u
            || supertrait->trait_type.arguments == NULL
            || supertrait->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
            || (argument = cm_hir_get_type(state->hir,
                supertrait->trait_type.arguments[0].data.type)) == NULL
            || argument->kind != CM_HIR_TYPE_PARAMETER_KIND
            || argument->data.parameter_type.parameter
                != item->generic_parameter_start
            || parent == NULL || parent->trait_flags != callable_flags
            || parent->item->data.trait_item.supertrait_count != 0u
            || parent->associated_count != 2u
            || capture->associated_count != 1u) return 0;
    }
    for (index = 0u; index < capture->associated_count; ++index) {
        const CmHirItem *child = state->associated_items[
            capture->associated_start - 1u + index].item;
        if (child == NULL || !cm_hir_def_id_equal(child->parent_definition,
                item->definition)) return 0;
        if (child->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            if (item->data.trait_item.supertrait_count != 0u || index != 0u
                || child->data.type_alias_item.target != CM_HIR_TYPE_NONE
                || !cm_hir_def_id_is_none(
                    child->data.type_alias_item.trait_item_definition)
                || child->data.type_alias_item.bound_count != 0u
                || child->data.type_alias_item.bounds != NULL
                || child->generic_parameter_count != 0u
                || child->predicate_count != 0u
                || state->associated_items[
                    capture->associated_start - 1u + index]
                        .lang_item_length == 0u) return 0;
        } else if (child->kind == CM_HIR_ITEM_FUNCTION) {
            const CmHirFunctionSignature *signature =
                &child->data.function_item.signature;
            const CmHirType *receiver;
            const CmHirType *receiver_self;
            const CmHirType *argument;
            const CmHirType *projection;
            const CmHirType *projection_self;
            const CmHirType *projection_argument;
            const CmHirItem *projection_associated;
            uint32_t associated_local;
            if (index + 1u != capture->associated_count
                || child->generic_parameter_count != 0u
                || child->predicate_count != 0u
                || child->predicate_scope_count != 0u
                || child->outlives_predicate_count != 0u
                || child->data.function_item.has_default_body
                || child->data.function_item.body != CM_HIR_BODY_NONE
                || signature->parameter_count != 2u
                || signature->parameters == NULL
                || signature->safety != CM_HIR_SAFE
                || signature->is_const || signature->is_async
                || signature->is_variadic
                || !cm_decl_string_is(state->hir, signature->abi,
                    "rust-call")) return 0;
            receiver = cm_hir_get_type(state->hir,
                signature->parameters[0].type);
            receiver_self = receiver == NULL ? NULL
                : (signature->receiver == CM_HIR_RECEIVER_VALUE ? receiver
                    : receiver->kind == CM_HIR_TYPE_REFERENCE_KIND
                        ? cm_hir_get_type(state->hir,
                            receiver->data.reference_type.pointee) : NULL);
            argument = cm_hir_get_type(state->hir,
                signature->parameters[1].type);
            projection = cm_hir_get_type(state->hir,
                signature->return_type);
            projection_self = projection == NULL
                    || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
                ? NULL : cm_hir_get_type(state->hir,
                    projection->data.projection_type.self_type);
            associated_local = projection == NULL
                    || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
                ? 0u : cm_decl_associated_local(state,
                    projection->data.projection_type.associated_type
                        .definition);
            projection_associated = associated_local == 0u ? NULL
                : state->associated_items[associated_local - 1u].item;
            projection_argument = projection == NULL
                    || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
                    || projection->data.projection_type.trait_type
                        .argument_count != 1u
                    || projection->data.projection_type.trait_type.arguments
                        == NULL
                    || projection->data.projection_type.trait_type
                        .arguments[0].kind != CM_HIR_GENERIC_ARG_TYPE
                ? NULL : cm_hir_get_type(state->hir,
                    projection->data.projection_type.trait_type.arguments[0]
                        .data.type);
            if ((item->data.trait_item.supertrait_count == 0u
                    ? (signature->receiver != CM_HIR_RECEIVER_VALUE
                        || receiver == NULL
                        || receiver->kind != CM_HIR_TYPE_SELF_KIND)
                    : (signature->receiver != CM_HIR_RECEIVER_REF_MUTABLE
                        || receiver == NULL
                        || receiver->kind != CM_HIR_TYPE_REFERENCE_KIND
                        || receiver->data.reference_type.mutability
                            != CM_HIR_MUTABLE
                        || receiver->data.reference_type.region.kind
                            != CM_HIR_REGION_ERASED))
                || receiver_self == NULL
                || receiver_self->kind != CM_HIR_TYPE_SELF_KIND
                || !cm_hir_def_id_equal(receiver_self->data.self_type.owner,
                    item->definition)
                || argument == NULL
                || argument->kind != CM_HIR_TYPE_PARAMETER_KIND
                || argument->data.parameter_type.parameter
                    != item->generic_parameter_start
                || projection_self == NULL
                || projection_self->kind != CM_HIR_TYPE_SELF_KIND
                || !cm_hir_def_id_equal(projection_self->data.self_type.owner,
                    item->definition)
                || associated_local == 0u
                || projection_associated == NULL
                || !cm_hir_def_id_equal(
                    projection_associated->parent_definition,
                    projection->data.projection_type.trait_type.definition)
                || projection->data.projection_type.associated_type
                    .argument_count != 0u
                || projection->data.projection_type.associated_type.arguments
                    != NULL
                || projection->data.projection_type.trait_type.argument_count
                    != 1u
                || projection->data.projection_type.trait_type.arguments
                    == NULL
                || projection_argument == NULL
                || projection_argument->kind
                    != CM_HIR_TYPE_PARAMETER_KIND
                || projection_argument->data.parameter_type.parameter
                    != item->generic_parameter_start) return 0;
        } else return 0;
    }
    return 1;
}

static int cm_decl_trait_shape(const CmDeclCaptureState *state,
    const CmDeclCaptureItem *capture)
{
    const CmHirItem *item = capture->item;
    unsigned int parent_attribute_kinds = 0u;
    int safe_static_profile;
    int private_method_profile;
    uint32_t attribute_index;
    uint32_t index;
    if (capture->trait_flags != 0u)
        return cm_decl_callable_trait_shape(state, capture);
    if (item->kind != CM_HIR_ITEM_TRAIT
        || (item->visibility.kind != CM_HIR_VIS_PUBLIC
            && item->visibility.kind != CM_HIR_VIS_PRIVATE)
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->predicate_scope_count != 0u || item->predicate_count != 0u
        || item->outlives_predicate_count
            != (capture->has_static_outlives ? 1u : 0u)
        || item->data.trait_item.is_auto || item->data.trait_item.is_const
        || item->data.trait_item.supertrait_count != 0u
        || !cm_decl_generics_shape(state, item)) return 0;
    for (attribute_index = 0u; attribute_index < item->attribute_count;
            ++attribute_index) {
        const CmInternedString *attribute_metadata = cm_interner_get(
            &state->hir->strings, item->attributes[attribute_index].metadata);
        parent_attribute_kinds |= cm_decl_attribute_kind(attribute_metadata);
    }
    safe_static_profile = item->data.trait_item.safety == CM_HIR_SAFE
        && capture->has_static_outlives
        && capture->diagnostic_item != NULL
        && capture->diagnostic_item_length != 0u
        && item->generic_parameter_count == 0u
        && item->attribute_count == 2u
        && parent_attribute_kinds == (CM_DECL_ATTR_STABLE
            | CM_DECL_ATTR_DIAGNOSTIC_ITEM);
    private_method_profile = item->visibility.kind == CM_HIR_VIS_PRIVATE
        && item->data.trait_item.safety == CM_HIR_SAFE
        && !capture->has_static_outlives
        && capture->diagnostic_item == NULL
        && capture->diagnostic_item_length == 0u
        && item->generic_parameter_count == 0u
        && item->attribute_count == 1u
        && parent_attribute_kinds == CM_DECL_ATTR_ALLOW
        && capture->associated_count == 1u;
    if (capture->associated_count == 0u)
        return item->data.trait_item.safety == CM_HIR_SAFE
            && !capture->has_static_outlives
            && capture->diagnostic_item == NULL
            && capture->diagnostic_item_length == 0u;
    if (capture->associated_start == 0u
        || (size_t)(capture->associated_start - 1u)
            > state->associated_count
        || capture->associated_count > state->associated_count
            - (capture->associated_start - 1u)
        || (item->data.trait_item.safety == CM_HIR_UNSAFE
            ? (capture->has_static_outlives
                || capture->diagnostic_item != NULL
                || capture->diagnostic_item_length != 0u)
            : ((!safe_static_profile && !private_method_profile)
                || capture->associated_count != 1u)))
        return 0;
    for (index = 0u; index < capture->associated_count; ++index) {
        const CmHirItem *child = state->associated_items[
            capture->associated_start - 1u + index].item;
        const CmHirFunctionSignature *signature;
        const CmHirType *receiver;
        const CmHirType *receiver_self;
        uint32_t predicate_index;
        if (child == NULL || child->kind != CM_HIR_ITEM_FUNCTION
            || child->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
            || child->generic_parameter_count != 0u
            || child->predicate_scope_count != 0u
            || child->outlives_predicate_count != 0u
            || child->is_specializable) return 0;
        signature = &child->data.function_item.signature;
        receiver = signature->parameter_count == 0u
            || signature->parameters == NULL ? NULL
            : cm_hir_get_type(state->hir, signature->parameters[0].type);
        receiver_self = receiver == NULL
            || receiver->kind != CM_HIR_TYPE_REFERENCE_KIND ? NULL
            : cm_hir_get_type(state->hir,
                receiver->data.reference_type.pointee);
        if ((signature->receiver != CM_HIR_RECEIVER_REF_SHARED
                && signature->receiver != CM_HIR_RECEIVER_REF_MUTABLE)
            || signature->parameter_count == 0u
            || receiver == NULL
            || receiver->data.reference_type.mutability
                != (signature->receiver == CM_HIR_RECEIVER_REF_MUTABLE
                    ? CM_HIR_MUTABLE : CM_HIR_IMMUTABLE)
            || receiver->data.reference_type.region.kind
                != CM_HIR_REGION_ERASED
            || receiver_self == NULL
            || receiver_self->kind != CM_HIR_TYPE_SELF_KIND
            || !cm_hir_def_id_equal(receiver_self->data.self_type.owner,
                item->definition)
            || signature->is_const || signature->is_async
            || signature->is_variadic
            || !cm_decl_string_is(state->hir, signature->abi, "Rust"))
            return 0;
        if (private_method_profile) {
            if (signature->receiver != CM_HIR_RECEIVER_REF_MUTABLE
                || signature->parameter_count != 2u
                || signature->safety != CM_HIR_UNSAFE
                || child->attribute_count != 0u
                || child->attributes != NULL
                || child->predicate_count != 0u
                || child->predicates != NULL
                || child->data.function_item.has_default_body
                || child->data.function_item.body != CM_HIR_BODY_NONE
                || cm_decl_primitive(cm_hir_get_type(state->hir,
                    signature->return_type))
                    != CM_HIR_DECL_PRIMITIVE_UNIT) return 0;
        }
        if (safe_static_profile) {
            const CmHirType *return_type = cm_hir_get_type(state->hir,
                signature->return_type);
            const CmHirItem *return_item;
            uint32_t return_local;
            const CmInternedString *attribute_metadata;
            if (child->attribute_count != 1u || child->attributes == NULL
                || (attribute_metadata = cm_interner_get(&state->hir->strings,
                    child->attributes[0].metadata)) == NULL
                || cm_decl_attribute_kind(attribute_metadata)
                    != CM_DECL_ATTR_STABLE
                || child->predicate_count != 0u
                || child->predicates != NULL
                || child->data.function_item.has_default_body
                || child->data.function_item.body != CM_HIR_BODY_NONE
                || signature->safety != CM_HIR_SAFE
                || signature->parameter_count != 1u
                || return_type == NULL
                || return_type->kind != CM_HIR_TYPE_ADT_KIND
                || return_type->data.named_type.argument_count != 0u
                || return_type->data.named_type.arguments != NULL
                || (return_local = cm_decl_item_local(state,
                    return_type->data.named_type.definition)) == 0u
                || (return_item = state->items[return_local - 1u].item)
                    == NULL
                || return_item->kind != CM_HIR_ITEM_STRUCT
                || return_item->visibility.kind != CM_HIR_VIS_PUBLIC
                || return_item->generic_parameter_count != 0u) return 0;
        }
        for (predicate_index = 0u;
                predicate_index < child->predicate_count;
                ++predicate_index) {
            const CmHirTraitPredicate *predicate =
                &child->predicates[predicate_index];
            const CmHirType *subject = cm_hir_get_type(state->hir,
                predicate->subject);
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->equality_count != 0u
                || predicate->equalities != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || predicate->trait_type.argument_count != 0u
                || predicate->trait_type.arguments != NULL
                || cm_decl_trait_local(state,
                    predicate->trait_type.definition) == 0u
                || subject == NULL || subject->kind != CM_HIR_TYPE_SELF_KIND
                || !cm_hir_def_id_equal(subject->data.self_type.owner,
                    item->definition)) return 0;
        }
    }
    return 1;
}

static int cm_decl_string_is(const CmHirContext *hir, CmInternId id,
    const char *text)
{
    const CmInternedString *value = cm_interner_get(&hir->strings, id);
    size_t length = strlen(text);
    return value != NULL && value->len == length
        && memcmp(value->bytes, text, length) == 0;
}

static int cm_decl_const_function_attributes(
    const CmDeclCaptureState *state, const CmHirItem *item,
    size_t *out_projected_count)
{
    const unsigned int required = CM_DECL_ATTR_BARE_MUST_USE
        | CM_DECL_ATTR_STABLE | CM_DECL_ATTR_RUSTC_CONST_UNSTABLE;
    unsigned int seen = 0u;
    uint32_t index;
    *out_projected_count = 0u;
    if (item->attribute_count != 3u || item->attributes == NULL) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind & required) == 0u || (kind & ~required) != 0u
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        seen |= kind;
    }
    if (seen != required
        || !cm_decl_item_attribute_provenance(state, item,
            CM_AST_ITEM_FUNCTION, CM_HIR_LIBRARY_BINDING_VALUE)) return 0;
    *out_projected_count = 3u;
    return 1;
}

static int cm_decl_simple_unit_function_attributes(
    const CmDeclCaptureState *state, const CmHirItem *item,
    size_t *out_projected_count)
{
    const unsigned int allowed = CM_DECL_ATTR_STABLE
        | CM_DECL_ATTR_UNSTABLE | CM_DECL_ATTR_INLINE_HINT;
    int non_exhaustive = 0;
    if (item->attribute_count != 2u || item->attributes == NULL
        || !cm_decl_project_item_attributes(state, item, allowed,
            out_projected_count, &non_exhaustive)
        || non_exhaustive || *out_projected_count != 2u
        || !cm_decl_item_attribute_provenance(state, item,
            CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_BINDING_VALUE)) return 0;
    return 1;
}

static int cm_decl_simple_unit_function_shape(CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal,
    size_t *out_projected_count)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    const CmHirLibraryOwnedValue *owned;
    const CmHirType *return_type;
    const CmHirBody *body;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    const CmAstExpr *ast_body;
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    uint32_t namespace_module = 0u;
    uint32_t namespace_ordinal = 0u;
    *out_projected_count = 0u;
    if (!cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->data.function_item.has_default_body
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_decl_free_value_source(state, item, CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_VALUE_FUNCTION, &namespace_module,
            &namespace_ordinal)
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_FUNCTION,
            &module, out_ordinal, &effective, &ast, &ast_item)
        || module == NULL || namespace_module != module->local
        || namespace_ordinal != *out_ordinal
        || !cm_decl_simple_unit_function_attributes(state, item,
            out_projected_count)) return 0;
    *out_module = module->local;
    return_type = cm_hir_get_type(state->hir, signature->return_type);
    if (signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->parameters != NULL || signature->parameter_count != 0u
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_decl_string_is(state->hir, signature->abi, "Rust")
        || return_type == NULL
        || cm_decl_primitive(return_type) != CM_HIR_DECL_PRIMITIVE_UNIT
        || return_type->span.source != item->span.source
        || return_type->span.start > return_type->span.end
        || return_type->span.start < item->span.start
        || return_type->span.end > item->span.end
        || ast_item->is_default
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->generic_parameters != NULL
        || ast_item->generic_parameter_count != 0u
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->data.function_item.parameters != NULL
        || ast_item->data.function_item.parameter_count != 0u
        || ast_item->data.function_item.return_type != CM_AST_TYPE_NONE
        || ast_item->data.function_item.abi != CM_INTERN_ID_NONE
        || ast_item->data.function_item.body == CM_AST_EXPR_NONE
        || ast_item->data.function_item.is_const
        || ast_item->data.function_item.is_async
        || ast_item->data.function_item.is_safe
        || ast_item->data.function_item.is_unsafe) return 0;
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || owned->declaration.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || !cm_hir_def_id_is_none(
            owned->declaration.data.function.parent_trait)
        || owned->declaration.data.function.receiver != CM_HIR_RECEIVER_NONE
        || owned->declaration.data.function.has_default_body
        || owned->declaration.data.function.parameter_types != NULL
        || owned->declaration.data.function.parameter_count != 0u
        || owned->parameter_types != NULL || owned->parameter_count != 0u
        || owned->declaration.data.function.return_type
            != signature->return_type
        || owned->declaration.data.function.generic_parameter_start
            != CM_HIR_GENERIC_PARAM_NONE
        || owned->declaration.data.function.generic_parameter_count != 0u
        || owned->declaration.data.function.predicate_scopes != NULL
        || owned->declaration.data.function.predicate_scope_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->predicate_scope_count != 0u
        || owned->declaration.data.function.predicates != NULL
        || owned->declaration.data.function.predicate_count != 0u
        || owned->predicates != NULL || owned->predicate_arguments != NULL
        || owned->predicate_equalities != NULL
        || owned->predicate_lifetimes != NULL || owned->predicate_count != 0u
        || owned->declaration.data.function.outlives_predicates != NULL
        || owned->declaration.data.function.outlives_predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->outlives_predicate_count != 0u
        || owned->nominal_references != NULL
        || owned->nominal_reference_names != NULL
        || owned->nominal_reference_generic_kinds != NULL
        || owned->nominal_reference_count != 0u
        || owned->associated_availability != NULL
        || owned->associated_availability_count != 0u
        || owned->declaration.data.function.abi != signature->abi
        || owned->declaration.data.function.safety != signature->safety
        || owned->declaration.data.function.is_const != signature->is_const
        || owned->declaration.data.function.is_async != signature->is_async
        || owned->declaration.data.function.is_variadic
            != signature->is_variadic) return 0;
    body = cm_hir_get_body(state->hir, item->data.function_item.body);
    ast_body = cm_ast_get_expr(ast, ast_item->data.function_item.body);
    if (body == NULL || ast_body == NULL
        || ((ast_body->attribute_count == 0u)
            != (ast_body->attributes == NULL))
        || ast_body->attribute_count != 0u
        || ast_body->span.start > ast_body->span.end
        || ast_body->span.start < ast_item->span.start
        || ast_body->span.end > ast_item->span.end
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->origin.definition, item->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        || !cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        || body->state != CM_HIR_BODY_UNLOWERED
        || body->expected_type != signature->return_type
        || body->parameter_count != 0u
        || body->locals != NULL || body->local_count != 0u
        || body->source != effective.declaration.source
        || body->source != item->span.source
        || body->source_expression_id != ast_item->data.function_item.body
        || body->root_expression != CM_HIR_EXPR_NONE
        || body->error_reason != CM_INTERN_ID_NONE
        || body->span.source != item->span.source
        || body->span.start != item->span.start
        || body->span.end != item->span.end) return 0;
    return 1;
}

static int cm_decl_ast_name_matches_hir(const CmAst *ast, CmInternId ast_id,
    const CmHirContext *hir, CmInternId hir_id);

static int cm_decl_from_fn_generics_source(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstItem *ast_item, const CmHirItem *item)
{
    uint32_t index;
    if (item->generic_parameter_count != 3u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || ast_item->generic_parameter_count != 3u
        || ast_item->generic_parameters == NULL) return 0;
    for (index = 0u; index < 3u; ++index) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start + index);
        const CmAstGenericParam *ast_generic =
            &ast_item->generic_parameters[index];
        if (generic == NULL || generic->index != index
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->has_default || generic->is_relaxed_sized
            || generic->span.source != item->span.source
            || generic->span.start != item->span.start
            || generic->span.end != item->span.end
            || ast_generic->attributes != NULL
            || ast_generic->attribute_count != 0u
            || ast_generic->default_type != CM_AST_TYPE_NONE
            || ast_generic->default_const != CM_INTERN_ID_NONE
            || ast_generic->default_const_expr != CM_AST_EXPR_NONE
            || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
                state->hir, generic->name)) return 0;
        if (index == 1u) {
            const CmAstType *ast_declared = cm_ast_get_type(ast,
                ast_generic->declared_type);
            const CmHirType *declared = cm_hir_get_type(state->hir,
                generic->declared_type);
            if (generic->kind != CM_HIR_GENERIC_CONST
                || ast_generic->kind != CM_AST_PARAM_CONST
                || ast_generic->constraint == CM_INTERN_ID_NONE
                || ast_generic->bounds != NULL
                || ast_generic->bound_count != 0u
                || ast_declared == NULL || declared == NULL
                || cm_decl_primitive(declared)
                    != CM_HIR_DECL_PRIMITIVE_USIZE
                || !cm_decl_ast_type_matches_hir_primitive(ast,
                    ast_declared, declared)) return 0;
        } else if (generic->kind != CM_HIR_GENERIC_TYPE
            || generic->declared_type != CM_HIR_TYPE_NONE
            || ast_generic->kind != CM_AST_PARAM_TYPE
            || ast_generic->constraint != CM_INTERN_ID_NONE
            || ast_generic->bounds != NULL
            || ast_generic->bound_count != 0u
            || ast_generic->declared_type != CM_AST_TYPE_NONE) return 0;
    }
    return 1;
}

static int cm_decl_from_fn_predicate_source(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item,
    const CmHirLibraryOwnedValue *owned)
{
    const CmAstWherePredicate *ast_predicate;
    const CmAstWhereBound *ast_bound;
    const CmAstType *ast_subject;
    const CmAstType *ast_trait_type;
    const CmAstPath *ast_trait_path;
    const CmAstPathSegment *segment;
    const CmAstGenericArg *ast_tuple_argument;
    const CmAstGenericArg *ast_equality;
    const CmAstType *ast_tuple;
    const CmAstType *ast_tuple_element;
    const CmAstType *ast_equality_value;
    const CmHirTraitPredicate *predicate;
    const CmHirTraitPredicate *owned_predicate;
    const CmHirType *subject;
    const CmHirType *tuple;
    const CmHirType *tuple_element;
    const CmHirType *equality_value;
    const CmHirItem *direct_trait;
    const CmHirItem *associated;
    const CmHirItem *declaring_trait;
    const CmHirSupertrait *supertrait;
    const CmHirType *supertrait_argument;
    CmHirItemId ignored_id;
    uint32_t index;
    int saw_direct_trait = 0;
    int saw_declaring_trait = 0;
    int saw_associated = 0;
    if (item->predicate_scope_count != 0u || item->predicate_scopes != NULL
        || item->outlives_predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->predicate_count != 1u || item->predicates == NULL
        || ast_item->where_clause == CM_INTERN_ID_NONE
        || ast_item->where_predicate_count != 1u
        || ast_item->where_predicates == NULL
        || owned == NULL || owned->predicate_count != 1u
        || owned->predicates == NULL || owned->predicate_arguments == NULL
        || owned->predicate_equalities == NULL
        || owned->predicate_lifetimes == NULL) return 0;
    ast_predicate = &ast_item->where_predicates[0];
    ast_bound = ast_predicate->bounds;
    ast_subject = cm_ast_get_type(ast, ast_predicate->subject);
    ast_trait_type = ast_bound == NULL ? NULL
        : cm_ast_get_type(ast, ast_bound->trait_type);
    ast_trait_path = ast_trait_type == NULL
            || ast_trait_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(ast, ast_trait_type->path);
    segment = ast_trait_path == NULL
            || ast_trait_path->segment_count == 0u
            || ast_trait_path->segments == NULL
        ? NULL : &ast_trait_path->segments[
            ast_trait_path->segment_count - 1u];
    ast_tuple_argument = segment == NULL || segment->argument_count != 2u
            || segment->arguments == NULL
        ? NULL : &segment->arguments[0];
    ast_equality = ast_tuple_argument == NULL ? NULL : &segment->arguments[1];
    ast_tuple = ast_tuple_argument == NULL ? NULL
        : cm_ast_get_type(ast, ast_tuple_argument->type);
    ast_tuple_element = ast_tuple == NULL || ast_tuple->element_count != 1u
            || ast_tuple->elements == NULL
        ? NULL : cm_ast_get_type(ast, ast_tuple->elements[0]);
    ast_equality_value = ast_equality == NULL ? NULL
        : cm_ast_get_type(ast, ast_equality->type);
    predicate = &item->predicates[0];
    owned_predicate = &owned->predicates[0];
    subject = cm_hir_get_type(state->hir, predicate->subject);
    tuple = predicate->trait_type.argument_count != 1u
            || predicate->trait_type.arguments == NULL
            || predicate->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(state->hir,
            predicate->trait_type.arguments[0].data.type);
    tuple_element = tuple == NULL || tuple->kind != CM_HIR_TYPE_TUPLE_KIND
            || tuple->data.tuple_type.element_count != 1u
            || tuple->data.tuple_type.elements == NULL
        ? NULL : cm_hir_get_type(state->hir,
            tuple->data.tuple_type.elements[0]);
    equality_value = predicate->equality_count != 1u
            || predicate->equalities == NULL
        ? NULL : cm_hir_get_type(state->hir,
            predicate->equalities[0].value);
    direct_trait = cm_decl_bound_item(state->hir,
        predicate->trait_type.definition, &ignored_id);
    associated = predicate->equality_count != 1u ? NULL
        : cm_decl_bound_item(state->hir,
            predicate->equalities[0].associated_type, &ignored_id);
    declaring_trait = associated == NULL ? NULL
        : cm_decl_bound_item(state->hir, associated->parent_definition,
            &ignored_id);
    supertrait = direct_trait == NULL
            || direct_trait->kind != CM_HIR_ITEM_TRAIT
            || direct_trait->data.trait_item.supertrait_count != 1u
            || direct_trait->data.trait_item.supertraits == NULL
        ? NULL : &direct_trait->data.trait_item.supertraits[0];
    supertrait_argument = supertrait == NULL
            || supertrait->trait_type.argument_count != 1u
            || supertrait->trait_type.arguments == NULL
            || supertrait->trait_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
        ? NULL : cm_hir_get_type(state->hir,
            supertrait->trait_type.arguments[0].data.type);
    if (ast_predicate->kind != CM_AST_WHERE_PREDICATE_TYPE
        || ast_predicate->binder.lifetime_count != 0u
        || ast_predicate->binder.lifetimes != NULL
        || ast_predicate->bound_count != 1u || ast_bound == NULL
        || ast_bound->kind != CM_AST_WHERE_BOUND_TRAIT
        || ast_bound->modifier != CM_AST_WHERE_BOUND_REQUIRED
        || ast_bound->binder.lifetime_count != 0u
        || ast_bound->binder.lifetimes != NULL
        || ast_bound->lifetime != CM_INTERN_ID_NONE
        || ast_subject == NULL || ast_trait_path == NULL
        || !cm_decl_ast_type_matches_hir_field(state, ast, ast_subject,
            subject, item, 0u)
        || direct_trait == NULL || direct_trait->kind != CM_HIR_ITEM_TRAIT
        || !cm_decl_ast_path_resolves_item(state, ast, ast_trait_path,
            item, direct_trait)
        || ast_tuple_argument->kind != CM_AST_GENERIC_TYPE
        || ast_tuple_argument->name != CM_INTERN_ID_NONE
        || ast_tuple_argument->name_arguments != NULL
        || ast_tuple_argument->name_argument_count != 0u
        || ast_tuple_argument->bounds != NULL
        || ast_tuple_argument->bound_count != 0u
        || ast_tuple == NULL || ast_tuple->kind != CM_AST_TYPE_TUPLE
        || ast_tuple->tuple_provenance != CM_AST_TUPLE_CALLABLE_INPUTS
        || ast_tuple_element == NULL || tuple == NULL
        || tuple->span.source != item->span.source
        || tuple->span.start != ast_tuple->span.start
        || tuple->span.end != ast_tuple->span.end
        || tuple_element == NULL
        || !cm_decl_ast_type_matches_hir_primitive(ast, ast_tuple_element,
            tuple_element)
        || cm_decl_primitive(tuple_element)
            != CM_HIR_DECL_PRIMITIVE_USIZE
        || ast_equality == NULL
        || ast_equality->kind != CM_AST_GENERIC_BINDING
        || ast_equality->name_arguments != NULL
        || ast_equality->name_argument_count != 0u
        || ast_equality->bounds != NULL || ast_equality->bound_count != 0u
        || ast_equality_value == NULL || equality_value == NULL
        || associated == NULL || associated->kind != CM_HIR_ITEM_TYPE_ALIAS
        || declaring_trait == NULL
        || declaring_trait->kind != CM_HIR_ITEM_TRAIT
        || !cm_decl_ast_name_matches_hir(ast, ast_equality->name,
            state->hir, associated->name)
        || !cm_decl_ast_type_matches_hir_field(state, ast,
            ast_equality_value, equality_value, item, 0u)
        || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->binder.lifetime_count != 0u
        || predicate->binder.lifetimes != NULL
        || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
        || subject == NULL || subject->kind != CM_HIR_TYPE_PARAMETER_KIND
        || subject->data.parameter_type.parameter
            != item->generic_parameter_start + 2u
        || equality_value->kind != CM_HIR_TYPE_PARAMETER_KIND
        || equality_value->data.parameter_type.parameter
            != item->generic_parameter_start
        || predicate->span.source != item->span.source
        || predicate->span.start != ast_predicate->span.start
        || predicate->span.end != ast_predicate->span.end
        || predicate->equalities[0].span.source != item->span.source
        || predicate->equalities[0].span.start != ast_equality->span.start
        || predicate->equalities[0].span.end != ast_equality->span.end
        || supertrait == NULL
        || !cm_hir_def_id_equal(supertrait->trait_type.definition,
            declaring_trait->definition)
        || supertrait->modifier != CM_HIR_SUPERTRAIT_REQUIRED
        || supertrait->equality_count != 0u
        || supertrait->equalities != NULL
        || supertrait_argument == NULL
        || supertrait_argument->kind != CM_HIR_TYPE_PARAMETER_KIND
        || supertrait_argument->data.parameter_type.parameter
            != direct_trait->generic_parameter_start
        || owned_predicate->scope != predicate->scope
        || owned_predicate->modifier != predicate->modifier
        || owned_predicate->subject != predicate->subject
        || !cm_hir_def_id_equal(owned_predicate->trait_type.definition,
            predicate->trait_type.definition)
        || owned_predicate->trait_type.argument_count != 1u
        || owned->predicate_arguments[0] == NULL
        || owned_predicate->trait_type.arguments
            != owned->predicate_arguments[0]
        || owned_predicate->trait_type.arguments[0].kind
            != CM_HIR_GENERIC_ARG_TYPE
        || owned_predicate->trait_type.arguments[0].data.type
            != predicate->trait_type.arguments[0].data.type
        || owned_predicate->equality_count != 1u
        || owned->predicate_equalities[0] == NULL
        || owned_predicate->equalities != owned->predicate_equalities[0]
        || !cm_hir_def_id_equal(
            owned_predicate->equalities[0].associated_type,
            predicate->equalities[0].associated_type)
        || owned_predicate->equalities[0].value
            != predicate->equalities[0].value
        || owned_predicate->binder.lifetime_count != 0u
        || owned->predicate_lifetimes[0] != NULL
        || owned_predicate->binder.lifetimes != NULL
        || owned_predicate->span.source != predicate->span.source
        || owned_predicate->span.start != predicate->span.start
        || owned_predicate->span.end != predicate->span.end
        || owned->nominal_reference_count != 3u
        || owned->nominal_references == NULL
        || owned->associated_availability_count != 1u
        || owned->associated_availability == NULL
        || !cm_hir_def_id_equal(
            owned->associated_availability[0].direct_trait,
            direct_trait->definition)
        || !cm_hir_def_id_equal(
            owned->associated_availability[0].associated_type,
            associated->definition)) return 0;
    for (index = 0u; index < owned->nominal_reference_count; ++index) {
        const CmHirLibraryNominalReference *reference =
            &owned->nominal_references[index];
        if (cm_hir_def_id_equal(reference->definition,
                direct_trait->definition)
            && reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT)
            saw_direct_trait += 1;
        else if (cm_hir_def_id_equal(reference->definition,
                declaring_trait->definition)
            && reference->kind == CM_HIR_LIBRARY_NOMINAL_TRAIT)
            saw_declaring_trait += 1;
        else if (cm_hir_def_id_equal(reference->definition,
                associated->definition)
            && reference->kind
                == CM_HIR_LIBRARY_NOMINAL_ASSOCIATED_TYPE
            && cm_hir_def_id_equal(reference->declaring_trait,
                declaring_trait->definition)) saw_associated += 1;
        else return 0;
    }
    return saw_direct_trait == 1 && saw_declaring_trait == 1
        && saw_associated == 1;
}

static int cm_decl_from_fn_function_shape(CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal,
    size_t *out_projected_count)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    const CmHirLibraryOwnedValue *owned;
    const CmHirFunctionParameter *parameter;
    const CmHirType *parameter_type;
    const CmHirType *return_type;
    const CmHirBody *body;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    const CmAstFunctionParam *ast_parameter;
    const CmAstPattern *ast_pattern;
    const CmAstType *ast_parameter_type;
    const CmAstType *ast_return;
    const CmAstExpr *ast_body;
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    uint32_t namespace_module = 0u;
    uint32_t namespace_ordinal = 0u;
    if (!cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->data.function_item.has_default_body
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->parameter_count != 1u || signature->parameters == NULL
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_decl_string_is(state->hir, signature->abi, "Rust")
        || !cm_decl_generics_shape(state, item)
        || !cm_decl_free_value_source(state, item, CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_VALUE_FUNCTION, &namespace_module,
            &namespace_ordinal)
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_FUNCTION,
            &module, out_ordinal, &effective, &ast, &ast_item)
        || module == NULL || namespace_module != module->local
        || namespace_ordinal != *out_ordinal
        || !cm_decl_simple_unit_function_attributes(state, item,
            out_projected_count)
        || !cm_decl_from_fn_generics_source(state, ast, ast_item, item))
        return 0;
    *out_module = module->local;
    parameter = &signature->parameters[0];
    parameter_type = cm_hir_get_type(state->hir, parameter->type);
    return_type = cm_hir_get_type(state->hir, signature->return_type);
    ast_parameter = ast_item->data.function_item.parameter_count == 1u
            && ast_item->data.function_item.parameters != NULL
        ? &ast_item->data.function_item.parameters[0] : NULL;
    ast_pattern = ast_parameter == NULL ? NULL
        : cm_ast_get_pattern(ast, ast_parameter->pattern);
    ast_parameter_type = ast_parameter == NULL ? NULL
        : cm_ast_get_type(ast, ast_parameter->type);
    ast_return = cm_ast_get_type(ast,
        ast_item->data.function_item.return_type);
    if (ast_item->is_default
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->data.function_item.abi != CM_INTERN_ID_NONE
        || ast_item->data.function_item.is_const
        || ast_item->data.function_item.is_async
        || ast_item->data.function_item.is_safe
        || ast_item->data.function_item.is_unsafe
        || ast_item->data.function_item.body == CM_AST_EXPR_NONE
        || ast_parameter == NULL || ast_parameter->is_self
        || ast_parameter->receiver_lifetime != CM_INTERN_ID_NONE
        || ast_parameter_type == NULL || ast_pattern == NULL
        || ast_pattern->kind != CM_AST_PATTERN_BINDING
        || ast_pattern->data.binding.subpattern != CM_AST_PATTERN_NONE
        || ast_pattern->data.binding.is_ref
        || ast_pattern->data.binding.is_mutable
        || parameter->binding_kind != CM_HIR_BINDING_NAMED
        || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
        || !cm_decl_ast_name_matches_hir(ast,
            ast_pattern->data.binding.name, state->hir, parameter->name)
        || parameter->span.source != item->span.source
        || parameter->span.start != ast_pattern->span.start
        || parameter->span.end != ast_pattern->span.end
        || !cm_decl_ast_type_matches_hir_field(state, ast,
            ast_parameter_type, parameter_type, item, 0u)
        || parameter_type == NULL
        || parameter_type->kind != CM_HIR_TYPE_PARAMETER_KIND
        || parameter_type->data.parameter_type.parameter
            != item->generic_parameter_start + 2u
        || ast_return == NULL || return_type == NULL
        || !cm_decl_ast_type_matches_hir_field(state, ast, ast_return,
            return_type, item, 0u)
        || return_type->kind != CM_HIR_TYPE_ARRAY_KIND
        || return_type->data.array_type.length.kind
            != CM_HIR_CONST_PARAMETER
        || return_type->data.array_type.length.data.parameter
            != item->generic_parameter_start + 1u) return 0;
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || owned->declaration.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || !cm_hir_def_id_is_none(
            owned->declaration.data.function.parent_trait)
        || owned->declaration.data.function.receiver != CM_HIR_RECEIVER_NONE
        || owned->declaration.data.function.has_default_body
        || owned->parameter_count != 1u || owned->parameter_types == NULL
        || owned->declaration.data.function.parameter_count != 1u
        || owned->declaration.data.function.parameter_types
            != owned->parameter_types
        || owned->parameter_types[0] != parameter->type
        || owned->declaration.data.function.return_type
            != signature->return_type
        || owned->declaration.data.function.generic_parameter_start
            != item->generic_parameter_start
        || owned->declaration.data.function.generic_parameter_count != 3u
        || owned->declaration.data.function.predicate_scope_count != 0u
        || owned->predicate_scope_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->declaration.data.function.predicate_count != 1u
        || owned->declaration.data.function.predicates != owned->predicates
        || owned->declaration.data.function.outlives_predicate_count != 0u
        || owned->outlives_predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->declaration.data.function.abi != signature->abi
        || owned->declaration.data.function.safety != signature->safety
        || owned->declaration.data.function.is_const
        || owned->declaration.data.function.is_async
        || owned->declaration.data.function.is_variadic
        || !cm_decl_from_fn_predicate_source(state, ast, ast_item, item,
            owned)) return 0;
    body = cm_hir_get_body(state->hir, item->data.function_item.body);
    ast_body = cm_ast_get_expr(ast, ast_item->data.function_item.body);
    if (body == NULL || ast_body == NULL
        || ast_body->attribute_count != 0u || ast_body->attributes != NULL
        || ast_body->span.start > ast_body->span.end
        || ast_body->span.start < ast_item->span.start
        || ast_body->span.end > ast_item->span.end
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->origin.definition, item->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        || !cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        || body->state != CM_HIR_BODY_UNLOWERED
        || body->expected_type != signature->return_type
        || body->parameter_count != 1u
        || body->local_count != 1u || body->locals == NULL
        || body->locals[0].name != parameter->name
        || body->locals[0].type != parameter->type
        || body->locals[0].mutability != CM_HIR_IMMUTABLE
        || body->locals[0].parameter_index != 0u
        || body->locals[0].parameter_binding_index != 0u
        || body->locals[0].span.source != parameter->span.source
        || body->locals[0].span.start != parameter->span.start
        || body->locals[0].span.end != parameter->span.end
        || body->source != effective.declaration.source
        || body->source != item->span.source
        || body->source_expression_id != ast_item->data.function_item.body
        || body->root_expression != CM_HIR_EXPR_NONE
        || body->error_reason != CM_INTERN_ID_NONE
        || body->span.source != item->span.source
        || body->span.start != item->span.start
        || body->span.end != item->span.end) return 0;
    return 1;
}

static int cm_decl_from_mut_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, size_t *out_projected_count)
{
    uint32_t index;
    int saw_stable = 0;
    int saw_const_stable = 0;
    if (item->attribute_count != 2u || item->attributes == NULL) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, item->attributes[index].metadata);
        if (cm_decl_attribute_call_is(metadata, "stable")) {
            if (saw_stable) return 0;
            saw_stable = 1;
        } else if (cm_decl_attribute_call_is(metadata,
                "rustc_const_stable")) {
            if (saw_const_stable) return 0;
            saw_const_stable = 1;
        } else return 0;
    }
    if (!saw_stable || !saw_const_stable) return 0;
    *out_projected_count = 2u;
    return 1;
}

static int cm_decl_from_mut_function_shape(CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal,
    size_t *out_projected_count)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    const CmHirFunctionParameter *parameter;
    const CmHirGenericParam *generic;
    const CmHirType *input_type;
    const CmHirType *input_child;
    const CmHirType *output_type;
    const CmHirType *output_array;
    const CmHirType *output_element;
    const CmHirType *length_type;
    const CmHirLibraryOwnedValue *owned;
    const CmHirBody *body;
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    const CmAstFunctionParam *ast_parameter;
    const CmAstPattern *ast_pattern;
    const CmAstType *ast_input;
    const CmAstType *ast_output;
    const CmAstExpr *ast_body;
    uint32_t namespace_module = 0u;
    uint32_t namespace_ordinal = 0u;
    uint32_t attribute_index;
    if (!cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || item->predicate_scope_count != 0u || item->predicate_scopes != NULL
        || item->predicate_count != 0u || item->predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->data.function_item.has_default_body
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->parameter_count != 1u || signature->parameters == NULL
        || signature->safety != CM_HIR_SAFE || !signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_decl_string_is(state->hir, signature->abi, "Rust")
        || !cm_decl_generics_shape(state, item)
        || !cm_decl_free_value_source(state, item, CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_VALUE_FUNCTION, &namespace_module,
            &namespace_ordinal)
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_FUNCTION,
            &module, out_ordinal, &effective, &ast, &ast_item)
        || module == NULL || ast == NULL || ast_item == NULL
        || namespace_module != module->local
        || namespace_ordinal != *out_ordinal
        || !cm_decl_from_mut_attributes(state, item,
            out_projected_count)) return 0;
    *out_module = module->local;
    for (attribute_index = 0u; attribute_index < effective.attribute_count;
            ++attribute_index) {
        CmResolveEffectiveAttribute graph_attribute;
        if (cm_module_graph_get_effective_item_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, effective.id, attribute_index,
                &graph_attribute) != CM_RESOLVE_VIEW_OK
            || !cm_decl_effective_attribute_matches_hir(state,
                &graph_attribute, &item->attributes[attribute_index],
                effective.declaration)) return 0;
    }
    generic = cm_hir_get_generic_param(state->hir,
        item->generic_parameter_start);
    parameter = &signature->parameters[0];
    input_type = cm_hir_get_type(state->hir, parameter->type);
    input_child = input_type == NULL
            || input_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(state->hir,
            input_type->data.reference_type.pointee);
    output_type = cm_hir_get_type(state->hir, signature->return_type);
    output_array = output_type == NULL
            || output_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        ? NULL : cm_hir_get_type(state->hir,
            output_type->data.reference_type.pointee);
    output_element = output_array == NULL
            || output_array->kind != CM_HIR_TYPE_ARRAY_KIND
        ? NULL : cm_hir_get_type(state->hir,
            output_array->data.array_type.element);
    length_type = output_array == NULL
            || output_array->kind != CM_HIR_TYPE_ARRAY_KIND
            || output_array->data.array_type.length.kind != CM_HIR_CONST_VALUE
        ? NULL : cm_hir_get_type(state->hir,
            output_array->data.array_type.length.type);
    ast_parameter = ast_item->data.function_item.parameter_count == 1u
            && ast_item->data.function_item.parameters != NULL
        ? &ast_item->data.function_item.parameters[0] : NULL;
    ast_pattern = ast_parameter == NULL ? NULL
        : cm_ast_get_pattern(ast, ast_parameter->pattern);
    ast_input = ast_parameter == NULL ? NULL
        : cm_ast_get_type(ast, ast_parameter->type);
    ast_output = cm_ast_get_type(ast,
        ast_item->data.function_item.return_type);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || generic->index != 0u
        || !cm_hir_def_id_equal(generic->owner, item->definition)
        || generic->declared_type != CM_HIR_TYPE_NONE
        || generic->is_relaxed_sized || generic->has_default
        || generic->span.source != item->span.source
        || generic->span.start != item->span.start
        || generic->span.end != item->span.end
        || ast_item->generic_parameter_count != 1u
        || ast_item->generic_parameters == NULL
        || ast_item->generic_parameters[0].kind != CM_AST_PARAM_TYPE
        || ast_item->generic_parameters[0].constraint != CM_INTERN_ID_NONE
        || ast_item->generic_parameters[0].bound_count != 0u
        || ast_item->generic_parameters[0].bounds != NULL
        || ast_item->generic_parameters[0].attributes != NULL
        || ast_item->generic_parameters[0].attribute_count != 0u
        || ast_item->generic_parameters[0].declared_type != CM_AST_TYPE_NONE
        || ast_item->generic_parameters[0].default_type != CM_AST_TYPE_NONE
        || ast_item->generic_parameters[0].default_const != CM_INTERN_ID_NONE
        || ast_item->generic_parameters[0].default_const_expr
            != CM_AST_EXPR_NONE
        || !cm_decl_ast_name_matches_hir(ast,
            ast_item->generic_parameters[0].name, state->hir, generic->name)
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->is_default
        || ast_item->data.function_item.abi != CM_INTERN_ID_NONE
        || !ast_item->data.function_item.is_const
        || ast_item->data.function_item.is_async
        || ast_item->data.function_item.is_safe
        || ast_item->data.function_item.is_unsafe
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicate_count != 0u
        || ast_item->where_predicates != NULL
        || ast_item->data.function_item.body == CM_AST_EXPR_NONE
        || ast_parameter == NULL || ast_parameter->is_self
        || ast_parameter->receiver_lifetime != CM_INTERN_ID_NONE
        || ast_input == NULL || ast_input->kind != CM_AST_TYPE_REFERENCE
        || ast_input->lifetime != CM_INTERN_ID_NONE
        || !ast_input->is_mutable
        || ast_output == NULL || ast_output->kind != CM_AST_TYPE_REFERENCE
        || ast_output->lifetime != CM_INTERN_ID_NONE
        || !ast_output->is_mutable
        || ast_pattern == NULL || ast_pattern->kind != CM_AST_PATTERN_BINDING
        || ast_pattern->data.binding.subpattern != CM_AST_PATTERN_NONE
        || ast_pattern->data.binding.is_ref
        || ast_pattern->data.binding.is_mutable
        || parameter->binding_kind != CM_HIR_BINDING_NAMED
        || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
        || !cm_decl_ast_name_matches_hir(ast,
            ast_pattern->data.binding.name, state->hir, parameter->name)
        || parameter->span.source != item->span.source
        || parameter->span.start != ast_pattern->span.start
        || parameter->span.end != ast_pattern->span.end
        || input_type == NULL || input_child == NULL
        || input_type->data.reference_type.mutability != CM_HIR_MUTABLE
        || input_type->data.reference_type.region.kind != CM_HIR_REGION_ERASED
        || input_child->kind != CM_HIR_TYPE_PARAMETER_KIND
        || input_child->data.parameter_type.parameter
            != item->generic_parameter_start
        || output_type == NULL || output_array == NULL
        || output_element == NULL || length_type == NULL
        || output_type->data.reference_type.mutability != CM_HIR_MUTABLE
        || output_type->data.reference_type.region.kind
            != CM_HIR_REGION_ERASED
        || output_element->kind != CM_HIR_TYPE_PARAMETER_KIND
        || output_element->data.parameter_type.parameter
            != item->generic_parameter_start
        || output_array->data.array_type.length.data.value.low_bits != 1u
        || output_array->data.array_type.length.data.value.high_bits != 0u
        || cm_decl_primitive(length_type) != CM_HIR_DECL_PRIMITIVE_USIZE
        || parameter->type == signature->return_type
        || !cm_decl_ast_type_matches_hir_field(state, ast, ast_input,
            input_type, item, 0u)
        || !cm_decl_ast_type_matches_hir_field(state, ast, ast_output,
            output_type, item, 0u)) return 0;
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || owned->declaration.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || !cm_hir_def_id_is_none(
            owned->declaration.data.function.parent_trait)
        || owned->declaration.data.function.receiver != CM_HIR_RECEIVER_NONE
        || owned->declaration.data.function.has_default_body
        || owned->parameter_count != 1u || owned->parameter_types == NULL
        || owned->declaration.data.function.parameter_count != 1u
        || owned->declaration.data.function.parameter_types
            != owned->parameter_types
        || owned->parameter_types[0] != parameter->type
        || owned->declaration.data.function.return_type
            != signature->return_type
        || owned->declaration.data.function.generic_parameter_start
            != item->generic_parameter_start
        || owned->declaration.data.function.generic_parameter_count != 1u
        || owned->declaration.data.function.predicate_scope_count != 0u
        || owned->predicate_scope_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->declaration.data.function.predicate_count != 0u
        || owned->predicate_count != 0u || owned->predicates != NULL
        || owned->predicate_arguments != NULL
        || owned->predicate_equalities != NULL
        || owned->predicate_lifetimes != NULL
        || owned->declaration.data.function.outlives_predicate_count != 0u
        || owned->outlives_predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->nominal_reference_count != 0u
        || owned->nominal_references != NULL
        || owned->nominal_reference_names != NULL
        || owned->nominal_reference_generic_kinds != NULL
        || owned->associated_availability_count != 0u
        || owned->associated_availability != NULL
        || owned->declaration.data.function.abi != signature->abi
        || owned->declaration.data.function.safety != signature->safety
        || !owned->declaration.data.function.is_const
        || owned->declaration.data.function.is_async
        || owned->declaration.data.function.is_variadic) return 0;
    body = cm_hir_get_body(state->hir, item->data.function_item.body);
    ast_body = cm_ast_get_expr(ast, ast_item->data.function_item.body);
    if (body == NULL || ast_body == NULL
        || ast_body->attribute_count != 0u || ast_body->attributes != NULL
        || ast_body->span.start > ast_body->span.end
        || ast_body->span.start < ast_item->span.start
        || ast_body->span.end > ast_item->span.end
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->origin.definition, item->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        || !cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        || body->state != CM_HIR_BODY_UNLOWERED
        || body->expected_type != signature->return_type
        || body->parameter_count != 1u
        || body->local_count != 1u || body->locals == NULL
        || body->locals[0].name != parameter->name
        || body->locals[0].type != parameter->type
        || body->locals[0].mutability != CM_HIR_IMMUTABLE
        || body->locals[0].parameter_index != 0u
        || body->locals[0].parameter_binding_index != 0u
        || body->locals[0].span.source != parameter->span.source
        || body->locals[0].span.start != parameter->span.start
        || body->locals[0].span.end != parameter->span.end
        || body->source != effective.declaration.source
        || body->source != item->span.source
        || body->source_expression_id != ast_item->data.function_item.body
        || body->root_expression != CM_HIR_EXPR_NONE
        || body->error_reason != CM_INTERN_ID_NONE
        || body->span.source != item->span.source
        || body->span.start != item->span.start
        || body->span.end != item->span.end) return 0;
    return 1;
}

static int cm_decl_legacy_function_shape(CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal,
    size_t *out_projected_count)
{
    const CmHirFunctionSignature *signature =
        &item->data.function_item.signature;
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    uint32_t namespace_module = 0u;
    uint32_t namespace_ordinal = 0u;
    *out_projected_count = 0u;
    if (!cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable || item->attribute_count != 0u
        || item->predicate_scope_count != 0u
        || item->outlives_predicate_count != 0u
        || item->generic_parameter_count == 0u || item->predicate_count == 0u
        || item->data.function_item.has_default_body
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_decl_generics_shape(state, item)
        || signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE || signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_decl_string_is(state->hir, signature->abi, "Rust")
        || ((signature->parameter_count == 0u)
            != (signature->parameters == NULL))
        || !cm_decl_free_value_source(state, item, CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_VALUE_FUNCTION, &namespace_module,
            &namespace_ordinal)
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_FUNCTION,
            &module, out_ordinal, &effective, &ast, &ast_item)
        || module == NULL || namespace_module != module->local
        || namespace_ordinal != *out_ordinal) return 0;
    *out_module = module->local;
    (void)effective;
    (void)ast;
    (void)ast_item;
    return 1;
}

static int cm_decl_function_shape(CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t *out_module, uint32_t *out_ordinal,
    size_t *out_projected_count)
{
    const CmHirFunctionSignature *signature;
    const CmHirLibraryOwnedValue *owned;
    const CmHirGenericParam *generic;
    const CmHirType *return_type;
    const CmHirType *return_child;
    const CmHirBody *body;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    const CmAstType *ast_return;
    const CmAstExpr *ast_body;
    const CmAstFunctionParam *ast_parameter = NULL;
    const CmAstPattern *ast_pattern = NULL;
    const CmAstType *ast_parameter_type = NULL;
    const CmHirFunctionParameter *parameter = NULL;
    const CmHirType *parameter_type = NULL;
    const CmHirType *parameter_child = NULL;
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    uint32_t namespace_module = 0u;
    uint32_t namespace_ordinal = 0u;
    if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
    signature = &item->data.function_item.signature;
    if (!signature->is_const && item->generic_parameter_count == 0u
        && item->predicate_count == 0u)
        return cm_decl_simple_unit_function_shape(state, item, out_module,
            out_ordinal, out_projected_count);
    if (!signature->is_const && item->generic_parameter_count == 3u
        && item->predicate_count == 1u)
        return cm_decl_from_fn_function_shape(state, item, out_module,
            out_ordinal, out_projected_count);
    if (!signature->is_const)
        return cm_decl_legacy_function_shape(state, item, out_module,
            out_ordinal, out_projected_count);
    if (item->generic_parameter_count == 1u
        && item->predicate_count == 0u && signature->parameter_count == 1u
        && cm_decl_from_mut_function_shape(state, item, out_module,
            out_ordinal, out_projected_count)) return 1;
    if (!cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->generic_parameter_count != 1u
        || !cm_decl_generics_shape(state, item)
        || (generic = cm_hir_get_generic_param(state->hir,
            item->generic_parameter_start)) == NULL
        || !generic->is_relaxed_sized
        || item->data.function_item.has_default_body
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || !cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)
        || !cm_decl_free_value_source(state, item, CM_AST_ITEM_FUNCTION,
            CM_HIR_LIBRARY_VALUE_FUNCTION, &namespace_module,
            &namespace_ordinal)
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_FUNCTION,
            &module, out_ordinal, &effective, &ast, &ast_item)
        || module == NULL || namespace_module != module->local
        || namespace_ordinal != *out_ordinal
        || !cm_decl_const_function_attributes(state, item,
            out_projected_count)
        || !cm_decl_ast_generic_shape(state, ast, ast_item, item)) return 0;
    *out_module = module->local;
    if (signature->receiver != CM_HIR_RECEIVER_NONE
        || signature->safety != CM_HIR_SAFE || !signature->is_const
        || signature->is_async || signature->is_variadic
        || !cm_decl_string_is(state->hir, signature->abi, "Rust")
        || signature->parameter_count > 1u
        || ((signature->parameter_count == 0u)
            != (signature->parameters == NULL))
        || ast_item->is_default
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || !ast_item->data.function_item.is_const
        || ast_item->data.function_item.is_async
        || ast_item->data.function_item.is_safe
        || ast_item->data.function_item.is_unsafe
        || ast_item->data.function_item.abi != CM_INTERN_ID_NONE
        || ast_item->data.function_item.body == CM_AST_EXPR_NONE
        || ast_item->data.function_item.parameter_count
            != signature->parameter_count
        || ((ast_item->data.function_item.parameter_count == 0u)
            != (ast_item->data.function_item.parameters == NULL))
        || ast_item->data.function_item.return_type == CM_AST_TYPE_NONE
        || (return_type = cm_hir_get_type(state->hir,
            signature->return_type)) == NULL
        || return_type->kind != CM_HIR_TYPE_REFERENCE_KIND
        || return_type->data.reference_type.mutability != CM_HIR_IMMUTABLE
        || return_type->data.reference_type.region.kind
            != CM_HIR_REGION_STATIC
        || (return_child = cm_hir_get_type(state->hir,
            return_type->data.reference_type.pointee)) == NULL
        || cm_decl_primitive(return_child) != CM_HIR_DECL_PRIMITIVE_STR
        || (ast_return = cm_ast_get_type(ast,
            ast_item->data.function_item.return_type)) == NULL
        || !cm_decl_ast_type_matches_hir_field(state, ast, ast_return,
            return_type, item, 0u)) return 0;
    if (signature->parameter_count == 1u) {
        parameter = &signature->parameters[0];
        ast_parameter = &ast_item->data.function_item.parameters[0];
        ast_pattern = cm_ast_get_pattern(ast, ast_parameter->pattern);
        ast_parameter_type = cm_ast_get_type(ast, ast_parameter->type);
        parameter_type = cm_hir_get_type(state->hir, parameter->type);
        parameter_child = parameter_type == NULL
                || parameter_type->kind != CM_HIR_TYPE_REFERENCE_KIND
            ? NULL : cm_hir_get_type(state->hir,
                parameter_type->data.reference_type.pointee);
        if (ast_parameter->is_self
            || ast_parameter->receiver_lifetime != CM_INTERN_ID_NONE
            || ast_parameter->type == CM_AST_TYPE_NONE
            || ast_pattern == NULL
            || ast_pattern->kind != CM_AST_PATTERN_BINDING
            || ast_pattern->data.binding.subpattern != CM_AST_PATTERN_NONE
            || ast_pattern->data.binding.is_ref
            || ast_pattern->data.binding.is_mutable
            || ast_parameter_type == NULL
            || ast_parameter_type->kind != CM_AST_TYPE_REFERENCE
            || ast_parameter_type->lifetime != CM_INTERN_ID_NONE
            || ast_parameter_type->is_mutable
            || parameter->name == CM_INTERN_ID_NONE
            || !cm_decl_ast_name_matches_hir(ast,
                ast_pattern->data.binding.name, state->hir, parameter->name)
            || parameter->span.source != item->span.source
            || parameter->span.start != ast_pattern->span.start
            || parameter->span.end != ast_pattern->span.end
            || parameter->binding_kind != CM_HIR_BINDING_NAMED
            || parameter->binding_mode != CM_HIR_PARAMETER_BINDING_MOVE
            || parameter_type == NULL
            || parameter_type->kind != CM_HIR_TYPE_REFERENCE_KIND
            || parameter_type->data.reference_type.mutability
                != CM_HIR_IMMUTABLE
            || parameter_type->data.reference_type.region.kind
                != CM_HIR_REGION_ERASED
            || parameter_type->data.reference_type.region.data
                .inference_variable != 0u
            || parameter_child == NULL
            || parameter_child->kind != CM_HIR_TYPE_PARAMETER_KIND
            || parameter_child->data.parameter_type.parameter
                != item->generic_parameter_start
            || !cm_decl_ast_type_matches_hir_field(state, ast,
                ast_parameter_type, parameter_type, item, 0u)) return 0;
    }
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || owned->declaration.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || !cm_hir_def_id_is_none(
            owned->declaration.data.function.parent_trait)
        || owned->declaration.data.function.receiver != CM_HIR_RECEIVER_NONE
        || owned->declaration.data.function.has_default_body
        || owned->declaration.data.function.parameter_count
            != signature->parameter_count
        || owned->parameter_count != signature->parameter_count
        || ((owned->parameter_count == 0u)
            != (owned->parameter_types == NULL))
        || owned->declaration.data.function.return_type
            != signature->return_type
        || owned->declaration.data.function.generic_parameter_start
            != item->generic_parameter_start
        || owned->declaration.data.function.generic_parameter_count != 1u
        || owned->declaration.data.function.predicate_scopes != NULL
        || owned->declaration.data.function.predicate_scope_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->predicate_scope_count != 0u
        || owned->declaration.data.function.predicates != NULL
        || owned->declaration.data.function.predicate_count != 0u
        || owned->predicates != NULL || owned->predicate_arguments != NULL
        || owned->predicate_equalities != NULL
        || owned->predicate_lifetimes != NULL || owned->predicate_count != 0u
        || owned->declaration.data.function.outlives_predicates != NULL
        || owned->declaration.data.function.outlives_predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->outlives_predicate_count != 0u
        || owned->nominal_references != NULL
        || owned->nominal_reference_names != NULL
        || owned->nominal_reference_generic_kinds != NULL
        || owned->nominal_reference_count != 0u
        || owned->associated_availability != NULL
        || owned->associated_availability_count != 0u
        || owned->declaration.data.function.abi != signature->abi
        || owned->declaration.data.function.safety != signature->safety
        || owned->declaration.data.function.is_const != signature->is_const
        || owned->declaration.data.function.is_async != signature->is_async
        || owned->declaration.data.function.is_variadic
            != signature->is_variadic
        || (signature->parameter_count == 1u
            && owned->parameter_types[0] != parameter->type)) return 0;
    body = cm_hir_get_body(state->hir, item->data.function_item.body);
    ast_body = cm_ast_get_expr(ast, ast_item->data.function_item.body);
    if (body == NULL || ast_body == NULL
        || ((ast_body->attribute_count == 0u)
            != (ast_body->attributes == NULL))
        || ast_body->attribute_count != 0u
        || ast_body->span.start > ast_body->span.end
        || ast_body->span.start < ast_item->span.start
        || ast_body->span.end > ast_item->span.end
        || !cm_hir_def_id_equal(body->owner, item->definition)
        || body->origin.kind != CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        || !cm_hir_def_id_equal(body->origin.definition, item->definition)
        || !cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        || !cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        || body->state != CM_HIR_BODY_UNLOWERED
        || body->expected_type != signature->return_type
        || body->parameter_count != signature->parameter_count
        || body->local_count != signature->parameter_count
        || ((body->local_count == 0u) != (body->locals == NULL))
        || body->source != effective.declaration.source
        || body->source != item->span.source
        || body->source_expression_id != ast_item->data.function_item.body
        || body->root_expression != CM_HIR_EXPR_NONE
        || body->error_reason != CM_INTERN_ID_NONE
        || body->span.source != item->span.source
        || body->span.start != item->span.start
        || body->span.end != item->span.end) return 0;
    if (signature->parameter_count == 1u
        && (body->locals[0].name != parameter->name
            || body->locals[0].type != parameter->type
            || body->locals[0].mutability != CM_HIR_IMMUTABLE
            || body->locals[0].span.source != parameter->span.source
            || body->locals[0].span.start != parameter->span.start
            || body->locals[0].span.end != parameter->span.end
            || body->locals[0].parameter_index != 0u
            || body->locals[0].parameter_binding_index != 0u)) return 0;
    return 1;
}

static int cm_decl_ast_path_is(const CmAst *ast, CmAstPathId id,
    const char *expected)
{
    const CmAstPath *path = cm_ast_get_path(ast, id);
    const CmInternedString *name;
    size_t length = strlen(expected);
    if (path == NULL || path->absolute || path->segment_count != 1u
        || path->segments == NULL || path->segments[0].argument_count != 0u
        || path->segments[0].arguments != NULL) return 0;
    name = cm_ast_get_string(ast, path->segments[0].name);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static const char *cm_decl_primitive_name(uint8_t primitive)
{
    switch (primitive) {
    case CM_HIR_DECL_PRIMITIVE_BOOL: return "bool";
    case CM_HIR_DECL_PRIMITIVE_CHAR: return "char";
    case CM_HIR_DECL_PRIMITIVE_STR: return "str";
    case CM_HIR_DECL_PRIMITIVE_I8: return "i8";
    case CM_HIR_DECL_PRIMITIVE_I16: return "i16";
    case CM_HIR_DECL_PRIMITIVE_I32: return "i32";
    case CM_HIR_DECL_PRIMITIVE_I64: return "i64";
    case CM_HIR_DECL_PRIMITIVE_I128: return "i128";
    case CM_HIR_DECL_PRIMITIVE_ISIZE: return "isize";
    case CM_HIR_DECL_PRIMITIVE_U8: return "u8";
    case CM_HIR_DECL_PRIMITIVE_U16: return "u16";
    case CM_HIR_DECL_PRIMITIVE_U32: return "u32";
    case CM_HIR_DECL_PRIMITIVE_U64: return "u64";
    case CM_HIR_DECL_PRIMITIVE_U128: return "u128";
    case CM_HIR_DECL_PRIMITIVE_USIZE: return "usize";
    case CM_HIR_DECL_PRIMITIVE_F32: return "f32";
    case CM_HIR_DECL_PRIMITIVE_F64: return "f64";
    default: return NULL;
    }
}

static int cm_decl_ast_type_matches_hir_primitive(const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type)
{
    uint8_t primitive = cm_decl_primitive(hir_type);
    const char *name;
    if (primitive == CM_HIR_DECL_PRIMITIVE_UNIT) {
        return ast_type != NULL && ast_type->kind == CM_AST_TYPE_TUPLE
            && ast_type->tuple_provenance == CM_AST_TUPLE_SOURCE
            && ast_type->elements == NULL && ast_type->element_count == 0u;
    }
    name = cm_decl_primitive_name(primitive);
    return name != NULL && ast_type != NULL
        && ast_type->kind == CM_AST_TYPE_PATH
        && cm_decl_ast_path_is(ast, ast_type->path, name);
}

static int cm_decl_ast_type_matches_hir_static(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type, size_t depth)
{
    uint32_t index;
    if (depth > CM_META_MAX_TYPE_NESTING || ast_type == NULL
        || hir_type == NULL || ast_type->span.start != hir_type->span.start
        || ast_type->span.end != hir_type->span.end) return 0;
    if (cm_decl_primitive(hir_type) != 0u)
        return cm_decl_ast_type_matches_hir_primitive(ast, ast_type, hir_type);
    if (hir_type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (ast_type->kind != CM_AST_TYPE_TUPLE
            || ast_type->tuple_provenance != CM_AST_TUPLE_SOURCE
            || hir_type->data.tuple_type.element_count == 0u
            || hir_type->data.tuple_type.element_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || ast_type->element_count
                != hir_type->data.tuple_type.element_count
            || ast_type->elements == NULL
            || hir_type->data.tuple_type.elements == NULL) return 0;
        for (index = 0u; index < ast_type->element_count; ++index) {
            const CmAstType *ast_child = cm_ast_get_type(ast,
                ast_type->elements[index]);
            const CmHirType *hir_child = cm_hir_get_type(state->hir,
                hir_type->data.tuple_type.elements[index]);
            if (!cm_decl_ast_type_matches_hir_static(state, ast, ast_child,
                    hir_child, depth + 1u)) return 0;
        }
        return 1;
    }
    if (hir_type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        const CmHirType *length_type;
        const CmInternedString *length_text;
        uint64_t length;
        if (ast_type->kind != CM_AST_TYPE_ARRAY
            || ast_type->child == CM_AST_TYPE_NONE
            || hir_type->data.array_type.element == CM_HIR_TYPE_NONE
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.array_type.element)) == NULL
            || (length_type = cm_hir_get_type(state->hir,
                hir_type->data.array_type.length.type)) == NULL
            || cm_decl_primitive(length_type) != CM_HIR_DECL_PRIMITIVE_USIZE
            || hir_type->data.array_type.length.kind != CM_HIR_CONST_VALUE
            || hir_type->data.array_type.length.data.value.high_bits != 0u
            || (length_text = cm_ast_get_string(ast, ast_type->text)) == NULL
            || !cm_decl_parse_u64_decimal(length_text, &length)
            || length
                != hir_type->data.array_type.length.data.value.low_bits)
            return 0;
        return cm_decl_ast_type_matches_hir_static(state, ast, ast_child,
            hir_child, depth + 1u);
    }
    return 0;
}

static int cm_decl_ast_name_matches_hir(const CmAst *ast, CmInternId ast_id,
    const CmHirContext *hir, CmInternId hir_id)
{
    const CmInternedString *ast_name = cm_ast_get_string(ast, ast_id);
    const CmInternedString *hir_name = cm_interner_get(&hir->strings, hir_id);
    return ast_name != NULL && hir_name != NULL
        && cm_decl_bytes_equal(ast_name->bytes, ast_name->len,
            hir_name->bytes, hir_name->len);
}

static int cm_decl_trait_attributes(const CmDeclCaptureState *state,
    const CmHirItem *item, int parent_trait,
    size_t *out_projected_count, int *out_const_trait,
    const unsigned char **out_diagnostic_item,
    size_t *out_diagnostic_item_length)
{
    unsigned int seen = 0u;
    uint32_t index;
    *out_projected_count = 0u;
    *out_const_trait = 0;
    if (out_diagnostic_item != NULL) *out_diagnostic_item = NULL;
    if (out_diagnostic_item_length != NULL)
        *out_diagnostic_item_length = 0u;
    if ((item->attribute_count == 0u) != (item->attributes == NULL)) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_UNSTABLE
                && kind != CM_DECL_ATTR_DEPRECATED
                && !(parent_trait && kind == CM_DECL_ATTR_CONST_TRAIT)
                && !(parent_trait && kind == CM_DECL_ATTR_DIAGNOSTIC_ITEM)
                && !(!parent_trait && item->kind == CM_HIR_ITEM_FUNCTION
                    && kind == CM_DECL_ATTR_INLINE_HINT))
            || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior) {
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        }
        seen |= kind;
        if (kind == CM_DECL_ATTR_DIAGNOSTIC_ITEM) {
            const unsigned char *name = NULL;
            size_t name_length = 0u;
            if (out_diagnostic_item == NULL
                || out_diagnostic_item_length == NULL
                || !cm_decl_diagnostic_item_name(metadata, &name,
                    &name_length)) return 0;
            *out_diagnostic_item = name;
            *out_diagnostic_item_length = name_length;
        } else if (kind != CM_DECL_ATTR_CONST_TRAIT) {
            if (*out_projected_count == SIZE_MAX) return 0;
            *out_projected_count += 1u;
        }
    }
    if ((seen & CM_DECL_ATTR_STABLE) != 0u
        && (seen & CM_DECL_ATTR_UNSTABLE) != 0u) return 0;
    *out_const_trait = (seen & CM_DECL_ATTR_CONST_TRAIT) != 0u;
    return 1;
}

/*
 * Exact source-relative projection for the first callable-trait closure.
 * Lang/const/paren/fundamental/coherence flags are retained structurally;
 * stability, diagnostic text, must-use text, and const-unstable metadata are
 * counted in the v3.0 SEMANTIC_ATTRIBUTES-ABSENT projection.
 */
static int cm_decl_callable_trait_attributes(
    const CmDeclCaptureState *state, const CmHirItem *item,
    size_t *out_projected_count, uint8_t *out_flags,
    const unsigned char **out_lang, size_t *out_lang_length)
{
    const unsigned int tuple_required = CM_DECL_ATTR_UNSTABLE
        | CM_DECL_ATTR_LANG_ITEM
        | CM_DECL_ATTR_ON_UNIMPLEMENTED
        | CM_DECL_ATTR_RUSTC_DENY_EXPLICIT_IMPL
        | CM_DECL_ATTR_RUSTC_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const unsigned int callable_required = CM_DECL_ATTR_STABLE
        | CM_DECL_ATTR_LANG_ITEM | CM_DECL_ATTR_RUSTC_PAREN_SUGAR
        | CM_DECL_ATTR_ON_UNIMPLEMENTED | CM_DECL_ATTR_FUNDAMENTAL
        | CM_DECL_ATTR_MUST_USE | CM_DECL_ATTR_CONST_TRAIT
        | CM_DECL_ATTR_RUSTC_CONST_UNSTABLE;
    unsigned int seen = 0u;
    int saw_diagnostic_on_unimplemented = 0;
    int saw_rustc_on_unimplemented = 0;
    uint32_t index;
    *out_projected_count = 0u;
    *out_flags = 0u;
    *out_lang = NULL;
    *out_lang_length = 0u;
    if (item->attribute_count == 0u || item->attributes == NULL) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        uint32_t prior;
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source == 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || kind == 0u || (seen & kind) != 0u) return 0;
        for (prior = 0u; prior < index; ++prior)
            if (item->attributes[prior].span.source == attribute->span.source
                && item->attributes[prior].source_attribute
                    == attribute->source_attribute) return 0;
        if (kind == CM_DECL_ATTR_LANG_ITEM) {
            if (!cm_decl_lang_item_name(metadata, out_lang,
                    out_lang_length)) return 0;
        }
        if (kind == CM_DECL_ATTR_ON_UNIMPLEMENTED) {
            saw_diagnostic_on_unimplemented = cm_decl_attribute_call_is(
                metadata, "diagnostic::on_unimplemented");
            saw_rustc_on_unimplemented = cm_decl_attribute_call_is(metadata,
                "rustc_on_unimplemented");
            if (saw_diagnostic_on_unimplemented
                    == saw_rustc_on_unimplemented) return 0;
        }
        seen |= kind;
    }
    if (seen == tuple_required && saw_diagnostic_on_unimplemented
        && !saw_rustc_on_unimplemented) {
        *out_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
            | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
            | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
        *out_projected_count = 2u;
        return 1;
    }
    if (seen == callable_required && saw_rustc_on_unimplemented
        && !saw_diagnostic_on_unimplemented) {
        *out_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
            | CM_HIR_DECL_TRAIT_IS_CONST
            | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
            | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
        *out_projected_count = 4u;
        return 1;
    }
    return 0;
}

static int cm_decl_associated_type_attributes(
    const CmDeclCaptureState *state, const CmHirItem *item,
    size_t *out_projected_count, const unsigned char **out_lang,
    size_t *out_lang_length)
{
    unsigned int seen = 0u;
    uint32_t index;
    *out_projected_count = 0u;
    *out_lang = NULL;
    *out_lang_length = 0u;
    if (item->kind != CM_HIR_ITEM_TYPE_ALIAS
        || item->attribute_count != 2u || item->attributes == NULL) return 0;
    for (index = 0u; index < item->attribute_count; ++index) {
        const CmHirAttribute *attribute = &item->attributes[index];
        const CmInternedString *metadata = cm_interner_get(
            &state->hir->strings, attribute->metadata);
        unsigned int kind = cm_decl_attribute_kind(metadata);
        if (attribute->source_attribute == 0u
            || attribute->expansion_depth != 0u
            || attribute->span.source != item->span.source
            || attribute->span.start > attribute->span.end
            || (kind != CM_DECL_ATTR_STABLE
                && kind != CM_DECL_ATTR_LANG_ITEM)
            || (seen & kind) != 0u) return 0;
        if (kind == CM_DECL_ATTR_LANG_ITEM
            && !cm_decl_lang_item_name(metadata, out_lang,
                out_lang_length)) return 0;
        seen |= kind;
    }
    if (seen != (CM_DECL_ATTR_STABLE | CM_DECL_ATTR_LANG_ITEM)) return 0;
    *out_projected_count = 1u;
    return 1;
}

static CmHirItemKind cm_decl_hir_kind_for_ast(CmAstItemKind kind)
{
    switch (kind) {
    case CM_AST_ITEM_FUNCTION: return CM_HIR_ITEM_FUNCTION;
    case CM_AST_ITEM_TYPE_ALIAS: return CM_HIR_ITEM_TYPE_ALIAS;
    case CM_AST_ITEM_CONST: return CM_HIR_ITEM_CONST;
    default: return CM_HIR_ITEM_NONE;
    }
}

static int cm_decl_trait_member_source_shape(
    const CmDeclCaptureState *state, const CmDeclCaptureItem *parent,
    const CmResolveEffectiveItem *effective, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item, CmHirItemId item_id,
    size_t *out_projected_count)
{
    const CmHirDefinition *definition = cm_hir_lookup_definition(state->hir,
        item->definition);
    const CmDeclCaptureModule *module = cm_decl_module_by_local(
        (CmDeclCaptureState *)state, parent->owner_module);
    int ignored_const_trait;
    const unsigned char *ignored_lang = NULL;
    size_t ignored_lang_length = 0u;
    uint32_t attribute_index;
    if (module == NULL || effective->is_generated
        || effective->id == CM_RESOLVE_EFFECTIVE_ITEM_NONE
        || effective->child_kind != CM_EXPANDED_CHILD_NONE
        || effective->child_count != 0u
        || effective->item_kind != ast_item->kind
        || effective->visibility != CM_AST_VIS_INHERITED
        || effective->declaration.source != item->span.source
        || effective->span.source != item->span.source
        || effective->span.start != item->span.start
        || effective->span.end != item->span.end
        || ast_item->span.start != item->span.start
        || ast_item->span.end != item->span.end
        || ast_item->visibility.kind != CM_AST_VIS_INHERITED
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || item->kind != cm_decl_hir_kind_for_ast(ast_item->kind)
        || item->definition.crate_id != state->input->crate_id
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_equal(item->parent_definition,
            parent->item->definition)
        || item->owner_module != parent->item->owner_module
        || item->visibility.kind != CM_HIR_VIS_PRIVATE
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || item->is_specializable != ast_item->is_default
        || !cm_decl_ast_name_matches_hir(ast, ast_item->name,
            state->hir, item->name)
        || effective->attribute_count != item->attribute_count
        || definition == NULL || definition->state != CM_HIR_DEFINITION_BOUND
        || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->entity.item_id != item_id
        || definition->span.source != item->span.source
        || definition->span.start != item->span.start
        || definition->span.end != item->span.end
        || (item->kind == CM_HIR_ITEM_TYPE_ALIAS
            ? !cm_decl_associated_type_attributes(state, item,
                out_projected_count, &ignored_lang, &ignored_lang_length)
            : (!cm_decl_trait_attributes(state, item, 0,
                out_projected_count, &ignored_const_trait, NULL, NULL)
                || ignored_const_trait))) return 0;
    (void)ignored_lang;
    (void)ignored_lang_length;
    for (attribute_index = 0u; attribute_index < effective->attribute_count;
            ++attribute_index) {
        CmResolveEffectiveAttribute graph_attribute;
        if (cm_module_graph_get_effective_item_attribute(
                state->input->graph, state->input->revision,
                module->graph.id,
                effective->id, attribute_index, &graph_attribute)
                != CM_RESOLVE_VIEW_OK
            || !cm_decl_effective_attribute_matches_hir(state,
                &graph_attribute, &item->attributes[attribute_index],
                effective->declaration)) return 0;
    }
    if (item->kind == CM_HIR_ITEM_FUNCTION) {
        const CmHirFunctionSignature *signature =
            &item->data.function_item.signature;
        const CmInternedString *ast_abi =
            ast_item->data.function_item.abi == CM_INTERN_ID_NONE
                ? NULL : cm_ast_get_string(ast,
                    ast_item->data.function_item.abi);
        const CmHirLibraryOwnedValue *owned = cm_decl_owned_value(
            state->owned, item->definition);
        uint32_t parameter_index;
        uint32_t predicate_index;
        if ((ast_item->data.function_item.is_safe
                && ast_item->data.function_item.is_unsafe)
            || signature->safety != (ast_item->data.function_item.is_unsafe
                ? CM_HIR_UNSAFE : CM_HIR_SAFE)
            || (ast_abi == NULL
                ? !cm_decl_string_is(state->hir, signature->abi, "Rust")
                : (!cm_decl_bytes_equal(ast_abi->bytes, ast_abi->len,
                        (const unsigned char *)"rust-call", 9u)
                    || !cm_decl_string_is(state->hir, signature->abi,
                        "rust-call")))
            || signature->is_const != ast_item->data.function_item.is_const
            || signature->is_async != ast_item->data.function_item.is_async
            || item->data.function_item.has_default_body
                != (ast_item->data.function_item.body != CM_AST_EXPR_NONE)
            || (item->data.function_item.body != CM_HIR_BODY_NONE)
                != (ast_item->data.function_item.body != CM_AST_EXPR_NONE)
            || !cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            || ast_item->generic_parameter_count
                != item->generic_parameter_count
            || ((ast_item->generic_parameter_count == 0u)
                != (ast_item->generic_parameters == NULL))
            || ast_item->data.function_item.parameter_count
                != signature->parameter_count
            || ((signature->parameter_count == 0u)
                != (signature->parameters == NULL))
            || ((ast_item->data.function_item.parameter_count == 0u)
                != (ast_item->data.function_item.parameters == NULL))
            || ast_item->where_predicate_count != item->predicate_count
            || (ast_item->where_clause == CM_INTERN_ID_NONE)
                != (ast_item->where_predicate_count == 0u)
            || ((ast_item->where_predicate_count == 0u)
                != (ast_item->where_predicates == NULL))) return 0;
        if (parent->item->visibility.kind == CM_HIR_VIS_PUBLIC
            && (owned == NULL
            || owned->storage_kind != CM_HIR_LIBRARY_VALUE_FUNCTION
            || owned->declaration.kind != CM_HIR_LIBRARY_VALUE_FUNCTION
            || !cm_hir_def_id_equal(owned->declaration.definition,
                item->definition)
            || !cm_hir_def_id_equal(
                owned->declaration.data.function.parent_trait,
                parent->item->definition)
            || owned->declaration.data.function.receiver
                != signature->receiver
            || owned->declaration.data.function.has_default_body
                != item->data.function_item.has_default_body
            || owned->declaration.data.function.parameter_count
                != signature->parameter_count
            || owned->parameter_count != signature->parameter_count
            || ((owned->parameter_count == 0u)
                != (owned->parameter_types == NULL))
            || owned->declaration.data.function.return_type
                != signature->return_type
            || owned->declaration.data.function.generic_parameter_start
                != item->generic_parameter_start
            || owned->declaration.data.function.generic_parameter_count
                != item->generic_parameter_count
            || owned->declaration.data.function.predicate_scope_count
                != item->predicate_scope_count
            || owned->declaration.data.function.predicate_count
                != item->predicate_count
            || owned->predicate_count != item->predicate_count
            || ((owned->predicate_count == 0u)
                != (owned->predicates == NULL))
            || owned->declaration.data.function.outlives_predicate_count
                != item->outlives_predicate_count
            || owned->declaration.data.function.abi != signature->abi
            || owned->declaration.data.function.safety != signature->safety
            || owned->declaration.data.function.is_const
                != signature->is_const
            || owned->declaration.data.function.is_async
                != signature->is_async
            || owned->declaration.data.function.is_variadic
                != signature->is_variadic)) return 0;
        for (parameter_index = 0u;
                parameter_index < signature->parameter_count;
                ++parameter_index) {
            const CmAstFunctionParam *ast_parameter =
                &ast_item->data.function_item.parameters[parameter_index];
            const CmHirType *hir_type = cm_hir_get_type(state->hir,
                signature->parameters[parameter_index].type);
            if ((parent->item->visibility.kind == CM_HIR_VIS_PUBLIC
                    && owned->parameter_types[parameter_index]
                        != signature->parameters[parameter_index].type)
                || ast_parameter->receiver_lifetime != CM_INTERN_ID_NONE)
                return 0;
            if (parameter_index == 0u
                && signature->receiver != CM_HIR_RECEIVER_NONE) {
                if (!ast_parameter->is_self
                    || ast_parameter->type != CM_AST_TYPE_NONE
                    || hir_type == NULL) return 0;
            } else {
                const CmAstType *ast_type;
                if (ast_parameter->is_self
                    || ast_parameter->type == CM_AST_TYPE_NONE
                    || (ast_type = cm_ast_get_type(ast,
                        ast_parameter->type)) == NULL
                    || !cm_decl_ast_type_matches_hir_field(state, ast,
                        ast_type, hir_type, item, 0u)) return 0;
            }
        }
        {
            const CmHirType *hir_return = cm_hir_get_type(state->hir,
                signature->return_type);
            if (ast_item->data.function_item.return_type
                    == CM_AST_TYPE_NONE) {
                if (cm_decl_primitive(hir_return)
                        != CM_HIR_DECL_PRIMITIVE_UNIT) return 0;
            } else {
                const CmAstType *ast_return = cm_ast_get_type(ast,
                    ast_item->data.function_item.return_type);
                if (!cm_decl_ast_type_matches_hir_field(state, ast,
                        ast_return, hir_return, item, 0u)) return 0;
            }
        }
        for (predicate_index = 0u;
                predicate_index < item->predicate_count;
                ++predicate_index) {
            const CmAstWherePredicate *ast_predicate =
                &ast_item->where_predicates[predicate_index];
            const CmHirTraitPredicate *hir_predicate =
                &item->predicates[predicate_index];
            const CmHirTraitPredicate *owned_predicate =
                &owned->predicates[predicate_index];
            const CmAstType *ast_subject = cm_ast_get_type(ast,
                ast_predicate->subject);
            const CmHirType *hir_subject = cm_hir_get_type(state->hir,
                hir_predicate->subject);
            const CmAstWhereBound *ast_bound;
            const CmAstType *ast_trait_type;
            const CmAstPath *ast_trait_path;
            const CmHirItem *hir_trait;
            CmHirItemId hir_trait_id;
            if (ast_predicate->kind != CM_AST_WHERE_PREDICATE_TYPE
                || ast_predicate->binder.lifetime_count != 0u
                || ast_predicate->binder.lifetimes != NULL
                || ast_predicate->bound_count != 1u
                || ast_predicate->bounds == NULL
                || !cm_decl_ast_type_matches_hir_field(state, ast,
                    ast_subject, hir_subject, item, 0u)
                || hir_predicate->trait_type.argument_count != 0u
                || hir_predicate->trait_type.arguments != NULL
                || hir_predicate->span.source != item->span.source
                || hir_predicate->span.start != ast_predicate->span.start
                || hir_predicate->span.end != ast_predicate->span.end
                || owned_predicate->scope != hir_predicate->scope
                || owned_predicate->modifier != hir_predicate->modifier
                || owned_predicate->subject != hir_predicate->subject
                || !cm_hir_def_id_equal(
                    owned_predicate->trait_type.definition,
                    hir_predicate->trait_type.definition)
                || owned_predicate->trait_type.argument_count != 0u
                || owned_predicate->trait_type.arguments != NULL
                || owned_predicate->equality_count != 0u
                || owned_predicate->equalities != NULL
                || owned_predicate->binder.lifetime_count != 0u
                || owned_predicate->binder.lifetimes != NULL
                || owned_predicate->span.source != hir_predicate->span.source
                || owned_predicate->span.start != hir_predicate->span.start
                || owned_predicate->span.end != hir_predicate->span.end
                || (hir_trait = cm_decl_bound_item(state->hir,
                    hir_predicate->trait_type.definition,
                    &hir_trait_id)) == NULL
                || hir_trait->kind != CM_HIR_ITEM_TRAIT) return 0;
            ast_bound = &ast_predicate->bounds[0];
            ast_trait_type = cm_ast_get_type(ast, ast_bound->trait_type);
            ast_trait_path = ast_trait_type == NULL
                    || ast_trait_type->kind != CM_AST_TYPE_PATH
                ? NULL : cm_ast_get_path(ast, ast_trait_type->path);
            if (ast_bound->kind != CM_AST_WHERE_BOUND_TRAIT
                || ast_bound->modifier != CM_AST_WHERE_BOUND_REQUIRED
                || ast_bound->binder.lifetime_count != 0u
                || ast_bound->binder.lifetimes != NULL
                || ast_bound->lifetime != CM_INTERN_ID_NONE
                || ast_trait_path == NULL || ast_trait_path->absolute
                || ast_trait_path->segment_count != 1u
                || ast_trait_path->segments == NULL
                || ast_trait_path->segments[0].argument_count != 0u
                || ast_trait_path->segments[0].arguments != NULL
                || !cm_decl_ast_name_matches_hir(ast,
                    ast_trait_path->segments[0].name, state->hir,
                    hir_trait->name)) return 0;
        }
    } else if (item->kind == CM_HIR_ITEM_CONST) {
        if (ast_item->data.value_item.is_mutable
            || item->data.value_item.mutability != CM_HIR_IMMUTABLE
            || item->data.value_item.has_default_body
                != ast_item->data.value_item.has_value
            || !cm_hir_def_id_is_none(
                item->data.value_item.trait_item_definition)) return 0;
    } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
        if ((item->data.type_alias_item.target != CM_HIR_TYPE_NONE)
                != ast_item->data.value_item.has_value
            || !cm_hir_def_id_is_none(
                item->data.type_alias_item.trait_item_definition)) return 0;
    } else return 0;
    return 1;
}

static int cm_decl_trait_static_outlives_source(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item,
    int *out_has_static)
{
    const CmAstSupertrait *bound;
    const CmInternedString *lifetime;
    const CmHirOutlivesPredicate *predicate;
    const CmHirType *subject;
    *out_has_static = 0;
    if ((ast_item->data.trait_item.structured_supertrait_count == 0u)
            != (ast_item->data.trait_item.structured_supertraits == NULL)
        || (item->data.trait_item.supertrait_count == 0u)
            != (item->data.trait_item.supertraits == NULL)
        || (item->outlives_predicate_count == 0u)
            != (item->outlives_predicates == NULL)
        || item->predicate_scope_count != 0u
        || item->predicate_scopes != NULL
        || item->predicate_count != 0u || item->predicates != NULL
        || item->data.trait_item.supertrait_count != 0u) return 0;
    if (ast_item->data.trait_item.structured_supertrait_count == 0u)
        return ast_item->data.trait_item.supertraits == CM_INTERN_ID_NONE
            && item->outlives_predicate_count == 0u;
    if (ast_item->data.trait_item.structured_supertrait_count != 1u
        || ast_item->data.trait_item.supertraits == CM_INTERN_ID_NONE
        || item->outlives_predicate_count != 1u) return 0;
    bound = &ast_item->data.trait_item.structured_supertraits[0];
    lifetime = cm_ast_get_string(ast, bound->lifetime);
    predicate = &item->outlives_predicates[0];
    subject = cm_hir_get_type(state->hir, predicate->subject.type);
    if (bound->kind != CM_AST_SUPERTRAIT_LIFETIME
        || bound->modifier != CM_AST_SUPERTRAIT_REQUIRED
        || bound->type != CM_AST_TYPE_NONE
        || bound->lifetime == CM_INTERN_ID_NONE
        || lifetime == NULL
        || !cm_decl_bytes_equal(lifetime->bytes, lifetime->len,
            (const unsigned char *)"'static", 7u)
        || bound->span.start > bound->span.end
        || predicate->subject_kind != CM_HIR_OUTLIVES_TYPE
        || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->bound.kind != CM_HIR_REGION_STATIC
        || predicate->span.source != item->span.source
        || predicate->span.start != bound->span.start
        || predicate->span.end != bound->span.end
        || subject == NULL || subject->kind != CM_HIR_TYPE_SELF_KIND
        || !cm_hir_def_id_equal(subject->data.self_type.owner,
            item->definition)
        || subject->span.source != predicate->span.source
        || subject->span.start != predicate->span.start
        || subject->span.end != predicate->span.end) return 0;
    *out_has_static = 1;
    return 1;
}

static int cm_decl_callable_trait_header_source(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item,
    const CmDeclCaptureItem *capture)
{
    const uint8_t tuple_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint8_t callable_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    const CmAstGenericParam *ast_generic;
    const CmAstGenericParamBound *ast_bound;
    const CmHirGenericParam *generic;
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmHirItem *bound_trait;
    const CmAstType *ast_bound_type;
    const CmAstPath *ast_bound_path;
    CmHirItemId ignored_id;
    if (item->predicate_scope_count != 0u || item->predicate_scopes != NULL
        || item->outlives_predicate_count != 0u
        || item->outlives_predicates != NULL) return 0;
    if (capture->trait_flags == tuple_flags) {
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
            && item->generic_parameter_count == 0u
            && ast_item->generic_parameters == NULL
            && ast_item->generic_parameter_count == 0u
            && item->predicates == NULL && item->predicate_count == 0u
            && item->data.trait_item.supertraits == NULL
            && item->data.trait_item.supertrait_count == 0u
            && ast_item->data.trait_item.supertraits == CM_INTERN_ID_NONE
            && ast_item->data.trait_item.structured_supertraits == NULL
            && ast_item->data.trait_item.structured_supertrait_count == 0u
            && item->data.trait_item.safety == CM_HIR_SAFE
            && !item->data.trait_item.is_auto
            && !item->data.trait_item.is_const;
    }
    if (capture->trait_flags != callable_flags
        || item->generic_parameter_count != 1u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || ast_item->generic_parameter_count != 1u
        || ast_item->generic_parameters == NULL
        || item->predicate_count != 1u || item->predicates == NULL
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicate_count != 0u
        || ast_item->where_predicates != NULL
        || item->data.trait_item.safety != CM_HIR_SAFE
        || item->data.trait_item.is_auto || !item->data.trait_item.is_const)
        return 0;
    ast_generic = &ast_item->generic_parameters[0];
    generic = cm_hir_get_generic_param(state->hir,
        item->generic_parameter_start);
    predicate = &item->predicates[0];
    subject = cm_hir_get_type(state->hir, predicate->subject);
    ast_bound = ast_generic->bounds;
    ast_bound_type = ast_bound == NULL ? NULL
        : cm_ast_get_type(ast, ast_bound->trait_type);
    ast_bound_path = ast_bound_type == NULL
            || ast_bound_type->kind != CM_AST_TYPE_PATH
        ? NULL : cm_ast_get_path(ast, ast_bound_type->path);
    bound_trait = cm_decl_bound_item(state->hir,
        predicate->trait_type.definition, &ignored_id);
    if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
        || generic->index != 0u
        || !cm_hir_def_id_equal(generic->owner, item->definition)
        || generic->declared_type != CM_HIR_TYPE_NONE
        || generic->is_relaxed_sized || generic->has_default
        || ast_generic->kind != CM_AST_PARAM_TYPE
        || ast_generic->attributes != NULL
        || ast_generic->attribute_count != 0u
        || ast_generic->constraint == CM_INTERN_ID_NONE
        || ast_generic->bound_count != 1u || ast_generic->bounds == NULL
        || ast_generic->declared_type != CM_AST_TYPE_NONE
        || ast_generic->default_type != CM_AST_TYPE_NONE
        || ast_generic->default_const != CM_INTERN_ID_NONE
        || ast_generic->default_const_expr != CM_AST_EXPR_NONE
        || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
            state->hir, generic->name)
        || ast_bound->kind != CM_AST_GENERIC_BOUND_TRAIT
        || ast_bound->modifier != CM_AST_GENERIC_BOUND_REQUIRED
        || ast_bound->lifetime != CM_INTERN_ID_NONE
        || ast_bound_path == NULL || bound_trait == NULL
        || bound_trait->kind != CM_HIR_ITEM_TRAIT
        || !cm_decl_ast_path_resolves_item(state, ast, ast_bound_path,
            item, bound_trait)
        || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
        || predicate->binder.lifetime_count != 0u
        || predicate->binder.lifetimes != NULL
        || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
        || predicate->trait_type.argument_count != 0u
        || predicate->trait_type.arguments != NULL
        || predicate->equality_count != 0u || predicate->equalities != NULL
        || subject == NULL || subject->kind != CM_HIR_TYPE_PARAMETER_KIND
        || subject->data.parameter_type.parameter
            != item->generic_parameter_start
        || predicate->span.source != item->span.source
        || predicate->span.start != ast_bound->span.start
        || predicate->span.end != ast_bound->span.end) return 0;
    if (item->data.trait_item.supertrait_count == 0u) {
        return item->data.trait_item.supertraits == NULL
            && ast_item->data.trait_item.supertraits == CM_INTERN_ID_NONE
            && ast_item->data.trait_item.structured_supertraits == NULL
            && ast_item->data.trait_item.structured_supertrait_count == 0u;
    }
    if (item->data.trait_item.supertrait_count == 1u
        && item->data.trait_item.supertraits != NULL
        && ast_item->data.trait_item.supertraits != CM_INTERN_ID_NONE
        && ast_item->data.trait_item.structured_supertrait_count == 1u
        && ast_item->data.trait_item.structured_supertraits != NULL) {
        const CmHirSupertrait *supertrait =
            &item->data.trait_item.supertraits[0];
        const CmAstSupertrait *ast_supertrait =
            &ast_item->data.trait_item.structured_supertraits[0];
        const CmAstType *ast_supertrait_type = cm_ast_get_type(ast,
            ast_supertrait->type);
        const CmAstPath *ast_supertrait_path = ast_supertrait_type == NULL
                || ast_supertrait_type->kind != CM_AST_TYPE_PATH
            ? NULL : cm_ast_get_path(ast, ast_supertrait_type->path);
        const CmHirItem *supertrait_item = cm_decl_bound_item(state->hir,
            supertrait->trait_type.definition, &ignored_id);
        const CmAstPathSegment *segment = ast_supertrait_path == NULL
                || ast_supertrait_path->segment_count == 0u
                || ast_supertrait_path->segments == NULL
            ? NULL : &ast_supertrait_path->segments[
                ast_supertrait_path->segment_count - 1u];
        const CmAstGenericArg *ast_argument = segment == NULL
                || segment->argument_count != 1u
                || segment->arguments == NULL
            ? NULL : &segment->arguments[0];
        const CmAstType *ast_argument_type = ast_argument == NULL
            ? NULL : cm_ast_get_type(ast, ast_argument->type);
        const CmHirType *hir_argument =
            supertrait->trait_type.argument_count != 1u
                || supertrait->trait_type.arguments == NULL
                || supertrait->trait_type.arguments[0].kind
                    != CM_HIR_GENERIC_ARG_TYPE
            ? NULL : cm_hir_get_type(state->hir,
                supertrait->trait_type.arguments[0].data.type);
        return ast_supertrait->kind == CM_AST_SUPERTRAIT_TRAIT
            && ast_supertrait->modifier == CM_AST_SUPERTRAIT_REQUIRED
            && ast_supertrait->lifetime == CM_INTERN_ID_NONE
            && ast_supertrait_path != NULL && supertrait_item != NULL
            && supertrait_item->kind == CM_HIR_ITEM_TRAIT
            && cm_decl_ast_path_resolves_item(state, ast,
                ast_supertrait_path, item, supertrait_item)
            && supertrait->modifier == CM_HIR_SUPERTRAIT_REQUIRED
            && supertrait->equality_count == 0u
            && supertrait->equalities == NULL
            && supertrait->span.source == item->span.source
            && supertrait->span.start == ast_supertrait->span.start
            && supertrait->span.end == ast_supertrait->span.end
            && ast_argument != NULL
            && ast_argument->kind == CM_AST_GENERIC_TYPE
            && ast_argument->name == CM_INTERN_ID_NONE
            && ast_argument->name_arguments == NULL
            && ast_argument->name_argument_count == 0u
            && ast_argument->bounds == NULL
            && ast_argument->bound_count == 0u
            && ast_argument_type != NULL
            && hir_argument != NULL
            && hir_argument->kind == CM_HIR_TYPE_PARAMETER_KIND
            && hir_argument->data.parameter_type.parameter
                == item->generic_parameter_start
            && cm_decl_ast_type_matches_hir_field(state, ast,
                ast_argument_type, hir_argument, item, 0u);
    }
    return 0;
}

static int cm_decl_trait_source_and_members(CmDeclCaptureState *state,
    CmDeclCaptureItem *capture, size_t *out_projected_count)
{
    const CmHirItem *item = capture->item;
    const CmInternedString *item_name = cm_decl_item_name(state, item);
    const CmDeclCaptureNamespace *source = NULL;
    CmDeclCaptureModule *module;
    CmResolveEffectiveItem effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    size_t direct_count = 0u;
    size_t hir_child_count = 0u;
    size_t projected_count;
    size_t namespace_index;
    size_t hir_index;
    uint32_t child_index;
    uint32_t attribute_index;
    uint32_t prior_raw_index = 0u;
    int const_trait;
    int has_static_outlives;
    int callable_attributes = 0;
    if (item_name == NULL || item_name->len == 0u) return 0;
    for (namespace_index = 0u; namespace_index < state->namespace_count;
            ++namespace_index) {
        const CmDeclCaptureNamespace *entry =
            &state->namespace_values[namespace_index];
        if (!cm_hir_def_id_equal(entry->target.definition,
                item->definition) || entry->is_import) continue;
        if (entry->target.kind != CM_HIR_LIBRARY_BINDING_TRAIT
            || entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
            || entry->item_kind != CM_AST_ITEM_TRAIT
            || entry->source_is_generated
            || !cm_decl_item_ref_equal(entry->declaration,
                entry->introduced_by)
            || !cm_decl_bytes_equal(entry->name, entry->name_length,
                item_name->bytes, item_name->len)) return 0;
        source = entry;
        direct_count += 1u;
    }
    if (direct_count != 1u || source == NULL
        || source->owner_module == 0u
        || source->owner_module > state->module_count) return 0;
    if (!cm_decl_trait_attributes(state, item, 1,
            &projected_count, &const_trait, &capture->diagnostic_item,
            &capture->diagnostic_item_length)) {
        if (!cm_decl_callable_trait_attributes(state, item,
                &projected_count, &capture->trait_flags,
                &capture->lang_item, &capture->lang_item_length)) return 0;
        callable_attributes = 1;
        const_trait = (capture->trait_flags
            & CM_HIR_DECL_TRAIT_IS_CONST) != 0u;
    }
    module = cm_decl_module_by_local(state, source->owner_module);
    if (module == NULL
        || item->owner_module != module->hir_id
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, source->export_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != CM_AST_ITEM_TRAIT
        || effective.child_kind != CM_EXPANDED_CHILD_TRAIT
        || effective.visibility != CM_AST_VIS_PUBLIC
        || !cm_decl_item_ref_equal(effective.declaration,
            source->declaration)
        || effective.attribute_count != item->attribute_count
        || effective.span.source != item->span.source
        || effective.span.start != item->span.start
        || effective.span.end != item->span.end
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, effective.declaration, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            effective.declaration.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_TRAIT
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->span.start != item->span.start
        || ast_item->span.end != item->span.end
        || ast_item->data.trait_item.is_alias
        || ast_item->data.trait_item.alias_bounds != CM_INTERN_ID_NONE
        || ast_item->data.trait_item.structured_alias_bounds != NULL
        || ast_item->data.trait_item.structured_alias_bound_count != 0u
        || item->definition.crate_id != state->input->crate_id
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->data.trait_item.safety
            != (ast_item->data.trait_item.is_unsafe
                ? CM_HIR_UNSAFE : CM_HIR_SAFE)
        || item->data.trait_item.is_auto
            != ast_item->data.trait_item.is_auto
        || item->data.trait_item.is_const != const_trait
        || (callable_attributes
            ? !cm_decl_callable_trait_header_source(state, ast, ast_item,
                item, capture)
            : !cm_decl_ast_generic_shape(state, ast, ast_item, item))
        || !cm_decl_ast_name_matches_hir(ast, ast_item->name,
            state->hir, item->name)
        || ((ast_item->data.trait_item.item_count == 0u)
            != (ast_item->data.trait_item.items == NULL))
        || (!callable_attributes
            && !cm_decl_trait_static_outlives_source(state, ast, ast_item,
                item, &has_static_outlives))) return 0;
    if (callable_attributes) has_static_outlives = 0;
    capture->has_static_outlives = has_static_outlives ? UINT8_C(1)
        : UINT8_C(0);
    for (attribute_index = 0u; attribute_index < effective.attribute_count;
            ++attribute_index) {
        CmResolveEffectiveAttribute graph_attribute;
        if (cm_module_graph_get_effective_item_attribute(
                state->input->graph, state->input->revision,
                module->graph.id, effective.id, attribute_index,
                &graph_attribute) != CM_RESOLVE_VIEW_OK
            || !cm_decl_effective_attribute_matches_hir(state,
                &graph_attribute, &item->attributes[attribute_index],
                effective.declaration)) return 0;
    }
    for (hir_index = 0u; hir_index < state->hir->items.len; ++hir_index) {
        const CmHirItem *child = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, hir_index);
        if (child != NULL && cm_hir_def_id_equal(child->parent_definition,
                item->definition)) hir_child_count += 1u;
    }
    if (hir_child_count != effective.child_count
        || effective.child_count > state->hir->items.len
        || state->associated_count > state->hir->items.len
            - effective.child_count
        || state->associated_count
            > CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS
        || (size_t)effective.child_count
            > CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS
                - state->associated_count) return 0;
    capture->owner_module = source->owner_module;
    capture->source_ordinal = source->export_ordinal;
    capture->associated_start = effective.child_count == 0u ? 0u
        : (uint32_t)(state->associated_count + 1u);
    capture->associated_count = effective.child_count;
    for (child_index = 0u; child_index < effective.child_count;
            ++child_index) {
        CmResolveEffectiveItem child_effective;
        const CmAstItem *child_ast;
        const CmHirItem *child_hir = NULL;
        CmHirItemId child_hir_id = CM_HIR_ITEM_NONE;
        uint32_t raw_index;
        size_t matches = 0u;
        size_t child_projected_count;
        if (cm_module_graph_get_effective_child(state->input->graph,
                state->input->revision, module->graph.id, effective.id,
                child_index, &child_effective) != CM_RESOLVE_VIEW_OK
            || child_effective.declaration.source
                != effective.declaration.source) return 0;
        for (raw_index = 0u;
                raw_index < ast_item->data.trait_item.item_count;
                ++raw_index) {
            if (ast_item->data.trait_item.items[raw_index]
                    == child_effective.declaration.item) break;
        }
        if (raw_index == ast_item->data.trait_item.item_count
            || (child_index != 0u && raw_index <= prior_raw_index)
            || (child_ast = cm_ast_get_item(ast,
                child_effective.declaration.item)) == NULL) return 0;
        prior_raw_index = raw_index;
        for (hir_index = 0u; hir_index < state->hir->items.len; ++hir_index) {
            const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
                &state->hir->items, hir_index);
            if (candidate != NULL
                && cm_hir_def_id_equal(candidate->parent_definition,
                    item->definition)
                && candidate->kind
                    == cm_decl_hir_kind_for_ast(child_ast->kind)
                && candidate->span.source == child_effective.span.source
                && candidate->span.start == child_effective.span.start
                && candidate->span.end == child_effective.span.end
                && cm_decl_ast_name_matches_hir(ast, child_ast->name,
                    state->hir, candidate->name)) {
                child_hir = candidate;
                child_hir_id = (CmHirItemId)(hir_index + 1u);
                matches += 1u;
            }
        }
        if (matches != 1u || child_hir == NULL) return 0;
        if (matches != 1u || child_hir == NULL
            || !cm_decl_trait_member_source_shape(state, capture,
                &child_effective, ast, child_ast, child_hir, child_hir_id,
                &child_projected_count)
            || projected_count > SIZE_MAX - child_projected_count
            || state->associated_count
                >= CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS) return 0;
        projected_count += child_projected_count;
        state->associated_items[state->associated_count].item = child_hir;
        state->associated_items[state->associated_count].id = child_hir_id;
        state->associated_items[state->associated_count].owner_module =
            source->owner_module;
        state->associated_items[state->associated_count].source_ordinal =
            raw_index;
        if (child_hir->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            size_t ignored_projected;
            if (!cm_decl_associated_type_attributes(state, child_hir,
                    &ignored_projected,
                    &state->associated_items[state->associated_count]
                        .lang_item,
                    &state->associated_items[state->associated_count]
                        .lang_item_length)
                || ignored_projected != child_projected_count) return 0;
        }
        state->associated_count += 1u;
    }
    *out_projected_count = projected_count;
    return 1;
}

static int cm_decl_ast_generic_shape(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstItem *ast_item, const CmHirItem *item)
{
    uint32_t index;
    if (ast_item->generic_parameter_count != item->generic_parameter_count
        || ((ast_item->generic_parameter_count == 0u)
            != (ast_item->generic_parameters == NULL))) return 0;
    if (item->predicate_count == 0u) {
        if (ast_item->where_clause != CM_INTERN_ID_NONE
            || ast_item->where_predicates != NULL
            || ast_item->where_predicate_count != 0u) return 0;
    } else if (item->predicate_count != 1u
        || ast_item->where_clause == CM_INTERN_ID_NONE
        || ast_item->where_predicates == NULL
        || ast_item->where_predicate_count != 1u) return 0;
    if (item->generic_parameter_count == 0u)
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) return 0;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmAstGenericParam *ast_generic =
            &ast_item->generic_parameters[index];
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start + index);
        if (generic == NULL
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->index != index || generic->has_default
            || generic->span.source != item->span.source
            || generic->span.start != item->span.start
            || generic->span.end != item->span.end
            || ast_generic->attributes != NULL
            || ast_generic->attribute_count != 0u
            || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
                state->hir, generic->name)
            || ast_generic->default_type != CM_AST_TYPE_NONE
            || ast_generic->default_const != CM_INTERN_ID_NONE
            || ast_generic->default_const_expr != CM_AST_EXPR_NONE) return 0;
        if (generic->kind == CM_HIR_GENERIC_CONST) {
            const CmAstType *ast_declared = cm_ast_get_type(ast,
                ast_generic->declared_type);
            const CmHirType *hir_declared = cm_hir_get_type(state->hir,
                generic->declared_type);
            if (ast_generic->kind != CM_AST_PARAM_CONST
                || generic->is_relaxed_sized
                || ast_generic->constraint == CM_INTERN_ID_NONE
                || ast_generic->bounds != NULL
                || ast_generic->bound_count != 0u
                || ast_declared == NULL || hir_declared == NULL
                || cm_decl_primitive(hir_declared)
                    != CM_HIR_DECL_PRIMITIVE_USIZE
                || !cm_decl_ast_type_matches_hir_primitive(ast,
                    ast_declared, hir_declared)) return 0;
        } else if (generic->kind == CM_HIR_GENERIC_TYPE) {
            if (ast_generic->kind != CM_AST_PARAM_TYPE
                || generic->declared_type != CM_HIR_TYPE_NONE
                || ast_generic->declared_type != CM_AST_TYPE_NONE) return 0;
            if (!generic->is_relaxed_sized) {
                if (ast_generic->constraint != CM_INTERN_ID_NONE
                    || ast_generic->bounds != NULL
                    || ast_generic->bound_count != 0u) return 0;
            } else {
                const CmAstGenericParamBound *bound;
                const CmAstType *bound_type;
                if (ast_generic->constraint == CM_INTERN_ID_NONE
                    || ast_generic->bounds == NULL
                    || ast_generic->bound_count != 1u) return 0;
                bound = &ast_generic->bounds[0];
                bound_type = cm_ast_get_type(ast, bound->trait_type);
                if (bound->kind != CM_AST_GENERIC_BOUND_TRAIT
                    || bound->modifier != CM_AST_GENERIC_BOUND_RELAXED
                    || bound->lifetime != CM_INTERN_ID_NONE
                    || bound->span.start > bound->span.end
                    || bound->trait_type == CM_AST_TYPE_NONE
                    || bound_type == NULL
                    || bound_type->kind != CM_AST_TYPE_PATH
                    || !cm_decl_ast_path_is(ast, bound_type->path, "Sized"))
                    return 0;
            }
        } else return 0;
    }
    if (item->predicate_count == 1u) {
        const CmHirTraitPredicate *hir_predicate = &item->predicates[0];
        const CmAstWherePredicate *ast_predicate =
            &ast_item->where_predicates[0];
        const CmHirType *subject = cm_hir_get_type(state->hir,
            hir_predicate->subject);
        CmHirItemId trait_item_id = CM_HIR_ITEM_NONE;
        const CmHirItem *trait_item = cm_decl_bound_item(state->hir,
            hir_predicate->trait_type.definition, &trait_item_id);
        const CmAstType *ast_subject = cm_ast_get_type(ast,
            ast_predicate->subject);
        const CmAstPath *subject_path = ast_subject == NULL
            || ast_subject->kind != CM_AST_TYPE_PATH ? NULL
            : cm_ast_get_path(ast, ast_subject->path);
        const CmAstType *ast_bound = ast_predicate->bounds == NULL ? NULL
            : cm_ast_get_type(ast,
                ast_predicate->bounds[0].trait_type);
        const CmAstPath *bound_path = ast_bound == NULL
            || ast_bound->kind != CM_AST_TYPE_PATH ? NULL
            : cm_ast_get_path(ast, ast_bound->path);
        const CmHirGenericParam *subject_generic = subject == NULL
            || subject->kind != CM_HIR_TYPE_PARAMETER_KIND ? NULL
            : cm_hir_get_generic_param(state->hir,
                subject->data.parameter_type.parameter);
        if (ast_predicate->kind != CM_AST_WHERE_PREDICATE_TYPE
            || ast_predicate->binder.lifetime_count != 0u
            || ast_predicate->binder.lifetimes != NULL
            || ast_predicate->bound_count != 1u
            || ast_predicate->bounds == NULL
            || ast_predicate->bounds[0].kind != CM_AST_WHERE_BOUND_TRAIT
            || ast_predicate->bounds[0].modifier
                != CM_AST_WHERE_BOUND_REQUIRED
            || ast_predicate->bounds[0].binder.lifetime_count != 0u
            || ast_predicate->bounds[0].binder.lifetimes != NULL
            || ast_predicate->bounds[0].lifetime != CM_INTERN_ID_NONE
            || subject_generic == NULL
            || subject_generic->kind != CM_HIR_GENERIC_TYPE
            || subject_generic->owner.crate_id != item->definition.crate_id
            || !cm_hir_def_id_equal(subject_generic->owner,
                item->definition)
            || hir_predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
            || hir_predicate->binder.lifetime_count != 0u
            || hir_predicate->binder.lifetimes != NULL
            || hir_predicate->modifier != CM_HIR_PREDICATE_REQUIRED
            || hir_predicate->trait_type.argument_count != 0u
            || hir_predicate->trait_type.arguments != NULL
            || hir_predicate->equality_count != 0u
            || hir_predicate->equalities != NULL
            || trait_item == NULL || trait_item->kind != CM_HIR_ITEM_TRAIT
            || trait_item_id == CM_HIR_ITEM_NONE
            || subject_path == NULL || subject_path->absolute
            || subject_path->segment_count != 1u
            || subject_path->segments == NULL
            || subject_path->segments[0].argument_count != 0u
            || subject_path->segments[0].arguments != NULL
            || !cm_decl_ast_name_matches_hir(ast,
                subject_path->segments[0].name, state->hir,
                subject_generic->name)
            || bound_path == NULL
            || !cm_decl_ast_path_resolves_item(state, ast, bound_path,
                item, trait_item)
            || hir_predicate->span.source != item->span.source
            || hir_predicate->span.start != ast_predicate->span.start
            || hir_predicate->span.end != ast_predicate->span.end) return 0;
    }
    return 1;
}

static int cm_decl_ast_ordinary_enum_generics(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstItem *ast_item, const CmHirItem *item)
{
    uint32_t index;
    if (item->generic_parameter_count == 0u
        || item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->generic_parameter_count != item->generic_parameter_count
        || ast_item->generic_parameters == NULL
        || !cm_decl_generics_shape(state, item)) return 0;
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmAstGenericParam *ast_generic =
            &ast_item->generic_parameters[index];
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start + index);
        if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(generic->owner, item->definition)
            || generic->index != index || generic->is_relaxed_sized
            || generic->declared_type != CM_HIR_TYPE_NONE
            || generic->has_default
            || generic->span.source != item->span.source
            || generic->span.start != item->span.start
            || generic->span.end != item->span.end
            || ast_generic->kind != CM_AST_PARAM_TYPE
            || ast_generic->attributes != NULL
            || ast_generic->attribute_count != 0u
            || !cm_decl_ast_name_matches_hir(ast, ast_generic->name,
                state->hir, generic->name)
            || ast_generic->constraint != CM_INTERN_ID_NONE
            || ast_generic->bounds != NULL || ast_generic->bound_count != 0u
            || ast_generic->declared_type != CM_AST_TYPE_NONE
            || ast_generic->default_type != CM_AST_TYPE_NONE
            || ast_generic->default_const != CM_INTERN_ID_NONE
            || ast_generic->default_const_expr != CM_AST_EXPR_NONE) return 0;
    }
    return 1;
}

static int cm_decl_field_visibility_matches(CmAstVisibility ast_visibility,
    CmHirVisibility hir_visibility)
{
    if (ast_visibility.kind == CM_AST_VIS_INHERITED)
        return ast_visibility.restriction == CM_AST_PATH_NONE
            && hir_visibility.kind == CM_HIR_VIS_PRIVATE
            && cm_hir_def_id_is_none(hir_visibility.restriction);
    if (ast_visibility.kind == CM_AST_VIS_PUBLIC)
        return ast_visibility.restriction == CM_AST_PATH_NONE
            && hir_visibility.kind == CM_HIR_VIS_PUBLIC
            && cm_hir_def_id_is_none(hir_visibility.restriction);
    if (ast_visibility.kind == CM_AST_VIS_CRATE)
        return ast_visibility.restriction == CM_AST_PATH_NONE
            && hir_visibility.kind == CM_HIR_VIS_CRATE
            && cm_hir_def_id_is_none(hir_visibility.restriction);
    return 0;
}

static int cm_decl_ast_path_resolves_item(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstPath *path, const CmHirItem *owner,
    const CmHirItem *target)
{
    CmAstItemKind target_kind;
    const CmDeclCaptureModule *owner_module = NULL;
    CmDeclCaptureModule *target_module = NULL;
    CmResolvePathSegmentView *segments;
    CmResolvedBinding binding;
    CmResolveEffectiveItem target_effective;
    const CmAst *target_ast = NULL;
    const CmAstItem *target_ast_item = NULL;
    uint32_t target_ordinal = 0u;
    size_t index;
    int valid;
    if (state == NULL || ast == NULL || path == NULL || owner == NULL
        || target == NULL || path->segment_count == 0u
        || path->segment_count > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
        || path->segments == NULL) return 0;
    if (target->kind == CM_HIR_ITEM_STRUCT) target_kind = CM_AST_ITEM_STRUCT;
    else if (target->kind == CM_HIR_ITEM_UNION)
        target_kind = CM_AST_ITEM_UNION;
    else if (target->kind == CM_HIR_ITEM_ENUM)
        target_kind = CM_AST_ITEM_ENUM;
    else if (target->kind == CM_HIR_ITEM_TRAIT)
        target_kind = CM_AST_ITEM_TRAIT;
    else if (target->kind == CM_HIR_ITEM_TYPE_ALIAS)
        target_kind = CM_AST_ITEM_TYPE_ALIAS;
    else return 0;
    for (index = 0u; index < state->module_count; ++index) {
        if (state->modules[index].hir_id == owner->owner_module) {
            if (owner_module != NULL) return 0;
            owner_module = &state->modules[index];
        }
    }
    if (owner_module == NULL
        || !cm_decl_item_source_view(state, target, target_kind,
            &target_module, &target_ordinal,
            &target_effective, &target_ast, &target_ast_item)
        || target_module->hir_id != target->owner_module) return 0;
    (void)target_ordinal;
    (void)target_ast;
    (void)target_ast_item;
    segments = (CmResolvePathSegmentView *)cm_alloc_zeroed(
        path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name = cm_ast_get_string(ast,
            path->segments[index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            return 0;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    valid = cm_import_resolve_path_checked(state->input->imports,
            state->input->graph, state->input->revision,
            owner_module->graph.id, path->absolute, segments,
            path->segment_count, CM_RESOLVE_NAMESPACE_TYPE, &binding)
            == CM_IMPORT_LOOKUP_OK
        && binding.revision == state->input->revision
        && binding.namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && binding.primitive_kind == CM_RESOLVE_PRIMITIVE_NONE
        && binding.variant.enumeration.source == 0u
        && binding.variant.enumeration.item == 0u
        && binding.target_module == CM_MODULE_NONE
        && binding.item_kind == target_kind
        && cm_decl_item_ref_equal(binding.declaration,
            target_effective.declaration);
    cm_free(segments);
    return valid;
}

static int cm_decl_pointer_storage_length(const CmInternedString *text,
    uint32_t pointer_bits, uint64_t *out_value)
{
    static const unsigned char suffix[] =
        "/size_of::<*const()>()";
    uint64_t numerator = 0u;
    size_t suffix_position = 0u;
    size_t position;
    int saw_digit = 0;
    if (text == NULL || out_value == NULL
        || (pointer_bits != 32u && pointer_bits != 64u)) return 0;
    for (position = 0u; position < text->len; ++position) {
        unsigned char byte = text->bytes[position];
        if (byte == (unsigned char)' ' || byte == (unsigned char)'\t'
            || byte == (unsigned char)'\r'
            || byte == (unsigned char)'\n') continue;
        if (suffix_position == 0u && byte >= (unsigned char)'0'
            && byte <= (unsigned char)'9') {
            unsigned int digit = (unsigned int)(byte - (unsigned char)'0');
            if (numerator > (UINT64_MAX - digit) / UINT64_C(10)) return 0;
            numerator = numerator * UINT64_C(10) + digit;
            saw_digit = 1;
            continue;
        }
        if (!saw_digit || suffix_position + 1u >= sizeof(suffix)
            || byte != suffix[suffix_position]) return 0;
        suffix_position += 1u;
    }
    if (!saw_digit || suffix_position + 1u != sizeof(suffix)) return 0;
    *out_value = numerator / ((uint64_t)pointer_bits / UINT64_C(8));
    return 1;
}

static int cm_decl_aggregate_array_length_matches(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner)
{
    const CmHirType *length_type;
    const CmInternedString *length_text;
    uint64_t length;
    if (state == NULL || ast == NULL || ast_type == NULL || hir_type == NULL
        || owner == NULL
        || ast_type->kind != CM_AST_TYPE_ARRAY
        || hir_type->kind != CM_HIR_TYPE_ARRAY_KIND
        || (length_text = cm_ast_get_string(ast, ast_type->text)) == NULL)
        return 0;
    if (hir_type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, hir_type->data.array_type.length.data.parameter);
        const CmInternedString *name = generic == NULL ? NULL
            : cm_interner_get(&state->hir->strings, generic->name);
        length_type = generic == NULL ? NULL
            : cm_hir_get_type(state->hir, generic->declared_type);
        return generic != NULL && generic->kind == CM_HIR_GENERIC_CONST
            && cm_hir_def_id_equal(generic->owner, owner->definition)
            && length_type != NULL
            && cm_decl_primitive(length_type)
                == CM_HIR_DECL_PRIMITIVE_USIZE
            && name != NULL && cm_decl_bytes_equal(length_text->bytes,
                length_text->len, name->bytes, name->len);
    }
    if ((length_type = cm_hir_get_type(state->hir,
            hir_type->data.array_type.length.type)) == NULL
        || cm_decl_primitive(length_type) != CM_HIR_DECL_PRIMITIVE_USIZE
        || hir_type->data.array_type.length.kind != CM_HIR_CONST_VALUE
        || hir_type->data.array_type.length.data.value.high_bits != 0u
        || (!cm_decl_parse_u64_decimal(length_text, &length)
            && !cm_decl_pointer_storage_length(length_text,
                state->input->target_pointer_bits, &length))) return 0;
    return length == hir_type->data.array_type.length.data.value.low_bits;
}

static int cm_decl_alias_instantiation_matches(
    const CmDeclCaptureState *state, const CmAst *use_ast,
    const CmAstPathSegment *use_segment, const CmHirItem *alias,
    const CmHirItem *owner, const CmHirType *alias_type,
    const CmHirType *final_type, size_t depth)
{
    uint32_t index;
    if (depth > 16u || alias_type == NULL || final_type == NULL) return 0;
    if (alias_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, alias_type->data.parameter_type.parameter);
        const CmAstGenericArg *argument;
        const CmAstType *ast_argument;
        if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
            || !cm_hir_def_id_equal(generic->owner, alias->definition)
            || generic->index >= use_segment->argument_count) return 0;
        argument = &use_segment->arguments[generic->index];
        ast_argument = argument->kind == CM_AST_GENERIC_TYPE
            ? cm_ast_get_type(use_ast, argument->type) : NULL;
        return ast_argument != NULL
            && cm_decl_ast_type_matches_hir_field(state, use_ast,
                ast_argument, final_type, owner, depth + 1u);
    }
    if (cm_decl_primitive(alias_type) != 0u)
        return cm_decl_primitive(alias_type) == cm_decl_primitive(final_type);
    if (alias_type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        const CmHirType *alias_child;
        const CmHirType *final_child;
        const CmHirGenericParam *generic;
        const CmHirGenericParam *final_generic;
        const CmAstGenericArg *argument;
        const CmAstType *argument_type;
        const CmAstPath *argument_path;
        const CmInternedString *final_name;
        if (final_type->kind != CM_HIR_TYPE_ARRAY_KIND
            || alias_type->data.array_type.length.kind
                != CM_HIR_CONST_PARAMETER
            || final_type->data.array_type.length.kind
                != CM_HIR_CONST_PARAMETER
            || (generic = cm_hir_get_generic_param(state->hir,
                alias_type->data.array_type.length.data.parameter)) == NULL
            || generic->kind != CM_HIR_GENERIC_CONST
            || !cm_hir_def_id_equal(generic->owner, alias->definition)
            || generic->index >= use_segment->argument_count
            || (final_generic = cm_hir_get_generic_param(state->hir,
                final_type->data.array_type.length.data.parameter)) == NULL
            || final_generic->kind != CM_HIR_GENERIC_CONST
            || !cm_hir_def_id_equal(final_generic->owner, owner->definition)
            || cm_decl_primitive(cm_hir_get_type(state->hir,
                generic->declared_type)) != CM_HIR_DECL_PRIMITIVE_USIZE
            || cm_decl_primitive(cm_hir_get_type(state->hir,
                final_generic->declared_type))
                != CM_HIR_DECL_PRIMITIVE_USIZE) return 0;
        argument = &use_segment->arguments[generic->index];
        argument_type = argument->kind == CM_AST_GENERIC_TYPE
            && argument->name == CM_INTERN_ID_NONE
            && argument->name_arguments == NULL
            && argument->name_argument_count == 0u
            && argument->bounds == NULL && argument->bound_count == 0u
            && argument->type != CM_AST_TYPE_NONE
            ? cm_ast_get_type(use_ast, argument->type) : NULL;
        argument_path = argument_type != NULL
            && argument_type->kind == CM_AST_TYPE_PATH
            ? cm_ast_get_path(use_ast, argument_type->path) : NULL;
        final_name = cm_interner_get(&state->hir->strings,
            final_generic->name);
        alias_child = cm_hir_get_type(state->hir,
            alias_type->data.array_type.element);
        final_child = cm_hir_get_type(state->hir,
            final_type->data.array_type.element);
        return argument_path != NULL && !argument_path->absolute
            && argument_path->segment_count == 1u
            && argument_path->segments != NULL
            && argument_path->segments[0].argument_count == 0u
            && argument_path->segments[0].arguments == NULL
            && final_name != NULL
            && cm_decl_ast_name_matches_hir(use_ast,
                argument_path->segments[0].name, state->hir,
                final_generic->name)
            && cm_decl_alias_instantiation_matches(state, use_ast,
                use_segment, alias, owner, alias_child, final_child,
                depth + 1u);
    }
    if (alias_type->kind == CM_HIR_TYPE_ADT_KIND) {
        if (final_type->kind != CM_HIR_TYPE_ADT_KIND
            || !cm_hir_def_id_equal(alias_type->data.named_type.definition,
                final_type->data.named_type.definition)
            || alias_type->data.named_type.argument_count
                != final_type->data.named_type.argument_count
            || (alias_type->data.named_type.argument_count == 0u)
                != (alias_type->data.named_type.arguments == NULL)
            || (final_type->data.named_type.argument_count == 0u)
                != (final_type->data.named_type.arguments == NULL)) return 0;
        for (index = 0u;
                index < alias_type->data.named_type.argument_count; ++index) {
            const CmHirGenericArg *left =
                &alias_type->data.named_type.arguments[index];
            const CmHirGenericArg *right =
                &final_type->data.named_type.arguments[index];
            if (left->kind != CM_HIR_GENERIC_ARG_TYPE
                || right->kind != CM_HIR_GENERIC_ARG_TYPE
                || !cm_decl_alias_instantiation_matches(state, use_ast,
                    use_segment, alias, owner,
                    cm_hir_get_type(state->hir, left->data.type),
                    cm_hir_get_type(state->hir, right->data.type),
                    depth + 1u)) return 0;
        }
        return 1;
    }
    return 0;
}

static int cm_decl_resolve_type_path(const CmDeclCaptureState *state,
    const CmAst *ast, const CmAstPath *path, const CmHirItem *owner,
    CmResolvedBinding *out_binding)
{
    const CmDeclCaptureModule *owner_module = NULL;
    CmResolvePathSegmentView *segments;
    size_t index;
    int valid;
    if (state == NULL || ast == NULL || path == NULL || owner == NULL
        || out_binding == NULL || path->segment_count == 0u
        || path->segment_count > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
        || path->segments == NULL) return 0;
    for (index = 0u; index < state->module_count; ++index)
        if (state->modules[index].hir_id == owner->owner_module) {
            if (owner_module != NULL) return 0;
            owner_module = &state->modules[index];
        }
    if (owner_module == NULL) return 0;
    segments = (CmResolvePathSegmentView *)cm_alloc_zeroed(
        path->segment_count, sizeof(*segments));
    for (index = 0u; index < path->segment_count; ++index) {
        const CmInternedString *name = cm_ast_get_string(ast,
            path->segments[index].name);
        if (name == NULL || name->len == 0u) {
            cm_free(segments);
            return 0;
        }
        segments[index].bytes = name->bytes;
        segments[index].length = name->len;
    }
    valid = cm_import_resolve_path_checked(state->input->imports,
            state->input->graph, state->input->revision,
            owner_module->graph.id, path->absolute, segments,
            path->segment_count, CM_RESOLVE_NAMESPACE_TYPE, out_binding)
            == CM_IMPORT_LOOKUP_OK
        && out_binding->revision == state->input->revision
        && out_binding->namespace_kind == CM_RESOLVE_NAMESPACE_TYPE
        && out_binding->primitive_kind == CM_RESOLVE_PRIMITIVE_NONE
        && out_binding->variant.enumeration.source == 0u
        && out_binding->variant.enumeration.item == 0u
        && out_binding->target_module == CM_MODULE_NONE;
    cm_free(segments);
    return valid;
}

static int cm_decl_ast_alias_matches_hir_field(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner, size_t depth)
{
    const CmAstPath *path = ast_type->kind == CM_AST_TYPE_PATH
        ? cm_ast_get_path(ast, ast_type->path) : NULL;
    const CmAstPathSegment *segment;
    CmResolvedBinding binding;
    size_t item_index;
    size_t matches = 0u;
    if (path == NULL || path->segment_count == 0u || path->segments == NULL)
        return 0;
    if (!cm_decl_resolve_type_path(state, ast, path, owner, &binding)
        || binding.item_kind != CM_AST_ITEM_TYPE_ALIAS) return 0;
    segment = &path->segments[path->segment_count - 1u];
    for (item_index = 0u; item_index < state->hir->items.len; ++item_index) {
        const CmHirItem *alias = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, item_index);
        CmDeclCaptureModule *module = NULL;
        CmResolveEffectiveItem effective;
        const CmAst *alias_ast = NULL;
        const CmAstItem *ast_alias = NULL;
        const CmAstType *ast_target;
        const CmHirType *alias_target;
        uint32_t ordinal = 0u;
        if (alias == NULL || alias->kind != CM_HIR_ITEM_TYPE_ALIAS
            || alias->definition.crate_id != state->input->crate_id
            || alias->attribute_count != 0u || alias->attributes != NULL
            || alias->predicate_scope_count != 0u
            || alias->predicate_scopes != NULL
            || alias->predicate_count != 0u || alias->predicates != NULL
            || alias->outlives_predicate_count != 0u
            || alias->outlives_predicates != NULL
            || alias->generic_parameter_count != segment->argument_count
            || segment->argument_count == 0u || segment->arguments == NULL
            || !cm_decl_item_source_view(state, alias,
                CM_AST_ITEM_TYPE_ALIAS, &module, &ordinal, &effective,
                &alias_ast, &ast_alias)
            || !cm_decl_item_ref_equal(binding.declaration,
                effective.declaration)
            || alias_ast == NULL || ast_alias == NULL
            || !cm_decl_ast_generic_shape(state, alias_ast, ast_alias, alias)
            || ast_alias->data.value_item.type == CM_AST_TYPE_NONE
            || (ast_target = cm_ast_get_type(alias_ast,
                ast_alias->data.value_item.type)) == NULL
            || (alias_target = cm_hir_get_type(state->hir,
                alias->data.type_alias_item.target)) == NULL
            || !cm_decl_ast_type_matches_hir_field(state, alias_ast,
                ast_target, alias_target, alias, depth + 1u)
            || !cm_decl_alias_instantiation_matches(state, ast, segment,
                alias, owner, alias_target, hir_type, depth + 1u)) continue;
        matches += 1u;
    }
    return matches == 1u;
}

static int cm_decl_ast_type_matches_hir_field(
    const CmDeclCaptureState *state, const CmAst *ast,
    const CmAstType *ast_type, const CmHirType *hir_type,
    const CmHirItem *owner, size_t depth)
{
    const CmAstPath *path;
    const CmAstPathSegment *segment;
    const CmHirItem *target;
    CmHirItemId target_id;
    uint32_t index;
    if (depth > 16u || ast_type == NULL || hir_type == NULL
        || hir_type->span.source != owner->span.source
        || hir_type->span.start != ast_type->span.start
        || hir_type->span.end != ast_type->span.end) return 0;
    if (cm_decl_primitive(hir_type) != 0u)
        return cm_decl_ast_type_matches_hir_primitive(ast, ast_type, hir_type);
    if (hir_type->kind == CM_HIR_TYPE_SELF_KIND) {
        CmHirDefId expected_owner = cm_hir_def_id_is_none(
                owner->parent_definition)
            ? owner->definition : owner->parent_definition;
        return cm_hir_def_id_equal(hir_type->data.self_type.owner,
                expected_owner)
            && ast_type->kind == CM_AST_TYPE_PATH
            && cm_decl_ast_path_is(ast, ast_type->path, "Self");
    }
    if (hir_type->kind == CM_HIR_TYPE_PROJECTION_KIND) {
        const CmHirType *self_type = cm_hir_get_type(state->hir,
            hir_type->data.projection_type.self_type);
        const CmHirItem *associated;
        const CmHirItem *trait_item;
        const CmHirType *argument;
        const CmInternedString *ast_self_name;
        CmHirItemId ignored_id;
        path = ast_type->kind == CM_AST_TYPE_PATH
            ? cm_ast_get_path(ast, ast_type->path) : NULL;
        associated = cm_decl_bound_item(state->hir,
            hir_type->data.projection_type.associated_type.definition,
            &ignored_id);
        trait_item = cm_decl_bound_item(state->hir,
            hir_type->data.projection_type.trait_type.definition,
            &ignored_id);
        argument = hir_type->data.projection_type.trait_type.argument_count
                    == 1u
                && hir_type->data.projection_type.trait_type.arguments != NULL
                && hir_type->data.projection_type.trait_type.arguments[0].kind
                    == CM_HIR_GENERIC_ARG_TYPE
            ? cm_hir_get_type(state->hir,
                hir_type->data.projection_type.trait_type.arguments[0]
                    .data.type)
            : NULL;
        ast_self_name = path == NULL || path->segment_count == 0u
                || path->segments == NULL
            ? NULL : cm_ast_get_string(ast, path->segments[0].name);
        return path != NULL && !path->absolute
            && path->segment_count == 2u && path->segments != NULL
            && path->segments[0].argument_count == 0u
            && path->segments[0].arguments == NULL
            && path->segments[1].argument_count == 0u
            && path->segments[1].arguments == NULL
            && ast_self_name != NULL
            && cm_decl_bytes_equal(ast_self_name->bytes, ast_self_name->len,
                (const unsigned char *)"Self", 4u)
            && self_type != NULL && self_type->kind == CM_HIR_TYPE_SELF_KIND
            && !cm_hir_def_id_is_none(owner->parent_definition)
            && cm_hir_def_id_equal(self_type->data.self_type.owner,
                owner->parent_definition)
            && trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
            && associated != NULL
            && associated->kind == CM_HIR_ITEM_TYPE_ALIAS
            && cm_hir_def_id_equal(associated->parent_definition,
                trait_item->definition)
            && cm_decl_ast_name_matches_hir(ast, path->segments[1].name,
                state->hir, associated->name)
            && hir_type->data.projection_type.associated_type.argument_count
                == 0u
            && hir_type->data.projection_type.associated_type.arguments
                == NULL
            && trait_item->generic_parameter_count == 1u
            && argument != NULL
            && argument->kind == CM_HIR_TYPE_PARAMETER_KIND
            && cm_hir_get_generic_param(state->hir,
                argument->data.parameter_type.parameter) != NULL
            && cm_hir_def_id_equal(cm_hir_get_generic_param(state->hir,
                    argument->data.parameter_type.parameter)->owner,
                owner->parent_definition);
    }
    if (hir_type->kind == CM_HIR_TYPE_REFERENCE_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        const CmInternedString *lifetime = ast_type->lifetime
                == CM_INTERN_ID_NONE
            ? NULL : cm_ast_get_string(ast, ast_type->lifetime);
        if (ast_type->kind != CM_AST_TYPE_REFERENCE
            || ast_type->child == CM_AST_TYPE_NONE
            || ast_type->is_mutable
                != (hir_type->data.reference_type.mutability
                    == CM_HIR_MUTABLE)
            || !((hir_type->data.reference_type.region.kind
                        == CM_HIR_REGION_ERASED
                    && ast_type->lifetime == CM_INTERN_ID_NONE)
                || (hir_type->data.reference_type.region.kind
                        == CM_HIR_REGION_STATIC
                    && lifetime != NULL
                    && cm_decl_bytes_equal(lifetime->bytes, lifetime->len,
                        (const unsigned char *)"'static", 7u)))
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.reference_type.pointee)) == NULL) return 0;
        return cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
            hir_child, owner, depth + 1u);
    }
    if (hir_type->kind == CM_HIR_TYPE_SLICE_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        if (ast_type->kind != CM_AST_TYPE_SLICE
            || ast_type->child == CM_AST_TYPE_NONE
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.slice_type.element)) == NULL) return 0;
        return cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
            hir_child, owner, depth + 1u);
    }
    if (hir_type->kind == CM_HIR_TYPE_RAW_POINTER_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        if (ast_type->kind != CM_AST_TYPE_POINTER
            || ast_type->child == CM_AST_TYPE_NONE
            || (hir_type->data.raw_pointer_type.mutability
                    != CM_HIR_IMMUTABLE
                && hir_type->data.raw_pointer_type.mutability
                    != CM_HIR_MUTABLE)
            || ast_type->is_mutable
                != (hir_type->data.raw_pointer_type.mutability
                    == CM_HIR_MUTABLE)
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.raw_pointer_type.pointee)) == NULL) return 0;
        return cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
            hir_child, owner, depth + 1u);
    }
    if (hir_type->kind == CM_HIR_TYPE_ARRAY_KIND) {
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        if (ast_type->kind != CM_AST_TYPE_ARRAY
            || ast_type->child == CM_AST_TYPE_NONE
            || hir_type->data.array_type.element == CM_HIR_TYPE_NONE
            || (ast_child = cm_ast_get_type(ast, ast_type->child)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_type->data.array_type.element)) == NULL
            || !cm_decl_aggregate_array_length_matches(state, ast, ast_type,
                hir_type, owner)) return 0;
        return cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
            hir_child, owner, depth + 1u);
    }
    if (hir_type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, hir_type->data.parameter_type.parameter);
        path = ast_type->kind == CM_AST_TYPE_PATH
            ? cm_ast_get_path(ast, ast_type->path) : NULL;
        return generic != NULL
            && (cm_hir_def_id_equal(generic->owner, owner->definition)
                || (!cm_hir_def_id_is_none(owner->parent_definition)
                    && cm_hir_def_id_equal(generic->owner,
                        owner->parent_definition)))
            && path != NULL && !path->absolute && path->segment_count == 1u
            && path->segments != NULL
            && path->segments[0].argument_count == 0u
            && path->segments[0].arguments == NULL
            && cm_decl_ast_name_matches_hir(ast, path->segments[0].name,
                state->hir, generic->name);
    }
    if (hir_type->kind == CM_HIR_TYPE_ADT_KIND
        && ast_type->kind == CM_AST_TYPE_PATH
        && cm_decl_ast_alias_matches_hir_field(state, ast, ast_type,
            hir_type, owner, depth)) return 1;
    if (hir_type->kind != CM_HIR_TYPE_ADT_KIND
        || ast_type->kind != CM_AST_TYPE_PATH
        || (hir_type->data.named_type.argument_count == 0u)
            != (hir_type->data.named_type.arguments == NULL)
        || (target = cm_decl_bound_item(state->hir,
            hir_type->data.named_type.definition, &target_id)) == NULL
        || target->definition.crate_id != state->input->crate_id
        || (target->kind != CM_HIR_ITEM_STRUCT
            && target->kind != CM_HIR_ITEM_UNION
            && target->kind != CM_HIR_ITEM_ENUM)
        || target->generic_parameter_count
            != hir_type->data.named_type.argument_count
        || hir_type->data.named_type.argument_count
            > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) return 0;
    path = cm_ast_get_path(ast, ast_type->path);
    if (path == NULL || path->segment_count == 0u || path->segments == NULL)
        return 0;
    for (index = 0u; index + 1u < path->segment_count; ++index) {
        if (path->segments[index].argument_count != 0u
            || path->segments[index].arguments != NULL) return 0;
    }
    segment = &path->segments[path->segment_count - 1u];
    if (!cm_decl_ast_name_matches_hir(ast, segment->name, state->hir,
            target->name)
        || segment->argument_count != hir_type->data.named_type.argument_count
        || ((segment->argument_count == 0u)
            != (segment->arguments == NULL))
        || !cm_decl_ast_path_resolves_item(state, ast, path, owner, target))
        return 0;
    for (index = 0u; index < segment->argument_count; ++index) {
        const CmAstGenericArg *ast_argument = &segment->arguments[index];
        const CmHirGenericArg *hir_argument =
            &hir_type->data.named_type.arguments[index];
        const CmAstType *ast_child;
        const CmHirType *hir_child;
        if (ast_argument->kind != CM_AST_GENERIC_TYPE
            || ast_argument->name != CM_INTERN_ID_NONE
            || ast_argument->name_arguments != NULL
            || ast_argument->name_argument_count != 0u
            || ast_argument->type == CM_AST_TYPE_NONE
            || ast_argument->bounds != NULL || ast_argument->bound_count != 0u
            || hir_argument->kind != CM_HIR_GENERIC_ARG_TYPE
            || (ast_child = cm_ast_get_type(ast, ast_argument->type)) == NULL
            || (hir_child = cm_hir_get_type(state->hir,
                hir_argument->data.type)) == NULL
            || !cm_decl_ast_type_matches_hir_field(state, ast, ast_child,
                hir_child, owner, depth + 1u)) return 0;
    }
    return 1;
}

static int cm_decl_aggregate_shape_and_source(CmDeclCaptureState *state,
    CmDeclCaptureItem *capture, size_t *out_projected_count)
{
    const CmHirItem *item = capture->item;
    CmDeclCaptureModule *module = NULL;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    CmResolveEffectiveItem effective;
    CmAstItemKind ast_kind = item->kind == CM_HIR_ITEM_UNION
        ? CM_AST_ITEM_UNION : CM_AST_ITEM_STRUCT;
    uint32_t index;
    uint32_t source_ordinal = 0u;
    size_t projected_count;
    if ((item->kind != CM_HIR_ITEM_STRUCT && item->kind != CM_HIR_ITEM_UNION)
        || (item->visibility.kind != CM_HIR_VIS_PUBLIC
            && item->visibility.kind != CM_HIR_VIS_PRIVATE
            && item->visibility.kind != CM_HIR_VIS_CRATE
            && item->visibility.kind != CM_HIR_VIS_RESTRICTED)
        || cm_hir_def_id_is_none(item->definition)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->predicate_scopes != NULL || item->predicate_scope_count != 0u
        || ((item->predicate_count == 0u) != (item->predicates == NULL))
        || item->predicate_count > 1u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || (item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
            && item->data.aggregate_item.form != CM_HIR_AGGREGATE_TUPLE)
        || (item->kind == CM_HIR_ITEM_UNION
            && item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED)
        || item->data.aggregate_item.field_count == 0u
        || (size_t)item->data.aggregate_item.field_count
            > CM_HIR_DECL_METADATA_MAX_FIELDS
        || item->data.aggregate_item.fields == NULL
        || !cm_decl_generics_shape(state, item)
        || !cm_decl_item_source_view(state, item, ast_kind, &module,
            &source_ordinal, &effective, &ast, &ast_item)
        || !cm_decl_aggregate_attributes(state, item,
            &capture->aggregate_repr, &capture->aggregate_flags,
            &capture->lang_item, &capture->lang_item_length,
            &capture->diagnostic_item, &capture->diagnostic_item_length,
            &projected_count)
        || !cm_decl_item_attribute_provenance(state, item, ast_kind,
            CM_HIR_LIBRARY_BINDING_TYPE)) return 0;
    capture->owner_module = module->local;
    capture->source_ordinal = source_ordinal;
    if (item->visibility.kind == CM_HIR_VIS_PUBLIC) {
        uint32_t namespace_module = 0u;
        uint32_t namespace_ordinal = 0u;
        if (!cm_decl_named_aggregate_source(state, item, ast_kind,
                &namespace_module, &namespace_ordinal)
            || namespace_module != capture->owner_module
            || namespace_ordinal != capture->source_ordinal) return 0;
    } else {
        for (index = 0u; index < state->namespace_count; ++index)
            if (cm_hir_def_id_equal(state->namespace_values[index]
                    .target.definition, item->definition)) return 0;
    }
    *out_projected_count = projected_count;
    if (ast_item->is_default
        || ast_item->data.aggregate_item.form
            != (item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
                ? CM_AST_FIELDS_NAMED : CM_AST_FIELDS_TUPLE)
        || ast_item->data.aggregate_item.field_count
            != item->data.aggregate_item.field_count
        || ast_item->data.aggregate_item.fields == NULL
        || !cm_decl_ast_generic_shape(state, ast, ast_item, item)) return 0;
    if (capture->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
        && item->kind == CM_HIR_ITEM_STRUCT
        && item->data.aggregate_item.field_count != 1u) return 0;
    if ((capture->aggregate_flags
            & CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR) != 0u
        && (item->kind != CM_HIR_ITEM_STRUCT
            || item->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
            || capture->aggregate_repr != CM_HIR_DECL_AGGREGATE_REPR_RUST
            || (capture->aggregate_flags
                & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) == 0u))
        return 0;
    if (capture->aggregate_repr == CM_HIR_DECL_AGGREGATE_REPR_RUST) {
        if (item->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE) {
            if (item->kind != CM_HIR_ITEM_STRUCT
                || item->generic_parameter_count != 0u
                || capture->aggregate_flags != 0u) return 0;
        } else if (item->data.aggregate_item.form
                != CM_HIR_AGGREGATE_NAMED) return 0;
    } else if (item->generic_parameter_count == 0u) {
        if (item->kind != CM_HIR_ITEM_STRUCT) return 0;
    } else if (item->kind == CM_HIR_ITEM_STRUCT) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start);
        const CmHirType *field_type = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[0].type);
        if (item->generic_parameter_count != 1u || generic == NULL
            || !generic->is_relaxed_sized
            || item->data.aggregate_item.field_count != 1u
            || item->data.aggregate_item.fields[0].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[0]
                .visibility.restriction)
            || field_type == NULL
            || field_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            || field_type->data.parameter_type.parameter
                != item->generic_parameter_start) return 0;
    } else {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, item->generic_parameter_start);
        const CmHirType *unit_type = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[0].type);
        const CmHirType *application = cm_hir_get_type(state->hir,
            item->data.aggregate_item.fields[1].type);
        const CmHirItem *target;
        CmHirItemId target_id;
        uint8_t target_repr = 0u;
        uint16_t target_flags = 0u;
        const unsigned char *target_lang = NULL;
        size_t target_lang_length = 0u;
        const unsigned char *target_diagnostic = NULL;
        size_t target_diagnostic_length = 0u;
        size_t ignored_projection = 0u;
        if (item->generic_parameter_count != 1u || generic == NULL
            || generic->is_relaxed_sized
            || item->data.aggregate_item.field_count != 2u
            || item->data.aggregate_item.fields[0].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || item->data.aggregate_item.fields[1].visibility.kind
                != CM_HIR_VIS_PRIVATE
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[0]
                .visibility.restriction)
            || !cm_hir_def_id_is_none(item->data.aggregate_item.fields[1]
                .visibility.restriction)
            || cm_decl_primitive(unit_type) != CM_HIR_DECL_PRIMITIVE_UNIT
            || application == NULL || application->kind != CM_HIR_TYPE_ADT_KIND
            || application->data.named_type.argument_count != 1u
            || application->data.named_type.arguments == NULL
            || application->data.named_type.arguments[0].kind
                != CM_HIR_GENERIC_ARG_TYPE
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type) == NULL
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type)->kind
                != CM_HIR_TYPE_PARAMETER_KIND
            || cm_hir_get_type(state->hir,
                application->data.named_type.arguments[0].data.type)->data
                    .parameter_type.parameter != item->generic_parameter_start
            || (target = cm_decl_bound_item(state->hir,
                application->data.named_type.definition, &target_id)) == NULL
            || target->kind != CM_HIR_ITEM_STRUCT
            || target->data.aggregate_item.form != CM_HIR_AGGREGATE_NAMED
            || target->generic_parameter_count != 1u
            || !cm_decl_aggregate_attributes(state, target, &target_repr,
                &target_flags, &target_lang, &target_lang_length,
                &target_diagnostic, &target_diagnostic_length,
                &ignored_projection)
            || target_repr != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
            || (target_flags & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) == 0u
            || target_diagnostic != NULL || target_diagnostic_length != 0u
            || !cm_decl_bytes_equal(target_lang, target_lang_length,
                (const unsigned char *)"manually_drop", 13u)) return 0;
    }
    for (index = 0u; index < item->data.aggregate_item.field_count; ++index) {
        const CmHirField *field = &item->data.aggregate_item.fields[index];
        const CmAstField *ast_field =
            &ast_item->data.aggregate_item.fields[index];
        const CmHirType *hir_type = cm_hir_get_type(state->hir, field->type);
        const CmAstType *ast_type = cm_ast_get_type(ast, ast_field->type);
        if ((item->data.aggregate_item.form == CM_HIR_AGGREGATE_NAMED
                ? !cm_decl_ast_name_matches_hir(ast, ast_field->name,
                    state->hir, field->name)
                : ast_field->name != CM_INTERN_ID_NONE
                    || field->name != CM_INTERN_ID_NONE)
            || !cm_decl_field_visibility_matches(ast_field->visibility,
                field->visibility)
            || field->span.source != item->span.source
            || field->span.start != item->span.start
            || field->span.end != item->span.end
            || !cm_decl_ast_type_matches_hir_field(state, ast, ast_type,
                hir_type, item, 0u)) return 0;
        if (item->data.aggregate_item.form == CM_HIR_AGGREGATE_TUPLE
            && (field->visibility.kind != CM_HIR_VIS_PRIVATE
                || !cm_hir_def_id_is_none(field->visibility.restriction)))
            return 0;
    }
    return 1;
}

static int cm_decl_capture_dependency_type(CmDeclCaptureState *state,
    CmHirTypeId type_id, size_t depth,
    CmHirDeclarationCaptureResult *result);

static int cm_decl_capture_private_trait(CmDeclCaptureState *state,
    const CmHirItem *item, CmHirItemId item_id, CmDeclCaptureItem *capture,
    size_t *out_projected_count)
{
    CmDeclCaptureModule *module = NULL;
    CmResolveEffectiveItem effective;
    CmResolveEffectiveItem child_effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item = NULL;
    const CmAstItem *child_ast;
    const CmHirItem *child_hir = NULL;
    CmHirItemId child_hir_id = CM_HIR_ITEM_NONE;
    uint32_t source_ordinal = 0u;
    uint32_t raw_index;
    size_t hir_index;
    size_t matches = 0u;
    size_t parent_projected = 0u;
    size_t child_projected = 0u;
    int ignored_non_exhaustive = 0;
    if (item == NULL || capture == NULL || out_projected_count == NULL
        || item->kind != CM_HIR_ITEM_TRAIT
        || item->visibility.kind != CM_HIR_VIS_PRIVATE
        || !cm_hir_def_id_is_none(item->visibility.restriction)
        || !cm_decl_project_item_attributes(state, item,
            CM_DECL_ATTR_ALLOW, &parent_projected,
            &ignored_non_exhaustive)
        || ignored_non_exhaustive || parent_projected != 1u
        || !cm_decl_item_attribute_provenance(state, item,
            CM_AST_ITEM_TRAIT, CM_HIR_LIBRARY_BINDING_TRAIT)
        || item->generic_parameter_count != 0u
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->predicate_scope_count != 0u || item->predicate_scopes != NULL
        || item->predicate_count != 0u || item->predicates != NULL
        || item->outlives_predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->data.trait_item.safety != CM_HIR_SAFE
        || item->data.trait_item.is_auto || item->data.trait_item.is_const
        || item->data.trait_item.supertrait_count != 0u
        || item->data.trait_item.supertraits != NULL
        || !cm_decl_item_source_view(state, item, CM_AST_ITEM_TRAIT,
            &module, &source_ordinal, &effective, &ast, &ast_item)
        || module == NULL || ast == NULL || ast_item == NULL
        || effective.child_kind != CM_EXPANDED_CHILD_TRAIT
        || effective.child_count != 1u
        || ast_item->data.trait_item.item_count != 1u
        || ast_item->data.trait_item.items == NULL
        || ast_item->data.trait_item.is_alias
        || ast_item->data.trait_item.is_unsafe
        || ast_item->data.trait_item.is_auto
        || ast_item->data.trait_item.supertraits != CM_INTERN_ID_NONE
        || ast_item->data.trait_item.structured_supertrait_count != 0u
        || ast_item->data.trait_item.structured_supertraits != NULL
        || cm_module_graph_get_effective_child(state->input->graph,
            state->input->revision, module->graph.id, effective.id, 0u,
            &child_effective) != CM_RESOLVE_VIEW_OK
        || child_effective.declaration.source != effective.declaration.source)
        return 0;
    for (raw_index = 0u; raw_index < ast_item->data.trait_item.item_count;
            ++raw_index)
        if (ast_item->data.trait_item.items[raw_index]
                == child_effective.declaration.item) break;
    if (raw_index != 0u
        || (child_ast = cm_ast_get_item(ast,
            child_effective.declaration.item)) == NULL) return 0;
    for (hir_index = 0u; hir_index < state->hir->items.len; ++hir_index) {
        const CmHirItem *candidate = (const CmHirItem *)cm_vec_at_const(
            &state->hir->items, hir_index);
        if (candidate != NULL
            && cm_hir_def_id_equal(candidate->parent_definition,
                item->definition)
            && candidate->kind == CM_HIR_ITEM_FUNCTION
            && candidate->span.source == child_effective.span.source
            && candidate->span.start == child_effective.span.start
            && candidate->span.end == child_effective.span.end
            && cm_decl_ast_name_matches_hir(ast, child_ast->name,
                state->hir, candidate->name)) {
            child_hir = candidate;
            child_hir_id = (CmHirItemId)(hir_index + 1u);
            matches += 1u;
        }
    }
    capture->item = item;
    capture->id = item_id;
    capture->owner_module = module->local;
    capture->source_ordinal = source_ordinal;
    capture->associated_start = (uint32_t)(state->associated_count + 1u);
    capture->associated_count = 1u;
    if (matches != 1u || child_hir == NULL
        || state->associated_count
            >= CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS
        || !cm_decl_trait_member_source_shape(state, capture,
            &child_effective, ast, child_ast, child_hir, child_hir_id,
            &child_projected)) return 0;
    state->associated_items[state->associated_count].item = child_hir;
    state->associated_items[state->associated_count].id = child_hir_id;
    state->associated_items[state->associated_count].owner_module =
        module->local;
    state->associated_items[state->associated_count].source_ordinal = 0u;
    state->associated_count += 1u;
    if (parent_projected > SIZE_MAX - child_projected) return 0;
    *out_projected_count = parent_projected + child_projected;
    return 1;
}

static int cm_decl_capture_private_dependency(CmDeclCaptureState *state,
    CmHirDefId definition, CmHirDeclarationCaptureResult *result)
{
    CmDeclCaptureItem capture;
    const CmHirItem *item;
    CmHirItemId item_id = CM_HIR_ITEM_NONE;
    size_t projected_count = 0u;
    size_t namespace_index;
    (void)result;
    memset(&capture, 0, sizeof(capture));
    if (cm_decl_item_already(state->traits, state->trait_count, definition))
        return 1;
    if (cm_decl_item_already(state->items, state->item_count, definition))
        return 1;
    if (cm_decl_item_already(state->associated_items,
            state->associated_count, definition)) return 1;
    item = cm_decl_bound_item(state->hir, definition, &item_id);
    if (item == NULL || item->definition.crate_id != state->input->crate_id
        || item->visibility.kind == CM_HIR_VIS_PUBLIC) return 0;
    if (item->kind == CM_HIR_ITEM_TRAIT) {
        if (cm_decl_item_already(state->traits, state->trait_count,
                definition)) return 1;
        if (state->trait_count >= state->namespace_count
            || !cm_decl_capture_private_trait(state, item, item_id, &capture,
                &projected_count)) return 0;
        if (projected_count > SIZE_MAX
                - state->projected_semantic_attribute_count) return 0;
        state->projected_semantic_attribute_count += projected_count;
        state->traits[state->trait_count++] = capture;
        return 1;
    }
    if (item->kind == CM_HIR_ITEM_TRAIT
        || state->item_count >= state->item_capacity
        || state->item_count >= CM_HIR_DECL_METADATA_MAX_ITEMS) return 0;
    for (namespace_index = 0u; namespace_index < state->namespace_count;
            ++namespace_index)
        if (cm_hir_def_id_equal(state->namespace_values[namespace_index]
                .target.definition, definition)) return 0;
    capture.item = item;
    capture.id = item_id;
    if (item->kind == CM_HIR_ITEM_STRUCT
            || item->kind == CM_HIR_ITEM_UNION) {
        if (!cm_decl_aggregate_shape_and_source(state, &capture,
                &projected_count)) return 0;
    } else if (item->kind == CM_HIR_ITEM_ENUM) {
        CmDeclCaptureModule *module = NULL;
        CmResolveEffectiveItem effective;
        const CmAst *ast = NULL;
        const CmAstItem *ast_item = NULL;
        int profile;
        if (!cm_decl_item_source_view(state, item, CM_AST_ITEM_ENUM, &module,
                &capture.source_ordinal, &effective, &ast, &ast_item)
            || module == NULL) return 0;
        capture.owner_module = module->local;
        if (!cm_decl_enum_item_attributes(state, item, &projected_count,
                &profile, &capture.lang_item, &capture.lang_item_length,
                &capture.enum_repr)
            || !cm_decl_enum_shape_and_variants(state, item, item_id,
                capture.owner_module, capture.source_ordinal, profile,
                capture.enum_repr, &projected_count)) return 0;
        (void)effective;
        (void)ast;
        (void)ast_item;
    } else return 0;
    if (projected_count > SIZE_MAX
            - state->projected_semantic_attribute_count) return 0;
    state->projected_semantic_attribute_count += projected_count;
    state->items[state->item_count++] = capture;
    return 1;
}

static int cm_decl_capture_dependency_type(CmDeclCaptureState *state,
    CmHirTypeId type_id, size_t depth,
    CmHirDeclarationCaptureResult *result)
{
    const CmHirType *type;
    uint32_t index;
    if (depth > CM_META_MAX_TYPE_NESTING || type_id == CM_HIR_TYPE_NONE
        || (size_t)type_id > state->hir->types.len
        || (type = cm_hir_get_type(state->hir, type_id)) == NULL) return 0;
    if (cm_decl_primitive(type) != 0u
        || type->kind == CM_HIR_TYPE_PARAMETER_KIND
        || type->kind == CM_HIR_TYPE_SELF_KIND) return 1;
    if (type->kind == CM_HIR_TYPE_ADT_KIND) {
        if ((type->data.named_type.argument_count == 0u)
                != (type->data.named_type.arguments == NULL)
            || (size_t)type->data.named_type.argument_count
                > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_decl_capture_private_dependency(state,
                type->data.named_type.definition, result)) return 0;
        for (index = 0u; index < type->data.named_type.argument_count;
                ++index) {
            if (type->data.named_type.arguments[index].kind
                    != CM_HIR_GENERIC_ARG_TYPE
                || !cm_decl_capture_dependency_type(state,
                    type->data.named_type.arguments[index].data.type,
                    depth + 1u, result)) return 0;
        }
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_REFERENCE_KIND)
        return cm_decl_capture_dependency_type(state,
            type->data.reference_type.pointee, depth + 1u, result);
    if (type->kind == CM_HIR_TYPE_SLICE_KIND)
        return cm_decl_capture_dependency_type(state,
            type->data.slice_type.element, depth + 1u, result);
    if (type->kind == CM_HIR_TYPE_RAW_POINTER_KIND)
        return cm_decl_capture_dependency_type(state,
            type->data.raw_pointer_type.pointee, depth + 1u, result);
    if (type->kind == CM_HIR_TYPE_ARRAY_KIND)
        return cm_decl_capture_dependency_type(state,
                type->data.array_type.element, depth + 1u, result)
            && cm_decl_capture_dependency_type(state,
                type->data.array_type.length.type, depth + 1u, result);
    if (type->kind == CM_HIR_TYPE_TUPLE_KIND) {
        if (type->data.tuple_type.element_count == 0u
            || type->data.tuple_type.elements == NULL
            || (size_t)type->data.tuple_type.element_count
                > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) return 0;
        for (index = 0u; index < type->data.tuple_type.element_count; ++index)
            if (!cm_decl_capture_dependency_type(state,
                    type->data.tuple_type.elements[index], depth + 1u,
                    result)) return 0;
        return 1;
    }
    if (type->kind == CM_HIR_TYPE_PROJECTION_KIND) {
        if (!cm_decl_capture_dependency_type(state,
                type->data.projection_type.self_type, depth + 1u, result)
            || !cm_decl_capture_private_dependency(state,
                type->data.projection_type.trait_type.definition, result)
            || !cm_decl_capture_private_dependency(state,
                type->data.projection_type.associated_type.definition,
                result)
            || type->data.projection_type.associated_type.argument_count != 0u
            || type->data.projection_type.associated_type.arguments != NULL)
            return 0;
        for (index = 0u;
                index < type->data.projection_type.trait_type.argument_count;
                ++index) {
            if (type->data.projection_type.trait_type.arguments == NULL
                || type->data.projection_type.trait_type.arguments[index].kind
                    != CM_HIR_GENERIC_ARG_TYPE
                || !cm_decl_capture_dependency_type(state,
                    type->data.projection_type.trait_type.arguments[index]
                        .data.type, depth + 1u, result)) return 0;
        }
        return 1;
    }
    return 0;
}

static int cm_decl_discover_private_dependencies(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    size_t item_index;
    for (item_index = 0u; item_index < state->item_count; ++item_index) {
        const CmHirItem *item = state->items[item_index].item;
        uint32_t field_index;
        if (item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION) {
            for (field_index = 0u;
                    field_index < item->data.aggregate_item.field_count;
                    ++field_index)
                if (!cm_decl_capture_dependency_type(state,
                        item->data.aggregate_item.fields[field_index].type,
                        0u, result)) return 0;
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                for (field_index = 0u; field_index < variant->field_count;
                        ++field_index)
                    if (!cm_decl_capture_dependency_type(state,
                            variant->fields[field_index].type, 0u, result))
                        return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            if (!cm_decl_capture_dependency_type(state,
                    item->data.type_alias_item.target, 0u, result)) return 0;
        }
        for (field_index = 0u; field_index < item->predicate_count;
                ++field_index) {
            const CmHirTraitPredicate *predicate =
                &item->predicates[field_index];
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->trait_type.argument_count != 0u
                || predicate->trait_type.arguments != NULL
                || predicate->equality_count != 0u
                || predicate->equalities != NULL
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || !cm_decl_capture_dependency_type(state,
                    predicate->subject, 0u, result)
                || !cm_decl_capture_private_dependency(state,
                    predicate->trait_type.definition, result)) return 0;
        }
    }
    for (item_index = 0u; item_index < state->associated_count;
            ++item_index) {
        const CmHirItem *method = state->associated_items[item_index].item;
        const CmHirFunctionSignature *signature;
        uint32_t parameter;
        if (method == NULL) return 0;
        if (method->kind == CM_HIR_ITEM_TYPE_ALIAS) continue;
        if (method->kind != CM_HIR_ITEM_FUNCTION) return 0;
        signature = &method->data.function_item.signature;
        for (parameter = 0u; parameter < signature->parameter_count;
                ++parameter)
            if (!cm_decl_capture_dependency_type(state,
                    signature->parameters[parameter].type, 0u, result))
                return 0;
        if (!cm_decl_capture_dependency_type(state, signature->return_type,
                0u, result)) return 0;
    }
    for (item_index = 0u; item_index < state->value_count; ++item_index) {
        const CmHirItem *value = state->values[item_index].item;
        uint32_t child;
        if (value->kind != CM_HIR_ITEM_FUNCTION) continue;
        for (child = 0u;
                child < value->data.function_item.signature.parameter_count;
                ++child)
            if (!cm_decl_capture_dependency_type(state,
                    value->data.function_item.signature.parameters[child]
                        .type, 0u, result)) return 0;
        if (!cm_decl_capture_dependency_type(state,
                value->data.function_item.signature.return_type, 0u,
                result)) return 0;
        for (child = 0u; child < value->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &value->predicates[child];
            uint32_t argument;
            if (!cm_decl_capture_dependency_type(state, predicate->subject,
                    0u, result)
                || !cm_decl_capture_private_dependency(state,
                    predicate->trait_type.definition, result)) return 0;
            for (argument = 0u;
                    argument < predicate->trait_type.argument_count;
                    ++argument)
                if (predicate->trait_type.arguments == NULL
                    || predicate->trait_type.arguments[argument].kind
                        != CM_HIR_GENERIC_ARG_TYPE
                    || !cm_decl_capture_dependency_type(state,
                        predicate->trait_type.arguments[argument].data.type,
                        0u, result)) return 0;
            for (argument = 0u; argument < predicate->equality_count;
                    ++argument)
                if (predicate->equalities == NULL
                    || !cm_decl_capture_private_dependency(state,
                        predicate->equalities[argument].associated_type,
                        result)
                    || !cm_decl_capture_dependency_type(state,
                        predicate->equalities[argument].value, 0u, result))
                    return 0;
        }
    }
    return 1;
}

static int cm_decl_free_value_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, CmHirItemKind hir_kind,
    CmAstItemKind ast_kind, CmHirLibraryValueKind library_kind,
    uint32_t owner_module, uint32_t source_ordinal)
{
    const CmDeclCaptureModule *module;
    const CmHirLibraryOwnedValue *owned;
    const CmHirType *type;
    const CmHirBody *body;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmAstType *ast_type;
    const CmAstExpr *initializer;
    CmResolveEffectiveItem effective;
    if (item->kind != hir_kind
        || !cm_decl_plain_visibility(item)
        || !cm_hir_def_id_is_none(item->parent_definition)
        || item->is_specializable
        || item->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        || item->generic_parameter_count != 0u
        || item->predicate_scopes != NULL
        || item->predicate_scope_count != 0u
        || item->predicates != NULL || item->predicate_count != 0u
        || item->outlives_predicates != NULL
        || item->outlives_predicate_count != 0u
        || (hir_kind == CM_HIR_ITEM_CONST
            && item->data.value_item.mutability != CM_HIR_IMMUTABLE)
        || item->data.value_item.has_default_body != 0
        || !cm_hir_def_id_is_none(
            item->data.value_item.trait_item_definition)
        || item->data.value_item.body == CM_HIR_BODY_NONE
        || (type = cm_hir_get_type(state->hir,
            item->data.value_item.type)) == NULL
        || (hir_kind == CM_HIR_ITEM_CONST && cm_decl_primitive(type) == 0u)
        || type->span.source != item->span.source
        || type->span.start > type->span.end
        || type->span.start < item->span.start
        || type->span.end > item->span.end) return 0;
    owned = cm_decl_owned_value(state->owned, item->definition);
    if (owned == NULL
        || owned->storage_kind != library_kind
        || owned->declaration.kind != library_kind
        || !cm_hir_def_id_equal(owned->declaration.definition,
            item->definition)
        || owned->declaration.data.value.type != item->data.value_item.type
        || owned->declaration.data.value.mutability
            != item->data.value_item.mutability
        || owned->parameter_types != NULL || owned->parameter_count != 0u
        || owned->predicate_scopes != NULL
        || owned->predicate_scope_lifetimes != NULL
        || owned->predicate_scope_count != 0u
        || owned->predicates != NULL
        || owned->predicate_arguments != NULL
        || owned->predicate_equalities != NULL
        || owned->predicate_lifetimes != NULL
        || owned->predicate_count != 0u
        || owned->outlives_predicates != NULL
        || owned->outlives_predicate_count != 0u
        || owned->nominal_references != NULL
        || owned->nominal_reference_names != NULL
        || owned->nominal_reference_generic_kinds != NULL
        || owned->nominal_reference_count != 0u
        || owned->associated_availability != NULL
        || owned->associated_availability_count != 0u) return 0;
    module = cm_decl_module_by_local((CmDeclCaptureState *)state,
        owner_module);
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id, source_ordinal,
            &effective) != CM_RESOLVE_VIEW_OK
        || effective.is_generated || effective.item_kind != ast_kind
        || !cm_hir_def_id_equal(item->definition,
            owned->declaration.definition)
        || !cm_module_graph_borrow_ast(state->input->graph,
            module->graph.id, &ast) || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            effective.declaration.item)) == NULL
        || ast_item->kind != ast_kind
        || ast_item->visibility.kind != CM_AST_VIS_PUBLIC
        || ast_item->visibility.restriction != CM_AST_PATH_NONE
        || ast_item->is_default
        || ast_item->generic_parameters != NULL
        || ast_item->generic_parameter_count != 0u
        || ast_item->where_clause != CM_INTERN_ID_NONE
        || ast_item->where_predicates != NULL
        || ast_item->where_predicate_count != 0u
        || ast_item->data.value_item.type == CM_AST_TYPE_NONE
        || !ast_item->data.value_item.has_value
        || ast_item->data.value_item.initializer == CM_AST_EXPR_NONE
        || ast_item->data.value_item.is_mutable
            != (item->data.value_item.mutability == CM_HIR_MUTABLE)
        || ast_item->data.value_item.bounds != NULL
        || ast_item->data.value_item.bound_count != 0u
        || ast_item->data.value_item.post_value_where_clause
            != CM_INTERN_ID_NONE
        || ast_item->data.value_item.post_value_where_predicates != NULL
        || ast_item->data.value_item.post_value_where_predicate_count != 0u
        || ast_item->span.start != item->span.start
        || ast_item->span.end != item->span.end
        || (ast_type = cm_ast_get_type(ast,
            ast_item->data.value_item.type)) == NULL
        || (hir_kind == CM_HIR_ITEM_CONST
            ? !cm_decl_ast_type_matches_hir_primitive(ast, ast_type, type)
            : !cm_decl_ast_type_matches_hir_static(state, ast, ast_type, type,
                0u))
        || ast_type->span.start != type->span.start
        || ast_type->span.end != type->span.end
        || (initializer = cm_ast_get_expr(ast,
            ast_item->data.value_item.initializer)) == NULL
        || ((initializer->attribute_count == 0u)
            != (initializer->attributes == NULL))
        || initializer->attribute_count != 0u
        || initializer->span.start > initializer->span.end
        || initializer->span.start < ast_item->span.start
        || initializer->span.end > ast_item->span.end) return 0;
    body = cm_hir_get_body(state->hir, item->data.value_item.body);
    return body != NULL
        && cm_hir_def_id_equal(body->owner, item->definition)
        && body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
        && cm_hir_def_id_equal(body->origin.definition, item->definition)
        && cm_hir_def_id_equal(body->origin.enclosing_definition,
            item->definition)
        && cm_hir_def_id_equal(
            body->origin.data.item_source.item_definition, item->definition)
        && body->state == CM_HIR_BODY_UNLOWERED
        && body->expected_type == item->data.value_item.type
        && body->locals == NULL && body->local_count == 0u
        && body->parameter_count == 0u
        && body->source == effective.declaration.source
        && body->source == item->span.source
        && body->source_expression_id
            == ast_item->data.value_item.initializer
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->error_reason == CM_INTERN_ID_NONE
        && body->span.source == item->span.source
        && body->span.start == item->span.start
        && body->span.end == item->span.end;
}

static int cm_decl_const_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t owner_module, uint32_t source_ordinal)
{
    return cm_decl_free_value_shape(state, item, CM_HIR_ITEM_CONST,
        CM_AST_ITEM_CONST, CM_HIR_LIBRARY_VALUE_CONST, owner_module,
        source_ordinal);
}

static int cm_decl_static_shape(const CmDeclCaptureState *state,
    const CmHirItem *item, uint32_t owner_module, uint32_t source_ordinal)
{
    return cm_decl_free_value_shape(state, item, CM_HIR_ITEM_STATIC,
        CM_AST_ITEM_STATIC, CM_HIR_LIBRARY_VALUE_STATIC, owner_module,
        source_ordinal);
}

static int cm_decl_def_id_compare(CmHirDefId left, CmHirDefId right)
{
    if (left.crate_id != right.crate_id)
        return left.crate_id < right.crate_id ? -1 : 1;
    if (left.index != right.index) return left.index < right.index ? -1 : 1;
    return 0;
}

static int cm_decl_trait_local_pair_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclTraitLocalPair *left =
        (const CmDeclTraitLocalPair *)left_value;
    const CmDeclTraitLocalPair *right =
        (const CmDeclTraitLocalPair *)right_value;
    return cm_decl_def_id_compare(left->definition, right->definition);
}

static int cm_decl_order_associated_items(CmDeclCaptureState *state)
{
    size_t index;
    for (index = 1u; index < state->associated_count; ++index) {
        CmDeclCaptureItem value = state->associated_items[index];
        uint32_t value_parent = cm_decl_trait_local(state,
            value.item->parent_definition);
        size_t cursor = index;
        if (value_parent == 0u) return 0;
        while (cursor != 0u) {
            const CmDeclCaptureItem *prior =
                &state->associated_items[cursor - 1u];
            uint32_t prior_parent = cm_decl_trait_local(state,
                prior->item->parent_definition);
            if (prior_parent == 0u) return 0;
            if (prior_parent < value_parent
                || (prior_parent == value_parent
                    && prior->source_ordinal < value.source_ordinal)) break;
            state->associated_items[cursor] = *prior;
            cursor -= 1u;
        }
        state->associated_items[cursor] = value;
    }
    for (index = 0u; index < state->trait_count; ++index) {
        state->traits[index].associated_start = 0u;
        state->traits[index].associated_count = 0u;
    }
    for (index = 0u; index < state->associated_count; ++index) {
        uint32_t parent = cm_decl_trait_local(state,
            state->associated_items[index].item->parent_definition);
        CmDeclCaptureItem *trait_capture;
        if (parent == 0u) return 0;
        trait_capture = &state->traits[parent - 1u];
        if (trait_capture->associated_count == 0u)
            trait_capture->associated_start = (uint32_t)(index + 1u);
        else if (state->associated_items[index - 1u].source_ordinal
                >= state->associated_items[index].source_ordinal) return 0;
        trait_capture->associated_count += 1u;
    }
    return 1;
}

static int cm_decl_collect_items(CmDeclCaptureState *state,
    CmHirDeclarationCaptureResult *result)
{
    size_t index;
    if (!cm_decl_reexport_attributes(state, result)) return 0;
    if (state->hir->items.len
            > CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS) {
        return cm_decl_capture_fail(result, CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED);
    }
    state->traits = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->namespace_count, sizeof(*state->traits));
    state->associated_items = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->hir->items.len, sizeof(*state->associated_items));
    state->item_capacity = state->hir->items.len;
    if (state->item_capacity > CM_HIR_DECL_METADATA_MAX_ITEMS)
        state->item_capacity = CM_HIR_DECL_METADATA_MAX_ITEMS;
    state->items = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->item_capacity, sizeof(*state->items));
    state->values = (CmDeclCaptureItem *)cm_alloc_zeroed(
        state->namespace_count, sizeof(*state->values));
    for (index = 0u; index < state->namespace_count; ++index) {
        CmDeclCaptureNamespace *entry = &state->namespace_values[index];
        CmDeclCaptureItem value;
        memset(&value, 0, sizeof(value));
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_MODULE) {
            if (cm_decl_module_by_definition(state,
                    entry->target.definition) == NULL) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                    entry, NULL, CM_HIR_ITEM_NONE);
                return 0;
            }
            continue;
        }
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_PRIMITIVE)
            continue;
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) {
            CmHirItemId enum_item_id = CM_HIR_ITEM_NONE;
            if (cm_decl_enum_variant_parent(state, &entry->target,
                    &enum_item_id) == NULL) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND,
                    entry, NULL, enum_item_id);
                return 0;
            }
            continue;
        }
        value.item = cm_decl_bound_item(state->hir,
            entry->target.definition, &value.id);
        if (value.item == NULL
            || value.item->definition.crate_id != state->input->crate_id) {
            cm_decl_capture_item_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND,
                entry, value.item, value.id);
            return 0;
        }
        if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
            size_t projected_count;
            if (cm_decl_item_already(state->traits, state->trait_count,
                    value.item->definition)) continue;
            if (!cm_decl_trait_source_and_members(state, &value,
                    &projected_count)) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                    entry, value.item, value.id);
                return 0;
            }
            if (projected_count > SIZE_MAX
                    - state->projected_semantic_attribute_count)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
            state->projected_semantic_attribute_count += projected_count;
            state->traits[state->trait_count++] = value;
        } else if (entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE
            || entry->target.kind
                == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR) {
            size_t projected_count;
            int non_exhaustive;
            if (cm_decl_item_already(state->items, state->item_count,
                    value.item->definition)) continue;
            if (state->item_count == CM_HIR_DECL_METADATA_MAX_ITEMS) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (value.item->kind == CM_HIR_ITEM_STRUCT) {
                if (value.item->data.aggregate_item.form
                        == CM_HIR_AGGREGATE_UNIT) {
                    if (!cm_decl_unit_struct_shape(value.item)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                            entry, value.item, value.id);
                        return 0;
                    }
                    if (!cm_decl_project_item_attributes(state, value.item,
                            CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                                | CM_DECL_ATTR_DEPRECATED | CM_DECL_ATTR_DERIVE
                                | CM_DECL_ATTR_NON_EXHAUSTIVE,
                            &projected_count, &non_exhaustive)
                        || !cm_decl_item_attribute_provenance(state,
                            value.item, CM_AST_ITEM_STRUCT,
                            CM_HIR_LIBRARY_BINDING_TYPE)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                            entry, value.item, value.id);
                        return 0;
                    }
                    if (!cm_decl_struct_source(state, value.item,
                            non_exhaustive, &value.owner_module,
                            &value.source_ordinal)) {
                        cm_decl_capture_item_failure(result,
                            CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                            entry, value.item, value.id);
                        return 0;
                    }
                } else if (!cm_decl_aggregate_shape_and_source(state, &value,
                        &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_UNION
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (!cm_decl_aggregate_shape_and_source(state, &value,
                        &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_TYPE_ALIAS
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                if (!cm_decl_type_alias_shape(state, value.item)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_project_item_attributes(state, value.item,
                        CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                            | CM_DECL_ATTR_DEPRECATED,
                        &projected_count, &non_exhaustive)
                    || non_exhaustive
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_TYPE_ALIAS,
                        CM_HIR_LIBRARY_BINDING_TYPE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_alias_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
            } else if (value.item->kind == CM_HIR_ITEM_ENUM
                && entry->target.kind == CM_HIR_LIBRARY_BINDING_TYPE) {
                int enum_profile;
                const unsigned char *enum_lang = NULL;
                size_t enum_lang_length = 0u;
                if (!cm_decl_enum_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_enum_item_attributes(state, value.item,
                        &projected_count, &enum_profile, &enum_lang,
                        &enum_lang_length, &value.enum_repr)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_enum_shape_and_variants(state, value.item,
                        value.id, value.owner_module, value.source_ordinal,
                        enum_profile, value.enum_repr, &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                value.lang_item = enum_lang;
                value.lang_item_length = enum_lang_length;
            } else {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (projected_count > SIZE_MAX
                    - state->projected_semantic_attribute_count)
                return cm_decl_capture_fail(result,
                    CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                    CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
            state->projected_semantic_attribute_count += projected_count;
            state->items[state->item_count++] = value;
        } else if (entry->target.kind == CM_HIR_LIBRARY_BINDING_VALUE) {
            size_t projected_count = 0u;
            int non_exhaustive = 0;
            if (cm_decl_item_already(state->values, state->value_count,
                    value.item->definition)) continue;
            if (state->value_count == CM_HIR_DECL_METADATA_MAX_VALUES) {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            if (value.item->kind == CM_HIR_ITEM_FUNCTION) {
                if (!cm_decl_function_shape(state, value.item,
                        &value.owner_module, &value.source_ordinal,
                        &projected_count)) {
                    cm_decl_capture_item_failure(result,
                        value.item->data.function_item.signature.is_const
                                || value.item->attribute_count != 0u
                            ? CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID
                            : CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (projected_count > SIZE_MAX
                        - state->projected_semantic_attribute_count)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                        CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
                state->projected_semantic_attribute_count += projected_count;
            } else if (value.item->kind == CM_HIR_ITEM_CONST) {
                if (!cm_decl_const_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_project_item_attributes(state, value.item,
                        CM_DECL_ATTR_STABLE | CM_DECL_ATTR_UNSTABLE
                            | CM_DECL_ATTR_DEPRECATED,
                        &projected_count, &non_exhaustive)
                    || non_exhaustive
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_CONST,
                        CM_HIR_LIBRARY_BINDING_VALUE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_const_shape(state, value.item,
                        value.owner_module, value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (projected_count > SIZE_MAX
                        - state->projected_semantic_attribute_count)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                        CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
                state->projected_semantic_attribute_count += projected_count;
            } else if (value.item->kind == CM_HIR_ITEM_STATIC) {
                if (!cm_decl_static_source(state, value.item,
                        &value.owner_module, &value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_static_attributes(state, value.item,
                        &projected_count)
                    || !cm_decl_item_attribute_provenance(state, value.item,
                        CM_AST_ITEM_STATIC,
                        CM_HIR_LIBRARY_BINDING_VALUE)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (!cm_decl_static_shape(state, value.item,
                        value.owner_module, value.source_ordinal)) {
                    cm_decl_capture_item_failure(result,
                        CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                        entry, value.item, value.id);
                    return 0;
                }
                if (projected_count > SIZE_MAX
                        - state->projected_semantic_attribute_count)
                    return cm_decl_capture_fail(result,
                        CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
                        CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT);
                state->projected_semantic_attribute_count += projected_count;
            } else {
                cm_decl_capture_item_failure(result,
                    CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED,
                    entry, value.item, value.id);
                return 0;
            }
            state->values[state->value_count++] = value;
        } else {
            cm_decl_capture_item_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED,
                entry, value.item, value.id);
            return 0;
        }
    }
    if (!cm_decl_discover_private_dependencies(state, result))
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED);
    cm_decl_sort_items(state->traits, state->trait_count, state);
    cm_decl_sort_items(state->items, state->item_count, state);
    cm_decl_sort_items(state->values, state->value_count, state);
    for (index = 0u; index < state->trait_count; ++index)
        state->traits[index].local = (uint32_t)(index + 1u);
    for (index = 0u; index < state->value_count; ++index)
        state->values[index].local = (uint32_t)(index + 1u);
    for (index = 0u; index < state->item_count; ++index)
        state->items[index].local = (uint32_t)(index + 1u);
    state->item_locals_by_hir_id = (uint32_t *)cm_alloc_zeroed(
        state->hir->items.len, sizeof(*state->item_locals_by_hir_id));
    state->trait_local_pairs = (CmDeclTraitLocalPair *)cm_alloc_zeroed(
        state->trait_count, sizeof(*state->trait_local_pairs));
    for (index = 0u; index < state->item_count; ++index) {
        CmHirItemId id = state->items[index].id;
        if (id == CM_HIR_ITEM_NONE || (size_t)id > state->hir->items.len
            || state->item_locals_by_hir_id[id - 1u] != 0u) return 0;
        state->item_locals_by_hir_id[id - 1u] =
            state->items[index].local;
    }
    for (index = 0u; index < state->trait_count; ++index) {
        CmHirItemId id = state->traits[index].id;
        const CmHirItem *item = cm_hir_get_item(state->hir, id);
        if (id == CM_HIR_ITEM_NONE || (size_t)id > state->hir->items.len
            || item == NULL || item != state->traits[index].item
            || item->kind != CM_HIR_ITEM_TRAIT
            || cm_hir_def_id_is_none(item->definition)
            || state->traits[index].local != (uint32_t)(index + 1u)) return 0;
        state->trait_local_pairs[index].definition = item->definition;
        state->trait_local_pairs[index].local = state->traits[index].local;
    }
    if (state->trait_count > 1u)
        qsort(state->trait_local_pairs, state->trait_count,
            sizeof(*state->trait_local_pairs),
            cm_decl_trait_local_pair_compare);
    for (index = 1u; index < state->trait_count; ++index)
        if (cm_hir_def_id_equal(
                state->trait_local_pairs[index - 1u].definition,
                state->trait_local_pairs[index].definition)) return 0;
    if (!cm_decl_order_associated_items(state))
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED);
    for (index = 0u; index < state->trait_count; ++index) {
        if (!cm_decl_trait_shape(state, &state->traits[index])) {
            cm_decl_capture_item_failure(result,
                CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED, NULL,
                state->traits[index].item, state->traits[index].id);
            return 0;
        }
    }
    if (state->trait_count == 0u || state->value_count == 0u)
        return cm_decl_capture_fail(result,
            CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_REQUIRED_ITEMS_MISSING);
    return 1;
}

static uint32_t cm_decl_trait_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    if (state->trait_local_pairs != NULL) {
        size_t begin = 0u;
        size_t end = state->trait_count;
        while (begin < end) {
            size_t middle = begin + (end - begin) / 2u;
            const CmDeclTraitLocalPair *pair =
                &state->trait_local_pairs[middle];
            if (cm_decl_def_id_compare(pair->definition, definition) < 0)
                begin = middle + 1u;
            else end = middle;
        }
        if (begin < state->trait_count
            && cm_hir_def_id_equal(
                state->trait_local_pairs[begin].definition, definition))
            return state->trait_local_pairs[begin].local;
        return 0u;
    }
    for (index = 0u; index < state->trait_count; ++index)
        if (cm_hir_def_id_equal(state->traits[index].item->definition,
                definition)) return state->traits[index].local;
    return 0u;
}

static uint32_t cm_decl_value_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    size_t index;
    for (index = 0u; index < state->value_count; ++index)
        if (cm_hir_def_id_equal(state->values[index].item->definition,
                definition)) return state->values[index].local;
    return 0u;
}

static uint32_t cm_decl_item_local(const CmDeclCaptureState *state,
    CmHirDefId definition)
{
    const CmHirDefinition *bound;
    size_t index;
    bound = cm_hir_lookup_definition(state->hir, definition);
    if (state->item_locals_by_hir_id != NULL && bound != NULL
        && bound->kind == CM_HIR_DEFINITION_ITEM
        && bound->state == CM_HIR_DEFINITION_BOUND
        && bound->entity.item_id != CM_HIR_ITEM_NONE
        && (size_t)bound->entity.item_id <= state->hir->items.len)
        return state->item_locals_by_hir_id[bound->entity.item_id - 1u];
    for (index = 0u; index < state->item_count; ++index)
        if (cm_hir_def_id_equal(state->items[index].item->definition,
                definition)) return state->items[index].local;
    return 0u;
}

static uint32_t cm_decl_enum_variant_local(
    const CmDeclCaptureState *state, const CmHirLibraryBinding *target)
{
    size_t item_index;
    uint32_t local = 0u;
    if (target == NULL
        || target->kind != CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) return 0u;
    for (item_index = 0u; item_index < state->item_count; ++item_index) {
        const CmHirItem *item = state->items[item_index].item;
        uint32_t variant_index;
        if (item->kind != CM_HIR_ITEM_ENUM) continue;
        for (variant_index = 0u;
                variant_index < item->data.enum_item.variant_count;
                ++variant_index) {
            const CmHirVariant *variant =
                &item->data.enum_item.variants[variant_index];
            if (local == UINT32_MAX) return 0u;
            local += 1u;
            if (cm_hir_def_id_equal(item->definition,
                    target->enum_definition)
                && variant_index == target->enum_variant_index
                && cm_hir_def_id_equal(variant->definition,
                    target->definition)
                && variant->form == target->enum_variant_form)
                return local;
        }
    }
    return 0u;
}

static uint8_t cm_decl_primitive(const CmHirType *type)
{
    if (type == NULL) return 0u;
    switch (type->kind) {
    case CM_HIR_TYPE_UNIT_KIND: return CM_HIR_DECL_PRIMITIVE_UNIT;
    case CM_HIR_TYPE_BOOL_KIND: return CM_HIR_DECL_PRIMITIVE_BOOL;
    case CM_HIR_TYPE_CHAR_KIND: return CM_HIR_DECL_PRIMITIVE_CHAR;
    case CM_HIR_TYPE_STR_KIND: return CM_HIR_DECL_PRIMITIVE_STR;
    case CM_HIR_TYPE_INTEGER_KIND:
        return (uint8_t)(CM_HIR_DECL_PRIMITIVE_I8
            + (unsigned int)type->data.integer_type.kind);
    case CM_HIR_TYPE_FLOAT_KIND:
        if (type->data.float_type.kind == CM_HIR_FLOAT_F32)
            return CM_HIR_DECL_PRIMITIVE_F32;
        if (type->data.float_type.kind == CM_HIR_FLOAT_F64)
            return CM_HIR_DECL_PRIMITIVE_F64;
        return 0u;
    default: return 0u;
    }
}

static int cm_decl_mark_type_depth(CmDeclCaptureState *state,
    CmHirTypeId type_id, CmHirDeclarationCaptureResult *result)
{
    const CmHirType *type;
    uint8_t primitive;
    uint32_t child;
    uint32_t item_local;
    uint32_t maximum_child_depth = 0u;
    if (type_id == CM_HIR_TYPE_NONE
        || (size_t)type_id > state->hir->types.len
        || state->type_visits == NULL || state->type_depths == NULL) goto bad;
    if (state->type_visits[type_id - 1u] == 2u) return 1;
    if (state->type_visits[type_id - 1u] == 1u) goto bad;
    state->type_visits[type_id - 1u] = 1u;
    type = cm_hir_get_type(state->hir, type_id);
    primitive = cm_decl_primitive(type);
    if (primitive != 0u) {
        state->primitive_types[primitive] = 1u;
        goto leaf;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND) {
        uint32_t trait_local = cm_decl_trait_local(state,
            type->data.self_type.owner);
        if (trait_local == 0u) goto bad;
        state->self_types[trait_local - 1u] = 1u;
        goto leaf;
    }
    if (type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
        CmHirGenericParamId parameter = type->data.parameter_type.parameter;
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, parameter);
        if (generic == NULL || generic->kind != CM_HIR_GENERIC_TYPE
            || parameter == CM_HIR_GENERIC_PARAM_NONE
            || (size_t)parameter > state->hir->generic_parameters.len
            || state->generic_locals[parameter - 1u] == 0u) goto bad;
        state->generic_types[state->generic_locals[parameter - 1u] - 1u] = 1u;
        goto leaf;
    }
#define CM_DECL_MARK_CHILD(child_id_) do { \
    CmHirTypeId marked_child_ = (child_id_); \
    uint32_t child_depth_; \
    if (!cm_decl_mark_type_depth(state, marked_child_, result)) return 0; \
    child_depth_ = state->type_depths[marked_child_ - 1u]; \
    if (child_depth_ > maximum_child_depth) \
        maximum_child_depth = child_depth_; \
} while (0)
    if (type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.pointee != CM_HIR_TYPE_NONE
        && (type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            || type->data.reference_type.mutability == CM_HIR_MUTABLE)
        && (type->data.reference_type.region.kind == CM_HIR_REGION_ERASED
            || type->data.reference_type.region.kind
                == CM_HIR_REGION_STATIC)) {
        CM_DECL_MARK_CHILD(type->data.reference_type.pointee);
        state->compound_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_SLICE_KIND
        && type->data.slice_type.element != CM_HIR_TYPE_NONE) {
        CM_DECL_MARK_CHILD(type->data.slice_type.element);
        state->compound_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && type->data.raw_pointer_type.pointee != CM_HIR_TYPE_NONE
        && (type->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE
            || type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE)) {
        CM_DECL_MARK_CHILD(type->data.raw_pointer_type.pointee);
        state->compound_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count != 0u
        && type->data.tuple_type.element_count
            <= (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
        && type->data.tuple_type.elements != NULL) {
        for (child = 0u; child < type->data.tuple_type.element_count; ++child)
            CM_DECL_MARK_CHILD(type->data.tuple_type.elements[child]);
        state->compound_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_ARRAY_KIND
        && type->data.array_type.element != CM_HIR_TYPE_NONE
        && ((type->data.array_type.length.kind == CM_HIR_CONST_VALUE
                && type->data.array_type.length.data.value.high_bits == 0u
                && cm_decl_primitive(cm_hir_get_type(state->hir,
                    type->data.array_type.length.type))
                    == CM_HIR_DECL_PRIMITIVE_USIZE)
            || (type->data.array_type.length.kind == CM_HIR_CONST_PARAMETER
                && type->data.array_type.length.data.parameter
                    != CM_HIR_GENERIC_PARAM_NONE
                && (size_t)type->data.array_type.length.data.parameter
                    <= state->hir->generic_parameters.len
                && cm_hir_get_generic_param(state->hir,
                    type->data.array_type.length.data.parameter) != NULL
                && cm_hir_get_generic_param(state->hir,
                    type->data.array_type.length.data.parameter)->kind
                    == CM_HIR_GENERIC_CONST
                && cm_decl_primitive(cm_hir_get_type(state->hir,
                    cm_hir_get_generic_param(state->hir,
                        type->data.array_type.length.data.parameter)
                            ->declared_type))
                    == CM_HIR_DECL_PRIMITIVE_USIZE
                && state->generic_locals[type->data.array_type.length.data
                        .parameter - 1u] != 0u))) {
        CM_DECL_MARK_CHILD(type->data.array_type.element);
        if (type->data.array_type.length.kind == CM_HIR_CONST_VALUE)
            CM_DECL_MARK_CHILD(type->data.array_type.length.type);
        state->compound_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count != 0u
        && type->data.named_type.argument_count
            <= (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
        && type->data.named_type.arguments != NULL
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && (state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_UNION
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_ENUM)
        && state->items[item_local - 1u].item->generic_parameter_count
            == type->data.named_type.argument_count) {
        for (child = 0u; child < type->data.named_type.argument_count;
                ++child) {
            if (type->data.named_type.arguments[child].kind
                    != CM_HIR_GENERIC_ARG_TYPE) goto bad;
            CM_DECL_MARK_CHILD(
                type->data.named_type.arguments[child].data.type);
        }
        state->application_types[type_id - 1u] = 1u;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && state->items[item_local - 1u].item->generic_parameter_count == 0u
        && (state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_UNION
            || state->items[item_local - 1u].item->kind == CM_HIR_ITEM_ENUM)) {
        state->named_item_types[item_local - 1u] = 1u;
        goto leaf;
    } else if (type != NULL && type->kind == CM_HIR_TYPE_PROJECTION_KIND
        && type->data.projection_type.self_type != CM_HIR_TYPE_NONE
        && cm_decl_trait_local(state,
            type->data.projection_type.trait_type.definition) != 0u
        && cm_decl_associated_local(state,
            type->data.projection_type.associated_type.definition) != 0u
        && type->data.projection_type.associated_type.argument_count == 0u
        && type->data.projection_type.associated_type.arguments == NULL
        && type->data.projection_type.trait_type.argument_count
            <= (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
        && (type->data.projection_type.trait_type.argument_count == 0u
            || type->data.projection_type.trait_type.arguments != NULL)) {
        CM_DECL_MARK_CHILD(type->data.projection_type.self_type);
        for (child = 0u;
                child < type->data.projection_type.trait_type.argument_count;
                ++child) {
            if (type->data.projection_type.trait_type.arguments[child].kind
                    != CM_HIR_GENERIC_ARG_TYPE) goto bad;
            CM_DECL_MARK_CHILD(type->data.projection_type.trait_type
                .arguments[child].data.type);
        }
        state->compound_types[type_id - 1u] = 1u;
    } else goto bad;
#undef CM_DECL_MARK_CHILD
    if (maximum_child_depth >= CM_META_MAX_TYPE_NESTING) goto bad;
    state->type_depths[type_id - 1u] = maximum_child_depth + 1u;
    state->type_visits[type_id - 1u] = 2u;
    return 1;
leaf:
    state->type_depths[type_id - 1u] = 0u;
    state->type_visits[type_id - 1u] = 2u;
    return 1;
bad:
    type = cm_hir_get_type(state->hir, type_id);
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA;
        result->failure_reason = CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED;
        result->rejected_type = type_id;
        if (type != NULL) {
            result->has_rejected_span = 1;
            result->rejected_span = type->span;
        }
    }
    return 0;
}

static int cm_decl_mark_type(CmDeclCaptureState *state, CmHirTypeId type_id,
    CmHirDeclarationCaptureResult *result)
{
    return cm_decl_mark_type_depth(state, type_id, result);
}

static int cm_decl_mark_named_adt(CmDeclCaptureState *state,
    CmHirTypeId type_id, CmHirDeclarationCaptureResult *result)
{
    const CmHirType *type = cm_hir_get_type(state->hir, type_id);
    uint32_t item_local;
    if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && type->data.named_type.argument_count == 0u
        && type->data.named_type.arguments == NULL
        && (item_local = cm_decl_item_local(state,
            type->data.named_type.definition)) != 0u
        && state->items[item_local - 1u].item->kind == CM_HIR_ITEM_STRUCT) {
        return cm_decl_mark_type(state, type_id, result);
    }
    if (result->failure_reason == CM_HIR_DECL_CAPTURE_REASON_NONE) {
        result->failure_stage = CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA;
        result->failure_reason = CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED;
        result->rejected_type = type_id;
        if (type != NULL) {
            result->has_rejected_span = 1;
            result->rejected_span = type->span;
        }
    }
    return 0;
}

static uint32_t cm_decl_type_local(const CmDeclCaptureState *state,
    const CmHirDeclarationMetadata *metadata, CmHirTypeId type_id)
{
    (void)metadata;
    if (type_id == CM_HIR_TYPE_NONE
        || (size_t)type_id > state->hir->types.len
        || state->canonical_type_locals == NULL) return 0u;
    return state->canonical_type_locals[type_id - 1u];
}

static int cm_decl_fill_identity(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    const CmInternedString *crate_name = cm_interner_get(&state->hir->strings,
        state->crate_value->name);
    const CmHirArtifactConfig *config = state->input->configuration;
    size_t index;
    uint8_t edition;
    switch (state->crate_value->edition) {
    case CM_HIR_EDITION_2015: edition = CM_HIR_DECL_EDITION_2015; break;
    case CM_HIR_EDITION_2018: edition = CM_HIR_DECL_EDITION_2018; break;
    case CM_HIR_EDITION_2021: edition = CM_HIR_DECL_EDITION_2021; break;
    case CM_HIR_EDITION_2024: edition = CM_HIR_DECL_EDITION_2024; break;
    default: return 0;
    }
    if (config->edition != (uint32_t)(edition == CM_HIR_DECL_EDITION_2015
            ? 2015u : edition == CM_HIR_DECL_EDITION_2018 ? 2018u
            : edition == CM_HIR_DECL_EDITION_2021 ? 2021u : 2024u)
        || !cm_decl_copy_intern(&metadata->crate_name, crate_name)
        || !cm_decl_copy_bytes(&metadata->crate_disambiguator,
            state->input->crate_disambiguator.data,
            state->input->crate_disambiguator.length)
        || !cm_decl_copy_bytes(&metadata->target_triple,
            state->input->target_triple.data,
            state->input->target_triple.length)
        || !cm_decl_copy_bytes(&metadata->data_layout,
            state->input->data_layout.data,
            state->input->data_layout.length)) return 0;
    metadata->edition = edition;
    if (config->panic_strategy.length == 5u
        && memcmp(config->panic_strategy.data, "abort", 5u) == 0)
        metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    else if (config->panic_strategy.length == 6u
        && memcmp(config->panic_strategy.data, "unwind", 6u) == 0)
        metadata->panic_strategy = CM_HIR_DECL_PANIC_UNWIND;
    else return 0;
    metadata->cfg_count = config->cfg_count;
    metadata->cfgs = config->cfg_count == 0u ? NULL
        : (CmHirDeclarationString *)cm_alloc_zeroed(config->cfg_count,
            sizeof(*metadata->cfgs));
    for (index = 0u; index < config->cfg_count; ++index)
        cm_decl_copy_bytes(&metadata->cfgs[index], config->cfgs[index].data,
            config->cfgs[index].length);
    return 1;
}

static int cm_decl_fill_modules(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    metadata->module_count = state->module_count;
    metadata->modules = (CmHirDeclarationModule *)cm_alloc_zeroed(
        metadata->module_count, sizeof(*metadata->modules));
    for (index = 0u; index < state->module_count; ++index) {
        const CmDeclCaptureModule *source = &state->modules[index];
        const CmInternedString *name = source->graph.parent == CM_MODULE_NONE
            ? cm_interner_get(&state->hir->strings, state->crate_value->name)
            : cm_interner_get(&state->hir->strings, source->hir->name);
        CmDeclCaptureModule *parent = source->graph.parent == CM_MODULE_NONE
            ? NULL : cm_decl_module_by_graph((CmDeclCaptureState *)state,
                source->graph.parent);
        if (!cm_decl_copy_intern(&metadata->modules[index].name, name))
            return 0;
        metadata->modules[index].parent_module = parent == NULL ? 0u
            : parent->local;
        if (parent == NULL) metadata->root_module = source->local;
    }
    return metadata->root_module != 0u;
}

static int cm_decl_enum_variant_source_ordinal(
    const CmDeclCaptureState *state, const CmDeclCaptureItem *capture,
    uint32_t variant_index, uint32_t *out_ordinal)
{
    CmDeclCaptureModule *module = cm_decl_module_by_local(
        (CmDeclCaptureState *)state, capture->owner_module);
    CmResolveEffectiveItem enumeration;
    CmResolveEffectiveVariant variant;
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id,
            capture->source_ordinal, &enumeration) != CM_RESOLVE_VIEW_OK
        || enumeration.item_kind != CM_AST_ITEM_ENUM
        || cm_module_graph_get_effective_variant(state->input->graph,
            state->input->revision, module->graph.id, enumeration.id,
            variant_index, &variant) != CM_RESOLVE_VIEW_OK) return 0;
    *out_ordinal = variant.declaration.index;
    return 1;
}

static int cm_decl_copy_enum_variant_lang(
    const CmDeclCaptureState *state, const CmDeclCaptureItem *capture,
    uint32_t variant_index, CmHirDeclarationString *out_lang)
{
    CmDeclCaptureModule *module = cm_decl_module_by_local(
        (CmDeclCaptureState *)state, capture->owner_module);
    CmResolveEffectiveItem enumeration;
    CmResolveEffectiveVariant effective;
    const CmAst *ast = NULL;
    const CmAstItem *ast_item;
    const CmAstVariant *ast_variant;
    if (module == NULL
        || cm_module_graph_get_effective_item(state->input->graph,
            state->input->revision, module->graph.id,
            capture->source_ordinal, &enumeration) != CM_RESOLVE_VIEW_OK
        || enumeration.item_kind != CM_AST_ITEM_ENUM
        || cm_module_graph_get_effective_variant(state->input->graph,
            state->input->revision, module->graph.id, enumeration.id,
            variant_index, &effective) != CM_RESOLVE_VIEW_OK
        || !cm_module_graph_borrow_item_ast(state->input->graph,
            module->graph.id, enumeration.declaration, &ast)
        || ast == NULL
        || (ast_item = cm_ast_get_item(ast,
            enumeration.declaration.item)) == NULL
        || ast_item->kind != CM_AST_ITEM_ENUM
        || effective.declaration.index
            >= ast_item->data.enum_item.variant_count
        || ast_item->data.enum_item.variants == NULL) return 0;
    ast_variant = &ast_item->data.enum_item.variants[
        effective.declaration.index];
    return cm_decl_enum_generic_variant_attributes(state, module,
        &enumeration, &effective, variant_index, ast, ast_variant,
        capture->item->data.enum_item.variants[variant_index].lang_item,
        out_lang);
}

static int cm_decl_fill_visibility(CmDeclCaptureState *state,
    CmHirVisibility source, CmHirDeclarationVisibility *out)
{
    CmDeclCaptureModule *restriction;
    out->restriction_module = 0u;
    if (source.kind == CM_HIR_VIS_PRIVATE)
        out->kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
    else if (source.kind == CM_HIR_VIS_PUBLIC)
        out->kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    else if (source.kind == CM_HIR_VIS_CRATE)
        out->kind = CM_HIR_DECL_VISIBILITY_CRATE;
    else if (source.kind == CM_HIR_VIS_RESTRICTED
        && (restriction = cm_decl_module_by_definition(state,
            source.restriction)) != NULL) {
        out->kind = CM_HIR_DECL_VISIBILITY_RESTRICTED;
        out->restriction_module = restriction->local;
    } else return 0;
    return source.kind == CM_HIR_VIS_RESTRICTED
        || cm_hir_def_id_is_none(source.restriction);
}

static int cm_decl_fill_items_and_generics(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t generic_count = 0u;
    size_t outlives_count = 0u;
    size_t outlives_cursor = 0u;
    size_t cursor = 0u;
    metadata->trait_count = state->trait_count;
    metadata->traits = (CmHirDeclarationTrait *)cm_alloc_zeroed(
        state->trait_count, sizeof(*metadata->traits));
    metadata->associated_count = state->associated_count;
    metadata->associated_items = state->associated_count == 0u ? NULL
        : (CmHirDeclarationAssociatedItem *)cm_alloc_zeroed(
            state->associated_count, sizeof(*metadata->associated_items));
    metadata->item_count = state->item_count;
    metadata->items = state->item_count == 0u ? NULL
        : (CmHirDeclarationItem *)cm_alloc_zeroed(state->item_count,
            sizeof(*metadata->items));
    metadata->value_count = state->value_count;
    metadata->values = (CmHirDeclarationValue *)cm_alloc_zeroed(
        state->value_count, sizeof(*metadata->values));
    for (index = 0u; index < state->trait_count; ++index)
        generic_count += state->traits[index].item->generic_parameter_count;
    for (index = 0u; index < state->trait_count; ++index) {
        if (state->traits[index].has_static_outlives) {
            if (outlives_count == CM_HIR_DECL_METADATA_MAX_PREDICATES)
                return 0;
            outlives_count += 1u;
        }
    }
    for (index = 0u; index < state->item_count; ++index)
        generic_count += state->items[index].item->generic_parameter_count;
    for (index = 0u; index < state->value_count; ++index)
        generic_count += state->values[index].item->generic_parameter_count;
    if (generic_count > CM_HIR_DECL_METADATA_MAX_RECORDS) return 0;
    metadata->generic_count = generic_count;
    metadata->generics = (CmHirDeclarationGeneric *)cm_alloc_zeroed(
        generic_count, sizeof(*metadata->generics));
    state->generic_locals = (uint32_t *)cm_alloc_zeroed(
        state->hir->generic_parameters.len, sizeof(*state->generic_locals));
    metadata->outlives_predicate_count = outlives_count;
    metadata->outlives_predicates = outlives_count == 0u ? NULL
        : (CmHirDeclarationOutlivesPredicate *)cm_alloc_zeroed(
            outlives_count, sizeof(*metadata->outlives_predicates));
    for (index = 0u; index < state->item_count; ++index) {
        const CmDeclCaptureItem *capture = &state->items[index];
        CmHirDeclarationItem *wire = &metadata->items[index];
        uint32_t variant_index;
        if (capture->item->kind == CM_HIR_ITEM_STRUCT)
            wire->kind = CM_HIR_DECL_ITEM_STRUCT;
        else if (capture->item->kind == CM_HIR_ITEM_UNION)
            wire->kind = CM_HIR_DECL_ITEM_UNION;
        else if (capture->item->kind == CM_HIR_ITEM_ENUM)
            wire->kind = CM_HIR_DECL_ITEM_ENUM;
        else wire->kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
        wire->owner_module = capture->owner_module;
        wire->source_ordinal = capture->source_ordinal;
        if (!cm_decl_fill_visibility(state, capture->item->visibility,
                &wire->visibility)) return 0;
        if (capture->item->kind == CM_HIR_ITEM_STRUCT
                || capture->item->kind == CM_HIR_ITEM_UNION) {
            uint32_t field_index;
            wire->aggregate_form = capture->item->data.aggregate_item.form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_HIR_DECL_AGGREGATE_UNIT
                : capture->item->data.aggregate_item.form
                    == CM_HIR_AGGREGATE_TUPLE
                ? CM_HIR_DECL_AGGREGATE_TUPLE
                : CM_HIR_DECL_AGGREGATE_NAMED;
            wire->aggregate_repr = capture->item->data.aggregate_item.form
                    == CM_HIR_AGGREGATE_UNIT
                ? CM_HIR_DECL_AGGREGATE_REPR_RUST
                : capture->aggregate_repr;
            wire->aggregate_flags = capture->aggregate_flags;
            wire->field_count =
                capture->item->data.aggregate_item.field_count;
            wire->fields = wire->field_count == 0u ? NULL
                : (CmHirDeclarationField *)cm_alloc_zeroed(
                    wire->field_count, sizeof(*wire->fields));
            if ((wire->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u
                && !cm_decl_copy_bytes(&wire->lang_item, capture->lang_item,
                    capture->lang_item_length)) return 0;
            if ((wire->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u
                && !cm_decl_copy_bytes(&wire->diagnostic_item,
                    capture->diagnostic_item,
                    capture->diagnostic_item_length)) return 0;
            for (field_index = 0u; field_index < wire->field_count;
                    ++field_index) {
                const CmHirField *field =
                    &capture->item->data.aggregate_item.fields[field_index];
                CmHirDeclarationField *wire_field =
                    &wire->fields[field_index];
                if (wire->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
                    && !cm_decl_copy_intern(&wire_field->name,
                        cm_interner_get(&state->hir->strings, field->name)))
                    return 0;
                if (field->visibility.kind == CM_HIR_VIS_PUBLIC) {
                    wire_field->visibility.kind =
                        CM_HIR_DECL_VISIBILITY_PUBLIC;
                } else if (field->visibility.kind == CM_HIR_VIS_CRATE) {
                    wire_field->visibility.kind =
                        CM_HIR_DECL_VISIBILITY_CRATE;
                } else if (field->visibility.kind == CM_HIR_VIS_PRIVATE) {
                    wire_field->visibility.kind =
                        CM_HIR_DECL_VISIBILITY_PRIVATE;
                } else return 0;
                wire_field->visibility.restriction_module = 0u;
                wire_field->source_ordinal = field_index;
            }
        } else if (capture->item->kind == CM_HIR_ITEM_ENUM) {
            const unsigned char *diagnostic_name = NULL;
            size_t diagnostic_name_length = 0u;
            int generic_default =
                capture->item->generic_parameter_count != 0u;
            int rust_default = capture->enum_repr
                == CM_HIR_DECL_ENUM_REPR_RUST;
            uint32_t attribute_index;
            for (attribute_index = 0u;
                    attribute_index < capture->item->attribute_count;
                    ++attribute_index) {
                const CmInternedString *attribute_metadata = cm_interner_get(
                    &state->hir->strings,
                    capture->item->attributes[attribute_index].metadata);
                const unsigned char *candidate_name = NULL;
                size_t candidate_length = 0u;
                if (cm_decl_diagnostic_item_name(attribute_metadata,
                        &candidate_name, &candidate_length)) {
                    diagnostic_name = candidate_name;
                    diagnostic_name_length = candidate_length;
                    rust_default = 1;
                }
            }
            wire->enum_repr_primitive = capture->enum_repr;
            if (rust_default
                && !cm_decl_copy_bytes(&wire->diagnostic_item,
                    diagnostic_name, diagnostic_name_length)) return 0;
            if (capture->lang_item_length != 0u) {
                wire->enum_flags = CM_HIR_DECL_ENUM_HAS_LANG_ITEM;
                if (!cm_decl_copy_bytes(&wire->enum_lang_item,
                        capture->lang_item, capture->lang_item_length))
                    return 0;
            }
            wire->variant_count =
                capture->item->data.enum_item.variant_count;
            wire->variants = (CmHirDeclarationVariant *)cm_alloc_zeroed(
                wire->variant_count, sizeof(*wire->variants));
            for (variant_index = 0u; variant_index < wire->variant_count;
                    ++variant_index) {
                const CmHirVariant *source =
                    &capture->item->data.enum_item.variants[variant_index];
                CmHirDeclarationVariant *variant =
                    &wire->variants[variant_index];
                variant->kind = source->form == CM_HIR_AGGREGATE_TUPLE
                    ? CM_HIR_DECL_VARIANT_TUPLE
                    : CM_HIR_DECL_VARIANT_UNIT;
                if (!cm_decl_enum_variant_source_ordinal(state, capture,
                        variant_index, &variant->source_ordinal)) return 0;
                if (rust_default) {
                    variant->discriminant_primitive =
                        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
                } else {
                    variant->discriminant_primitive =
                        CM_HIR_DECL_PRIMITIVE_ISIZE;
                    variant->discriminant_low =
                        source->discriminant.data.value.low_bits;
                    variant->discriminant_high =
                        source->discriminant.data.value.high_bits;
                }
                if (!cm_decl_copy_intern(&variant->name,
                        cm_interner_get(&state->hir->strings,
                            source->name))) return 0;
                if (generic_default) {
                    uint32_t field_index;
                    variant->flags = CM_HIR_DECL_VARIANT_HAS_LANG_ITEM;
                    if (!cm_decl_copy_enum_variant_lang(state, capture,
                            variant_index, &variant->lang_item)) return 0;
                    variant->field_count = source->field_count;
                    variant->fields = variant->field_count == 0u ? NULL
                        : (CmHirDeclarationVariantField *)cm_alloc_zeroed(
                            variant->field_count,
                            sizeof(*variant->fields));
                    for (field_index = 0u;
                            field_index < variant->field_count;
                            ++field_index)
                        variant->fields[field_index].source_ordinal =
                            field_index;
                }
            }
        }
    }
#define CM_DECL_FILL_OWNER(items_, count_, wire_, owner_tag_) do { \
    for (index = 0u; index < (count_); ++index) { \
        const CmDeclCaptureItem *capture_ = &(items_)[index]; \
        const CmHirItem *item_ = capture_->item; \
        const CmInternedString *item_name_ = cm_decl_item_name(state, item_); \
        uint32_t child_; \
        (wire_)[index].owner_module = capture_->owner_module; \
        (wire_)[index].source_ordinal = capture_->source_ordinal; \
        cm_decl_copy_intern(&(wire_)[index].name, item_name_); \
        (wire_)[index].generic_start = item_->generic_parameter_count == 0u \
            ? 0u : (uint32_t)(cursor + 1u); \
        (wire_)[index].generic_count = item_->generic_parameter_count; \
        for (child_ = 0u; child_ < item_->generic_parameter_count; ++child_) { \
            CmHirGenericParamId id_ = item_->generic_parameter_start + child_; \
            const CmHirGenericParam *generic_ = cm_hir_get_generic_param( \
                state->hir, id_); \
            const CmInternedString *name_ = cm_interner_get( \
                &state->hir->strings, generic_->name); \
            metadata->generics[cursor].owner_kind = (owner_tag_); \
            metadata->generics[cursor].owner_local = (uint32_t)(index + 1u); \
            metadata->generics[cursor].index = child_; \
            metadata->generics[cursor].kind = generic_->kind \
                    == CM_HIR_GENERIC_CONST \
                ? CM_HIR_DECL_GENERIC_CONST : CM_HIR_DECL_GENERIC_TYPE; \
            metadata->generics[cursor].is_relaxed_sized = \
                (uint8_t)generic_->is_relaxed_sized; \
            metadata->generics[cursor].has_default = \
                (uint8_t)generic_->has_default; \
            cm_decl_copy_intern(&metadata->generics[cursor].name, name_); \
            state->generic_locals[id_ - 1u] = (uint32_t)(cursor + 1u); \
            cursor += 1u; \
        } \
    } \
} while (0)
    CM_DECL_FILL_OWNER(state->traits, state->trait_count, metadata->traits,
        CM_HIR_DECL_GENERIC_NOMINAL);
    CM_DECL_FILL_OWNER(state->items, state->item_count, metadata->items,
        CM_HIR_DECL_GENERIC_ITEM);
    CM_DECL_FILL_OWNER(state->values, state->value_count, metadata->values,
        CM_HIR_DECL_GENERIC_VALUE);
#undef CM_DECL_FILL_OWNER
    for (index = 0u; index < state->trait_count; ++index) {
        const CmDeclCaptureItem *capture = &state->traits[index];
        if (!cm_decl_fill_visibility(state, capture->item->visibility,
                &metadata->traits[index].visibility)) return 0;
        metadata->traits[index].associated_start =
            capture->associated_start;
        metadata->traits[index].associated_count =
            capture->associated_count;
        metadata->traits[index].safety =
            (uint8_t)capture->item->data.trait_item.safety;
        metadata->traits[index].flags = capture->trait_flags;
        if ((capture->trait_flags & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u
            && !cm_decl_copy_bytes(&metadata->traits[index].lang_item,
                capture->lang_item, capture->lang_item_length)) return 0;
        if (capture->diagnostic_item_length != 0u) {
            metadata->traits[index].flags |=
                CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM;
            if (!cm_decl_copy_bytes(&metadata->traits[index].diagnostic_item,
                    capture->diagnostic_item,
                    capture->diagnostic_item_length)) return 0;
        }
        if (capture->has_static_outlives) {
            metadata->traits[index].outlives_start =
                (uint32_t)(outlives_cursor + 1u);
            metadata->traits[index].outlives_count = 1u;
            outlives_cursor += 1u;
        }
    }
    if (outlives_cursor != outlives_count) return 0;
    for (index = 0u; index < state->associated_count; ++index) {
        const CmDeclCaptureItem *capture = &state->associated_items[index];
        const CmHirItem *item = capture->item;
        CmHirDeclarationAssociatedItem *wire =
            &metadata->associated_items[index];
        wire->parent_kind = CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL;
        wire->parent_local = cm_decl_trait_local(state,
            item->parent_definition);
        wire->implemented_associated_local = 0u;
        wire->visibility.kind = CM_HIR_DECL_VISIBILITY_PRIVATE;
        wire->visibility.restriction_module = 0u;
        wire->source_ordinal = capture->source_ordinal;
        wire->is_specializable = (uint8_t)item->is_specializable;
        if (wire->parent_local == 0u
            || !cm_decl_copy_intern(&wire->name,
                cm_interner_get(&state->hir->strings, item->name))) return 0;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            wire->kind = CM_HIR_DECL_ASSOCIATED_TYPE;
            wire->flags = CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM;
            if (capture->lang_item_length == 0u
                || !cm_decl_copy_bytes(&wire->lang_item,
                    capture->lang_item, capture->lang_item_length)) return 0;
        } else if (item->kind == CM_HIR_ITEM_FUNCTION) {
            const CmHirFunctionSignature *signature =
                &item->data.function_item.signature;
            wire->kind = CM_HIR_DECL_ASSOCIATED_METHOD;
            wire->generic_start = 0u;
            wire->generic_count = 0u;
            wire->receiver = (uint8_t)signature->receiver;
            wire->parameter_count = signature->parameter_count;
            wire->return_type = 0u;
            wire->safety = (uint8_t)signature->safety;
            wire->is_const = (uint8_t)signature->is_const;
            wire->is_async = (uint8_t)signature->is_async;
            wire->is_variadic = (uint8_t)signature->is_variadic;
            wire->has_default_body =
                (uint8_t)item->data.function_item.has_default_body;
            if (!cm_decl_copy_intern(&wire->abi,
                    cm_interner_get(&state->hir->strings, signature->abi)))
                return 0;
        } else return 0;
    }
    return cursor == generic_count;
}

static uint8_t cm_decl_candidate_kind(const CmHirType *type)
{
    if (type == NULL) return 0u;
    switch (type->kind) {
    case CM_HIR_TYPE_SLICE_KIND: return CM_HIR_DECL_TYPE_SLICE;
    case CM_HIR_TYPE_RAW_POINTER_KIND: return CM_HIR_DECL_TYPE_RAW_POINTER;
    case CM_HIR_TYPE_REFERENCE_KIND: return CM_HIR_DECL_TYPE_REFERENCE;
    case CM_HIR_TYPE_ADT_KIND:
        return CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    case CM_HIR_TYPE_TUPLE_KIND: return CM_HIR_DECL_TYPE_TUPLE;
    case CM_HIR_TYPE_ARRAY_KIND: return CM_HIR_DECL_TYPE_ARRAY;
    case CM_HIR_TYPE_PROJECTION_KIND: return CM_HIR_DECL_TYPE_PROJECTION;
    default: return 0u;
    }
}

static int cm_decl_compare_u32(uint32_t left, uint32_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

static int cm_decl_compare_u64(uint64_t left, uint64_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

static int cm_decl_candidate_compare(const CmDeclCaptureState *state,
    const CmDeclTypeCandidate *left_candidate,
    const CmDeclTypeCandidate *right_candidate)
{
    const CmHirType *left = cm_hir_get_type(state->hir, left_candidate->id);
    const CmHirType *right = cm_hir_get_type(state->hir,
        right_candidate->id);
    uint32_t index;
    int order;
    if ((order = cm_decl_compare_u32(left_candidate->depth,
            right_candidate->depth)) != 0) return order;
    if ((order = cm_decl_compare_u32(left_candidate->kind,
            right_candidate->kind)) != 0) return order;
    if (left == NULL || right == NULL) return left == right ? 0
        : left == NULL ? -1 : 1;
    if (left_candidate->kind == CM_HIR_DECL_TYPE_SLICE) {
        return cm_decl_compare_u32(cm_decl_type_local(state, NULL,
            left->data.slice_type.element), cm_decl_type_local(state, NULL,
            right->data.slice_type.element));
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
        uint8_t left_mutability = left->data.raw_pointer_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
        uint8_t right_mutability = right->data.raw_pointer_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
        if ((order = cm_decl_compare_u32(left_mutability,
                right_mutability)) != 0) return order;
        return cm_decl_compare_u32(cm_decl_type_local(state, NULL,
            left->data.raw_pointer_type.pointee), cm_decl_type_local(state,
            NULL, right->data.raw_pointer_type.pointee));
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_REFERENCE) {
        uint8_t left_mutability = left->data.reference_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
        uint8_t right_mutability = right->data.reference_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
        if ((order = cm_decl_compare_u32(left->data.reference_type.region.kind,
                right->data.reference_type.region.kind)) != 0) return order;
        /* Admitted erased regions encode generic_local/binder_index as zero. */
        if ((order = cm_decl_compare_u32(left_mutability,
                right_mutability)) != 0) return order;
        return cm_decl_compare_u32(cm_decl_type_local(state, NULL,
            left->data.reference_type.pointee), cm_decl_type_local(state,
            NULL, right->data.reference_type.pointee));
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
        if ((order = cm_decl_compare_u32(cm_decl_item_local(state,
                left->data.named_type.definition), cm_decl_item_local(state,
                right->data.named_type.definition))) != 0) return order;
        if ((order = cm_decl_compare_u32(
                left->data.named_type.argument_count,
                right->data.named_type.argument_count)) != 0) return order;
        for (index = 0u; index < left->data.named_type.argument_count;
                ++index) {
            order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                    left->data.named_type.arguments[index].data.type),
                cm_decl_type_local(state, NULL,
                    right->data.named_type.arguments[index].data.type));
            if (order != 0) return order;
        }
        return 0;
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_TUPLE) {
        if ((order = cm_decl_compare_u32(left->data.tuple_type.element_count,
                right->data.tuple_type.element_count)) != 0) return order;
        for (index = 0u; index < left->data.tuple_type.element_count; ++index) {
            order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                    left->data.tuple_type.elements[index]),
                cm_decl_type_local(state, NULL,
                    right->data.tuple_type.elements[index]));
            if (order != 0) return order;
        }
        return 0;
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_ARRAY) {
        uint8_t left_kind = left->data.array_type.length.kind
                == CM_HIR_CONST_PARAMETER
            ? CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
            : CM_HIR_DECL_ARRAY_LENGTH_SCALAR;
        uint8_t right_kind = right->data.array_type.length.kind
                == CM_HIR_CONST_PARAMETER
            ? CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
            : CM_HIR_DECL_ARRAY_LENGTH_SCALAR;
        if ((order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                left->data.array_type.element), cm_decl_type_local(state,
                NULL, right->data.array_type.element))) != 0) return order;
        if ((order = cm_decl_compare_u32(left_kind, right_kind)) != 0)
            return order;
        if (left_kind == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER)
            return cm_decl_compare_u32(state->generic_locals[
                    left->data.array_type.length.data.parameter - 1u],
                state->generic_locals[
                    right->data.array_type.length.data.parameter - 1u]);
        if ((order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                left->data.array_type.length.type), cm_decl_type_local(state,
                NULL, right->data.array_type.length.type))) != 0) return order;
        if ((order = cm_decl_compare_u64(
                left->data.array_type.length.data.value.low_bits,
                right->data.array_type.length.data.value.low_bits)) != 0)
            return order;
        return cm_decl_compare_u64(
            left->data.array_type.length.data.value.high_bits,
            right->data.array_type.length.data.value.high_bits);
    }
    if (left_candidate->kind == CM_HIR_DECL_TYPE_PROJECTION) {
        const CmHirNamedType *left_trait =
            &left->data.projection_type.trait_type;
        const CmHirNamedType *right_trait =
            &right->data.projection_type.trait_type;
        if ((order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                left->data.projection_type.self_type),
                cm_decl_type_local(state, NULL,
                    right->data.projection_type.self_type))) != 0)
            return order;
        if ((order = cm_decl_compare_u32(cm_decl_trait_local(state,
                left_trait->definition), cm_decl_trait_local(state,
                    right_trait->definition))) != 0) return order;
        if ((order = cm_decl_compare_u32(cm_decl_associated_local(state,
                left->data.projection_type.associated_type.definition),
                cm_decl_associated_local(state,
                    right->data.projection_type.associated_type.definition)))
                != 0) return order;
        if ((order = cm_decl_compare_u32(left_trait->argument_count,
                right_trait->argument_count)) != 0) return order;
        for (index = 0u; index < left_trait->argument_count; ++index) {
            order = cm_decl_compare_u32(cm_decl_type_local(state, NULL,
                    left_trait->arguments[index].data.type),
                cm_decl_type_local(state, NULL,
                    right_trait->arguments[index].data.type));
            if (order != 0) return order;
        }
        return 0;
    }
    return 0;
}

static void cm_decl_merge_candidates(const CmDeclCaptureState *state,
    CmDeclTypeCandidate *values, CmDeclTypeCandidate *scratch,
    size_t begin, size_t middle, size_t end)
{
    size_t left = begin;
    size_t right = middle;
    size_t cursor = begin;
    while (left < middle && right < end) {
        if (cm_decl_candidate_compare(state, &values[left],
                &values[right]) <= 0)
            scratch[cursor++] = values[left++];
        else scratch[cursor++] = values[right++];
    }
    while (left < middle) scratch[cursor++] = values[left++];
    while (right < end) scratch[cursor++] = values[right++];
    memcpy(&values[begin], &scratch[begin],
        (end - begin) * sizeof(*values));
}

static void cm_decl_sort_candidates(const CmDeclCaptureState *state,
    CmDeclTypeCandidate *values, CmDeclTypeCandidate *scratch,
    size_t begin, size_t end)
{
    size_t middle;
    if (end - begin < 2u) return;
    middle = begin + (end - begin) / 2u;
    cm_decl_sort_candidates(state, values, scratch, begin, middle);
    cm_decl_sort_candidates(state, values, scratch, middle, end);
    cm_decl_merge_candidates(state, values, scratch, begin, middle, end);
}

static int cm_decl_assign_leaf_type_locals(CmDeclCaptureState *state)
{
    size_t index;
    for (index = 0u; index < state->hir->types.len; ++index) {
        const CmHirType *type;
        uint8_t primitive;
        uint32_t local = 0u;
        if (state->type_visits[index] != 2u
            || state->type_depths[index] != 0u) continue;
        type = cm_hir_get_type(state->hir, (CmHirTypeId)(index + 1u));
        primitive = cm_decl_primitive(type);
        if (primitive != 0u) local = state->primitive_type_locals[primitive];
        else if (type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND) {
            CmHirGenericParamId parameter =
                type->data.parameter_type.parameter;
            uint32_t generic_local;
            if (parameter == CM_HIR_GENERIC_PARAM_NONE
                || (size_t)parameter > state->hir->generic_parameters.len)
                return 0;
            generic_local = state->generic_locals[parameter - 1u];
            if (generic_local != 0u)
                local = state->generic_type_locals[generic_local - 1u];
        } else if (type != NULL && type->kind == CM_HIR_TYPE_SELF_KIND) {
            uint32_t trait_local = cm_decl_trait_local(state,
                type->data.self_type.owner);
            if (trait_local != 0u)
                local = state->self_type_locals[trait_local - 1u];
        } else if (type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND) {
            uint32_t item_local = cm_decl_item_local(state,
                type->data.named_type.definition);
            if (item_local != 0u)
                local = state->named_type_locals[item_local - 1u];
        }
        if (local == 0u) return 0;
        state->canonical_type_locals[index] = local;
    }
    return 1;
}

static int cm_decl_emit_candidate(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata, size_t *cursor,
    const CmDeclTypeCandidate *candidate, size_t *emitted_edges)
{
    const CmHirType *source = cm_hir_get_type(state->hir, candidate->id);
    CmHirDeclarationType *wire;
    uint32_t child;
    size_t edge_count;
    if (source == NULL || *cursor >= metadata->type_count) return 0;
    if (candidate->kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION)
        edge_count = source->data.named_type.argument_count;
    else if (candidate->kind == CM_HIR_DECL_TYPE_TUPLE)
        edge_count = source->data.tuple_type.element_count;
    else if (candidate->kind == CM_HIR_DECL_TYPE_ARRAY) edge_count = 2u;
    else if (candidate->kind == CM_HIR_DECL_TYPE_PROJECTION)
        edge_count = 1u
            + source->data.projection_type.trait_type.argument_count;
    else edge_count = 0u;
    if (edge_count > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES - *emitted_edges)
        return 0;
    *emitted_edges += edge_count;
    wire = &metadata->types[*cursor];
    wire->kind = candidate->kind;
    if (candidate->kind == CM_HIR_DECL_TYPE_SLICE) {
        wire->child_type = cm_decl_type_local(state, metadata,
            source->data.slice_type.element);
    } else if (candidate->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
        wire->child_type = cm_decl_type_local(state, metadata,
            source->data.raw_pointer_type.pointee);
        wire->mutability = source->data.raw_pointer_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
    } else if (candidate->kind == CM_HIR_DECL_TYPE_REFERENCE) {
        wire->child_type = cm_decl_type_local(state, metadata,
            source->data.reference_type.pointee);
        wire->mutability = source->data.reference_type.mutability
                == CM_HIR_MUTABLE
            ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE;
        wire->region.kind = source->data.reference_type.region.kind
                == CM_HIR_REGION_STATIC
            ? CM_HIR_DECL_REGION_STATIC : CM_HIR_DECL_REGION_ERASED;
    } else if (candidate->kind
            == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
        wire->item_local = cm_decl_item_local(state,
            source->data.named_type.definition);
        wire->argument_count = source->data.named_type.argument_count;
        wire->argument_types = (uint32_t *)cm_alloc_zeroed(
            wire->argument_count, sizeof(*wire->argument_types));
        for (child = 0u; child < wire->argument_count; ++child)
            wire->argument_types[child] = cm_decl_type_local(state, metadata,
                source->data.named_type.arguments[child].data.type);
    } else if (candidate->kind == CM_HIR_DECL_TYPE_TUPLE) {
        wire->element_count = source->data.tuple_type.element_count;
        wire->element_types = (uint32_t *)cm_alloc_zeroed(
            wire->element_count, sizeof(*wire->element_types));
        for (child = 0u; child < wire->element_count; ++child)
            wire->element_types[child] = cm_decl_type_local(state, metadata,
                source->data.tuple_type.elements[child]);
    } else if (candidate->kind == CM_HIR_DECL_TYPE_ARRAY) {
        wire->child_type = cm_decl_type_local(state, metadata,
            source->data.array_type.element);
        if (source->data.array_type.length.kind == CM_HIR_CONST_PARAMETER) {
            wire->array_length_kind =
                CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER;
            wire->array_length_generic_local = state->generic_locals[
                source->data.array_type.length.data.parameter - 1u];
        } else {
            wire->array_length_kind = CM_HIR_DECL_ARRAY_LENGTH_SCALAR;
            wire->array_length_type = cm_decl_type_local(state, metadata,
                source->data.array_type.length.type);
            wire->array_length_low_bits =
                source->data.array_type.length.data.value.low_bits;
            wire->array_length_high_bits =
                source->data.array_type.length.data.value.high_bits;
        }
    } else if (candidate->kind == CM_HIR_DECL_TYPE_PROJECTION) {
        const CmHirNamedType *trait_type =
            &source->data.projection_type.trait_type;
        wire->projection_self_type = cm_decl_type_local(state, metadata,
            source->data.projection_type.self_type);
        wire->projection_trait_local = cm_decl_trait_local(state,
            trait_type->definition);
        wire->projection_associated_local = cm_decl_associated_local(state,
            source->data.projection_type.associated_type.definition);
        wire->projection_argument_count = trait_type->argument_count;
        wire->projection_argument_types = trait_type->argument_count == 0u
            ? NULL : (uint32_t *)cm_alloc_zeroed(trait_type->argument_count,
                sizeof(*wire->projection_argument_types));
        for (child = 0u; child < trait_type->argument_count; ++child)
            wire->projection_argument_types[child] = cm_decl_type_local(state,
                metadata, trait_type->arguments[child].data.type);
    } else return 0;
    state->canonical_type_locals[candidate->id - 1u] =
        (uint32_t)(*cursor + 1u);
    *cursor += 1u;
    return 1;
}

static int cm_decl_fill_compound_types(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata, size_t *cursor, size_t pending)
{
    CmDeclTypeCandidate *candidates;
    CmDeclTypeCandidate *scratch;
    size_t depth_counts[CM_META_MAX_TYPE_NESTING + 1u];
    size_t depth_offsets[CM_META_MAX_TYPE_NESTING + 2u];
    size_t depth_cursors[CM_META_MAX_TYPE_NESTING + 1u];
    size_t emitted_edges = 0u;
    size_t index;
    uint32_t depth;
    memset(depth_counts, 0, sizeof(depth_counts));
    memset(depth_offsets, 0, sizeof(depth_offsets));
    if (pending == 0u) return 1;
    candidates = (CmDeclTypeCandidate *)cm_alloc_zeroed(pending,
        sizeof(*candidates));
    scratch = (CmDeclTypeCandidate *)cm_alloc_zeroed(pending,
        sizeof(*scratch));
    for (index = 0u; index < state->hir->types.len; ++index) {
        if (state->application_types[index] == 0u
            && state->compound_types[index] == 0u) continue;
        depth = state->type_depths[index];
        if (state->type_visits[index] != 2u || depth == 0u
            || depth > CM_META_MAX_TYPE_NESTING) goto fail;
        depth_counts[depth] += 1u;
    }
    for (depth = 1u; depth <= CM_META_MAX_TYPE_NESTING; ++depth)
        depth_offsets[depth + 1u] = depth_offsets[depth] + depth_counts[depth];
    if (depth_offsets[CM_META_MAX_TYPE_NESTING + 1u] != pending) goto fail;
    memcpy(depth_cursors, depth_offsets, sizeof(depth_cursors));
    for (index = 0u; index < state->hir->types.len; ++index) {
        CmDeclTypeCandidate *candidate;
        if (state->application_types[index] == 0u
            && state->compound_types[index] == 0u) continue;
        depth = state->type_depths[index];
        candidate = &candidates[depth_cursors[depth]++];
        candidate->id = (CmHirTypeId)(index + 1u);
        candidate->depth = depth;
        candidate->kind = cm_decl_candidate_kind(cm_hir_get_type(state->hir,
            candidate->id));
        if (candidate->kind == 0u) goto fail;
    }
    for (depth = 1u; depth <= CM_META_MAX_TYPE_NESTING; ++depth) {
        size_t begin = depth_offsets[depth];
        size_t end = depth_offsets[depth + 1u];
        uint32_t previous_local = 0u;
        cm_decl_sort_candidates(state, candidates, scratch, begin, end);
        for (index = begin; index < end; ++index) {
            if (index != begin && cm_decl_candidate_compare(state,
                    &candidates[index - 1u], &candidates[index]) == 0) {
                if (previous_local == 0u) goto fail;
                state->canonical_type_locals[candidates[index].id - 1u] =
                    previous_local;
            } else {
                if (!cm_decl_emit_candidate(state, metadata, cursor,
                        &candidates[index], &emitted_edges)) goto fail;
                previous_local = state->canonical_type_locals[
                    candidates[index].id - 1u];
            }
        }
    }
    cm_free(scratch);
    cm_free(candidates);
    return 1;
fail:
    cm_free(scratch);
    cm_free(candidates);
    return 0;
}

static int cm_decl_fill_types_values_predicates(CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata,
    CmHirDeclarationCaptureResult *result)
{
    size_t index;
    size_t predicate_count = 0u;
    size_t type_count = 0u;
    size_t application_count = 0u;
    size_t compound_count = 0u;
    size_t composite_occurrence_count;
    size_t type_capacity;
    size_t cursor;
    state->generic_types = (unsigned char *)cm_alloc_zeroed(
        metadata->generic_count, sizeof(*state->generic_types));
    state->self_types = (unsigned char *)cm_alloc_zeroed(
        metadata->trait_count, sizeof(*state->self_types));
    state->named_item_types = (unsigned char *)cm_alloc_zeroed(
        metadata->item_count, sizeof(*state->named_item_types));
    state->application_types = (unsigned char *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->application_types));
    state->compound_types = (unsigned char *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->compound_types));
    state->type_visits = (unsigned char *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->type_visits));
    state->type_depths = (uint32_t *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->type_depths));
    state->canonical_type_locals = (uint32_t *)cm_alloc_zeroed(
        state->hir->types.len, sizeof(*state->canonical_type_locals));
    state->generic_type_locals = (uint32_t *)cm_alloc_zeroed(
        metadata->generic_count, sizeof(*state->generic_type_locals));
    state->named_type_locals = (uint32_t *)cm_alloc_zeroed(
        metadata->item_count, sizeof(*state->named_type_locals));
    state->self_type_locals = (uint32_t *)cm_alloc_zeroed(
        metadata->trait_count, sizeof(*state->self_type_locals));
    for (index = 0u; index < state->hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, (CmHirGenericParamId)(index + 1u));
        if (state->generic_locals[index] == 0u || generic == NULL
            || generic->kind != CM_HIR_GENERIC_CONST) continue;
        if (!cm_decl_mark_type(state, generic->declared_type, result))
            return 0;
    }
    for (index = 0u; index < state->trait_count; ++index) {
        const CmDeclCaptureItem *capture = &state->traits[index];
        const CmHirItem *item = capture->item;
        uint32_t child;
        if (capture->has_static_outlives) {
            if (capture->item->outlives_predicate_count != 1u
                || capture->item->outlives_predicates == NULL
                || !cm_decl_mark_type(state,
                    capture->item->outlives_predicates[0].subject.type,
                    result)) return 0;
        }
        if (predicate_count > SIZE_MAX - item->predicate_count) return 0;
        predicate_count += item->predicate_count;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->equality_count != 0u
                || predicate->equalities != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || predicate->trait_type.argument_count != 0u
                || predicate->trait_type.arguments != NULL
                || cm_decl_trait_local(state,
                    predicate->trait_type.definition) == 0u
                || !cm_decl_mark_type(state, predicate->subject, result))
                return 0;
        }
        for (child = 0u;
                child < item->data.trait_item.supertrait_count; ++child) {
            const CmHirSupertrait *supertrait =
                &item->data.trait_item.supertraits[child];
            uint32_t argument;
            for (argument = 0u;
                    argument < supertrait->trait_type.argument_count;
                    ++argument) {
                if (supertrait->trait_type.arguments[argument].kind
                        != CM_HIR_GENERIC_ARG_TYPE
                    || !cm_decl_mark_type(state, supertrait->trait_type
                        .arguments[argument].data.type, result)) return 0;
            }
        }
    }
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        uint32_t child;
        if ((item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION)
            && item->data.aggregate_item.form != CM_HIR_AGGREGATE_UNIT) {
            for (child = 0u; child < item->data.aggregate_item.field_count;
                    ++child) {
                if (!cm_decl_mark_type(state,
                        item->data.aggregate_item.fields[child].type, result))
                    return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child)
                    if (!cm_decl_mark_type(state, variant->fields[child].type,
                            result)) return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && !cm_decl_mark_named_adt(state,
                item->data.type_alias_item.target, result)) return 0;
        if (predicate_count > SIZE_MAX - item->predicate_count) return 0;
        predicate_count += item->predicate_count;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->equality_count != 0u
                || predicate->equalities != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || predicate->trait_type.argument_count != 0u
                || predicate->trait_type.arguments != NULL
                || cm_decl_trait_local(state,
                    predicate->trait_type.definition) == 0u
                || !cm_decl_mark_type(state, predicate->subject, result))
                return 0;
        }
    }
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        uint32_t child;
        if (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC) {
            if (!cm_decl_mark_type(state, item->data.value_item.type, result))
                return 0;
            continue;
        }
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        {
            const CmHirFunctionSignature *signature =
                &item->data.function_item.signature;
            predicate_count += item->predicate_count;
            for (child = 0u; child < signature->parameter_count; ++child)
                if (!cm_decl_mark_type(state,
                        signature->parameters[child].type, result)) return 0;
            if (!cm_decl_mark_type(state, signature->return_type, result))
                return 0;
        }
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            uint32_t argument;
            if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || predicate->binder.lifetime_count != 0u
                || predicate->binder.lifetimes != NULL
                || predicate->modifier != CM_HIR_PREDICATE_REQUIRED
                || cm_decl_trait_local(state,
                    predicate->trait_type.definition) == 0u
                || !cm_decl_mark_type(state, predicate->subject, result))
                return 0;
            for (argument = 0u;
                    argument < predicate->trait_type.argument_count;
                    ++argument) {
                if (predicate->trait_type.arguments[argument].kind
                        != CM_HIR_GENERIC_ARG_TYPE
                    || !cm_decl_mark_type(state,
                        predicate->trait_type.arguments[argument].data.type,
                        result)) return 0;
            }
            for (argument = 0u; argument < predicate->equality_count;
                    ++argument) {
                if (predicate->equalities == NULL
                    || cm_decl_associated_local(state,
                        predicate->equalities[argument].associated_type) == 0u
                    || !cm_decl_mark_type(state,
                        predicate->equalities[argument].value, result))
                    return 0;
            }
        }
    }
    for (index = 0u; index < state->associated_count; ++index) {
        const CmHirItem *item = state->associated_items[index].item;
        const CmHirFunctionSignature *signature;
        uint32_t child;
        if (item == NULL) return 0;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) continue;
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        signature = &item->data.function_item.signature;
        if (predicate_count > SIZE_MAX - item->predicate_count) return 0;
        predicate_count += item->predicate_count;
        for (child = 0u; child < signature->parameter_count; ++child)
            if (!cm_decl_mark_type(state,
                    signature->parameters[child].type, result)) return 0;
        if (!cm_decl_mark_type(state, signature->return_type, result)) return 0;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            if (!cm_decl_mark_type(state, predicate->subject, result)) return 0;
        }
    }
    if (predicate_count > CM_HIR_DECL_METADATA_MAX_RECORDS) return 0;
    for (index = CM_HIR_DECL_PRIMITIVE_UNIT;
            index <= CM_HIR_DECL_PRIMITIVE_F64; ++index)
        if (state->primitive_types[index]) type_count += 1u;
    for (index = 0u; index < metadata->generic_count; ++index)
        if (state->generic_types[index]) type_count += 1u;
    for (index = 0u; index < metadata->item_count; ++index)
        if (state->named_item_types[index]) type_count += 1u;
    for (index = 0u; index < metadata->trait_count; ++index)
        if (state->self_types[index]) type_count += 1u;
    for (index = 0u; index < state->hir->types.len; ++index)
        if (state->application_types[index]) {
            application_count += 1u;
        }
    for (index = 0u; index < state->hir->types.len; ++index)
        if (state->compound_types[index]) {
            compound_count += 1u;
        }
    if (type_count > CM_HIR_DECL_METADATA_MAX_TYPES) return 0;
    if (application_count > SIZE_MAX - compound_count) return 0;
    composite_occurrence_count = application_count + compound_count;
    type_capacity = CM_HIR_DECL_METADATA_MAX_TYPES - type_count;
    if (composite_occurrence_count < type_capacity)
        type_capacity = composite_occurrence_count;
    type_capacity += type_count;
    metadata->type_count = type_capacity;
    metadata->types = (CmHirDeclarationType *)cm_alloc_zeroed(type_capacity,
        sizeof(*metadata->types));
    cursor = 0u;
    for (index = CM_HIR_DECL_PRIMITIVE_UNIT;
            index <= CM_HIR_DECL_PRIMITIVE_F64; ++index) {
        if (state->primitive_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
            metadata->types[cursor].primitive = (uint8_t)index;
            state->primitive_type_locals[index] = (uint32_t)(cursor + 1u);
            cursor += 1u;
        }
    }
    for (index = 0u; index < metadata->generic_count; ++index) {
        if (state->generic_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_GENERIC;
            metadata->types[cursor].generic_local = (uint32_t)(index + 1u);
            state->generic_type_locals[index] = (uint32_t)(cursor + 1u);
            cursor += 1u;
        }
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        if (state->named_item_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
            metadata->types[cursor].item_local = (uint32_t)(index + 1u);
            state->named_type_locals[index] = (uint32_t)(cursor + 1u);
            cursor += 1u;
        }
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        if (state->self_types[index]) {
            metadata->types[cursor].kind = CM_HIR_DECL_TYPE_SELF;
            metadata->types[cursor].self_trait_local =
                (uint32_t)(index + 1u);
            state->self_type_locals[index] = (uint32_t)(cursor + 1u);
            cursor += 1u;
        }
    }
    if (!cm_decl_assign_leaf_type_locals(state)) return 0;
    if (!cm_decl_fill_compound_types(state, metadata, &cursor,
            application_count + compound_count)) return 0;
    /* The provisional count is by live HIR ID. Structural duplicates consume
     * pending IDs but intentionally share the already emitted canonical local. */
    if (cursor > type_capacity) return 0;
    metadata->type_count = cursor;
    for (index = 0u; index < state->hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *generic = cm_hir_get_generic_param(
            state->hir, (CmHirGenericParamId)(index + 1u));
        uint32_t local = state->generic_locals[index];
        if (local == 0u || generic == NULL
            || generic->kind != CM_HIR_GENERIC_CONST) continue;
        metadata->generics[local - 1u].declared_type =
            cm_decl_type_local(state, metadata, generic->declared_type);
        if (metadata->generics[local - 1u].declared_type == 0u) return 0;
    }
    for (index = 0u; index < state->trait_count; ++index) {
        const CmHirItem *item = state->traits[index].item;
        CmHirDeclarationTrait *wire = &metadata->traits[index];
        uint32_t child;
        wire->supertrait_count = item->data.trait_item.supertrait_count;
        wire->supertraits = wire->supertrait_count == 0u ? NULL
            : (CmHirDeclarationSupertrait *)cm_alloc_zeroed(
                wire->supertrait_count, sizeof(*wire->supertraits));
        for (child = 0u; child < wire->supertrait_count; ++child) {
            const CmHirSupertrait *source =
                &item->data.trait_item.supertraits[child];
            CmHirDeclarationSupertrait *target = &wire->supertraits[child];
            uint32_t argument;
            target->modifier = CM_HIR_DECL_SUPERTRAIT_REQUIRED;
            target->trait_local = cm_decl_trait_local(state,
                source->trait_type.definition);
            target->argument_count = source->trait_type.argument_count;
            target->argument_types = target->argument_count == 0u ? NULL
                : (uint32_t *)cm_alloc_zeroed(target->argument_count,
                    sizeof(*target->argument_types));
            if (source->modifier != CM_HIR_SUPERTRAIT_REQUIRED
                || target->trait_local == 0u) return 0;
            for (argument = 0u; argument < target->argument_count;
                    ++argument) {
                target->argument_types[argument] = cm_decl_type_local(state,
                    metadata, source->trait_type.arguments[argument]
                        .data.type);
                if (target->argument_types[argument] == 0u) return 0;
            }
        }
    }
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        uint32_t child;
        if ((item->kind == CM_HIR_ITEM_STRUCT
                || item->kind == CM_HIR_ITEM_UNION)
            && item->data.aggregate_item.form != CM_HIR_AGGREGATE_UNIT) {
            for (child = 0u; child < item->data.aggregate_item.field_count;
                    ++child) {
                metadata->items[index].fields[child].type_local =
                    cm_decl_type_local(state, metadata,
                        item->data.aggregate_item.fields[child].type);
                if (metadata->items[index].fields[child].type_local == 0u)
                    return 0;
            }
        } else if (item->kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->data.enum_item.variant_count;
                    ++variant_index) {
                const CmHirVariant *variant =
                    &item->data.enum_item.variants[variant_index];
                CmHirDeclarationVariant *wire_variant =
                    &metadata->items[index].variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child) {
                    wire_variant->fields[child].type_local =
                        cm_decl_type_local(state, metadata,
                            variant->fields[child].type);
                    if (wire_variant->fields[child].type_local == 0u)
                        return 0;
                }
            }
        } else if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            metadata->items[index].alias_target_type = cm_decl_type_local(
                state, metadata, item->data.type_alias_item.target);
            if (metadata->items[index].alias_target_type == 0u) return 0;
        }
    }
    {
        size_t outlives_cursor = 0u;
        for (index = 0u; index < state->trait_count; ++index) {
            const CmDeclCaptureItem *capture = &state->traits[index];
            const CmHirOutlivesPredicate *source;
            CmHirDeclarationOutlivesPredicate *wire;
            if (!capture->has_static_outlives) continue;
            if (capture->item->outlives_predicate_count != 1u
                || capture->item->outlives_predicates == NULL
                || outlives_cursor >= metadata->outlives_predicate_count)
                return 0;
            source = &capture->item->outlives_predicates[0];
            wire = &metadata->outlives_predicates[outlives_cursor];
            wire->owner_kind = CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
            wire->owner_local = (uint32_t)(index + 1u);
            wire->ordinal = 0u;
            wire->subject_type = cm_decl_type_local(state, metadata,
                source->subject.type);
            wire->bound.kind = CM_HIR_DECL_REGION_STATIC;
            wire->scope = 0u;
            if (wire->subject_type == 0u) return 0;
            outlives_cursor += 1u;
        }
        if (outlives_cursor != metadata->outlives_predicate_count) return 0;
    }
    metadata->predicate_count = predicate_count;
    metadata->predicates = (CmHirDeclarationPredicate *)cm_alloc_zeroed(
        predicate_count, sizeof(*metadata->predicates));
    cursor = 0u;
    for (index = 0u; index < state->value_count; ++index) {
        const CmHirItem *item = state->values[index].item;
        CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        if (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC) {
            value->kind = item->kind == CM_HIR_ITEM_CONST
                ? CM_HIR_DECL_VALUE_CONST : CM_HIR_DECL_VALUE_STATIC;
            value->declared_type = cm_decl_type_local(state, metadata,
                item->data.value_item.type);
            value->mutability = item->data.value_item.mutability
                == CM_HIR_MUTABLE ? CM_HIR_DECL_MUTABLE
                : CM_HIR_DECL_IMMUTABLE;
            value->has_body = UINT8_C(1);
            if (value->declared_type == 0u) return 0;
            continue;
        }
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        value->kind = CM_HIR_DECL_VALUE_FUNCTION;
        value->predicate_start = item->predicate_count == 0u
            ? 0u : (uint32_t)(cursor + 1u);
        value->predicate_count = item->predicate_count;
        value->parameter_count = item->data.function_item.signature
            .parameter_count;
        value->parameter_types = value->parameter_count == 0u ? NULL
            : (uint32_t *)cm_alloc_zeroed(value->parameter_count,
                sizeof(*value->parameter_types));
        for (child = 0u; child < value->parameter_count; ++child)
            value->parameter_types[child] = cm_decl_type_local(state,
                metadata,
                item->data.function_item.signature.parameters[child].type);
        value->return_type = cm_decl_type_local(state, metadata,
            item->data.function_item.signature.return_type);
        value->has_body = item->data.function_item.body == CM_HIR_BODY_NONE
            ? UINT8_C(0) : UINT8_C(1);
        value->is_const = (uint8_t)item->data.function_item.signature.is_const;
        if (value->return_type == 0u) return 0;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            CmHirDeclarationPredicate *wire = &metadata->predicates[cursor];
            uint32_t argument;
            wire->owner_value = (uint32_t)(index + 1u);
            wire->ordinal = child;
            wire->subject_type = cm_decl_type_local(state, metadata,
                predicate->subject);
            wire->trait_local = cm_decl_trait_local(state,
                predicate->trait_type.definition);
            wire->argument_count = predicate->trait_type.argument_count;
            wire->argument_types = wire->argument_count == 0u ? NULL
                : (uint32_t *)cm_alloc_zeroed(wire->argument_count,
                    sizeof(*wire->argument_types));
            for (argument = 0u; argument < wire->argument_count; ++argument)
                wire->argument_types[argument] = cm_decl_type_local(state,
                    metadata,
                    predicate->trait_type.arguments[argument].data.type);
            wire->equality_count = predicate->equality_count;
            wire->equalities = wire->equality_count == 0u ? NULL
                : (CmHirDeclarationPredicateEquality *)cm_alloc_zeroed(
                    wire->equality_count, sizeof(*wire->equalities));
            for (argument = 0u; argument < wire->equality_count;
                    ++argument) {
                wire->equalities[argument].associated_local =
                    cm_decl_associated_local(state,
                        predicate->equalities[argument].associated_type);
                wire->equalities[argument].value_type = cm_decl_type_local(
                    state, metadata, predicate->equalities[argument].value);
                if (wire->equalities[argument].associated_local == 0u
                    || wire->equalities[argument].value_type == 0u) return 0;
            }
            if (wire->subject_type == 0u || wire->trait_local == 0u)
                return 0;
            cursor += 1u;
        }
    }
    for (index = 0u; index < state->associated_count; ++index) {
        const CmHirItem *item = state->associated_items[index].item;
        CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) continue;
        if (item->kind != CM_HIR_ITEM_FUNCTION) return 0;
        {
        const CmHirFunctionSignature *signature =
            &item->data.function_item.signature;
        associated->predicate_start = item->predicate_count == 0u ? 0u
            : (uint32_t)(cursor + 1u);
        associated->predicate_count = item->predicate_count;
        associated->parameter_types = associated->parameter_count == 0u
            ? NULL : (uint32_t *)cm_alloc_zeroed(
                associated->parameter_count,
                sizeof(*associated->parameter_types));
        for (child = 0u; child < associated->parameter_count; ++child) {
            associated->parameter_types[child] = cm_decl_type_local(state,
                metadata, signature->parameters[child].type);
            if (associated->parameter_types[child] == 0u) return 0;
        }
        associated->return_type = cm_decl_type_local(state, metadata,
            signature->return_type);
        if (associated->return_type == 0u) return 0;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            CmHirDeclarationPredicate *wire = &metadata->predicates[cursor];
            wire->owner_kind = CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED;
            wire->owner_value = 0u;
            wire->owner_associated = (uint32_t)(index + 1u);
            wire->ordinal = child;
            wire->subject_type = cm_decl_type_local(state, metadata,
                predicate->subject);
            wire->trait_local = cm_decl_trait_local(state,
                predicate->trait_type.definition);
            wire->argument_count = 0u;
            wire->argument_types = NULL;
            if (wire->subject_type == 0u || wire->trait_local == 0u)
                return 0;
            cursor += 1u;
        }
        }
    }
    for (index = 0u; index < state->trait_count; ++index) {
        const CmHirItem *item = state->traits[index].item;
        CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t child;
        trait_value->predicate_start = item->predicate_count == 0u ? 0u
            : (uint32_t)(cursor + 1u);
        trait_value->predicate_count = item->predicate_count;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            CmHirDeclarationPredicate *wire = &metadata->predicates[cursor];
            wire->owner_kind = CM_HIR_DECL_PREDICATE_OWNER_NOMINAL;
            wire->owner_nominal = (uint32_t)(index + 1u);
            wire->ordinal = child;
            wire->subject_type = cm_decl_type_local(state, metadata,
                predicate->subject);
            wire->trait_local = cm_decl_trait_local(state,
                predicate->trait_type.definition);
            if (wire->subject_type == 0u || wire->trait_local == 0u)
                return 0;
            cursor += 1u;
        }
    }
    for (index = 0u; index < state->item_count; ++index) {
        const CmHirItem *item = state->items[index].item;
        CmHirDeclarationItem *wire_item = &metadata->items[index];
        uint32_t child;
        wire_item->predicate_scope_start = 0u;
        wire_item->predicate_scope_count = 0u;
        wire_item->outlives_start = 0u;
        wire_item->outlives_count = 0u;
        wire_item->predicate_start = item->predicate_count == 0u ? 0u
            : (uint32_t)(cursor + 1u);
        wire_item->predicate_count = item->predicate_count;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirTraitPredicate *predicate = &item->predicates[child];
            CmHirDeclarationPredicate *wire = &metadata->predicates[cursor];
            wire->owner_kind = CM_HIR_DECL_PREDICATE_OWNER_ITEM;
            wire->owner_value = 0u;
            wire->owner_associated = 0u;
            wire->owner_item = (uint32_t)(index + 1u);
            wire->ordinal = child;
            wire->subject_type = cm_decl_type_local(state, metadata,
                predicate->subject);
            wire->trait_local = cm_decl_trait_local(state,
                predicate->trait_type.definition);
            wire->argument_count = 0u;
            wire->argument_types = NULL;
            if (wire->subject_type == 0u || wire->trait_local == 0u)
                return 0;
            cursor += 1u;
        }
    }
    return cursor == predicate_count;
}

static int cm_decl_fill_namespace(const CmDeclCaptureState *state,
    CmHirDeclarationMetadata *metadata)
{
    size_t index;
    metadata->namespace_count = state->namespace_count;
    metadata->namespace_entries = (CmHirDeclarationNamespaceEntry *)
        cm_alloc_zeroed(state->namespace_count,
            sizeof(*metadata->namespace_entries));
    for (index = 0u; index < state->namespace_count; ++index) {
        const CmDeclCaptureNamespace *source =
            &state->namespace_values[index];
        CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        entry->owner_module = source->owner_module;
        entry->namespace_kind = source->namespace_kind;
        cm_decl_copy_bytes(&entry->name, source->name, source->name_length);
        entry->export_ordinal = source->export_ordinal;
        if (source->target.kind == CM_HIR_LIBRARY_BINDING_MODULE) {
            CmDeclCaptureModule *module = cm_decl_module_by_definition(
                (CmDeclCaptureState *)state, source->target.definition);
            if (module == NULL) return 0;
            entry->target_kind = CM_HIR_DECL_TARGET_MODULE;
            entry->target_local = module->local;
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_TRAIT) {
            entry->target_kind = CM_HIR_DECL_TARGET_NOMINAL;
            entry->target_local = cm_decl_trait_local(state,
                source->target.definition);
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_VALUE) {
            entry->target_kind = CM_HIR_DECL_TARGET_VALUE;
            entry->target_local = cm_decl_value_local(state,
                source->target.definition);
        } else if (source->target.kind
                == CM_HIR_LIBRARY_BINDING_PRIMITIVE) {
            entry->target_kind = CM_HIR_DECL_TARGET_PRIMITIVE;
            entry->target_local = cm_decl_library_primitive(
                source->target.primitive_kind);
        } else if (source->target.kind
                == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT) {
            entry->target_kind = CM_HIR_DECL_TARGET_ENUM_VARIANT;
            entry->target_local = cm_decl_enum_variant_local(state,
                &source->target);
        } else if (source->target.kind == CM_HIR_LIBRARY_BINDING_TYPE
            || source->target.kind
                == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR) {
            entry->target_kind = CM_HIR_DECL_TARGET_ITEM;
            entry->target_local = cm_decl_item_local(state,
                source->target.definition);
        } else return 0;
        if (entry->target_local == 0u) return 0;
    }
    return 1;
}

static void cm_decl_state_destroy(CmDeclCaptureState *state)
{
    size_t index;
    for (index = 0u; index < state->module_count; ++index)
        cm_free(state->modules[index].path);
    for (index = 0u; index < state->namespace_capacity; ++index)
        cm_free(state->namespace_values[index].name);
    cm_free(state->modules);
    cm_free(state->namespace_values);
    cm_free(state->traits);
    cm_free(state->associated_items);
    cm_free(state->items);
    cm_free(state->values);
    cm_free(state->generic_locals);
    cm_free(state->self_types);
    cm_free(state->generic_types);
    cm_free(state->named_item_types);
    cm_free(state->application_types);
    cm_free(state->compound_types);
    cm_free(state->type_visits);
    cm_free(state->type_depths);
    cm_free(state->canonical_type_locals);
    cm_free(state->generic_type_locals);
    cm_free(state->named_type_locals);
    cm_free(state->self_type_locals);
    cm_free(state->item_locals_by_hir_id);
    cm_free(state->trait_local_pairs);
}

CmHirDeclarationCaptureResult cm_hir_declaration_metadata_capture(
    const CmHirDeclarationCaptureInput *input,
    CmHirDeclarationMetadata *output)
{
    CmHirDeclarationCaptureResult result = cm_decl_capture_result(
        CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT);
    CmDeclCaptureState state;
    CmHirLibraryArtifact library;
    CmHirLibraryArtifactResult library_result;
    const CmHirLibraryOwnedData *owned;
    CmHirDeclarationMetadata candidate;
    CmHirDeclarationMetadata old;
    uint64_t graph_lifetime;
    uint64_t resolver_lifetime;
    uint64_t resolver_generation;
    uint64_t storage_lifetime;
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_INPUT;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_INVALID_ARGUMENT;
    if (input == NULL || output == NULL || input->hir == NULL
        || input->graph == NULL || input->imports == NULL
        || input->modules == NULL || input->configuration == NULL
        || input->crate_id == CM_HIR_CRATE_NONE
        || input->revision == CM_MODULE_GRAPH_REVISION_NONE
        || input->crate_disambiguator.data == NULL
        || input->crate_disambiguator.length == 0u
        || input->target_triple.data == NULL
        || input->target_triple.length == 0u
        || input->data_layout.data == NULL
        || input->data_layout.length == 0u
        || !cm_decl_target_configuration_matches(input)) return result;
    if (cm_module_graph_revision(input->graph) != input->revision
        || cm_module_graph_error_count(input->graph) != 0u
        || cm_import_resolver_revision(input->imports) != input->revision
        || cm_import_error_count(input->imports) != 0u
        || !cm_import_resolver_matches_graph(input->imports, input->graph)) {
        result.status = CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY;
        result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY;
        result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH;
        return result;
    }
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NONE;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_NONE;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.hir = input->hir;
    state.crate_value = cm_hir_get_crate(input->hir, input->crate_id);
    if (state.crate_value == NULL) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_INPUT,
            CM_HIR_DECL_CAPTURE_REASON_CRATE_NOT_FOUND);
        return result;
    }
    if (input->hir->items.len
            > CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS) {
        result.status = CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR;
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED);
        return result;
    }
    graph_lifetime = cm_module_graph_lifetime_id(input->graph);
    resolver_lifetime = cm_import_resolver_lifetime_id(input->imports);
    resolver_generation = cm_import_resolver_generation(input->imports);
    storage_lifetime = input->hir->storage.lifetime_id;
    semantic_generation = input->hir->semantic_generation;
    rewind_generation = input->hir->rewind_generation;
    cm_hir_library_artifact_init(&library);
    library_result = cm_hir_library_declaration_artifact_build(&library,
        input->hir, input->crate_id, input->graph, input->revision,
        input->modules, "capture");
    if (library_result.status != CM_HIR_LIBRARY_OK) {
        result.status = CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE;
        result.library_status = library_result.status;
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_LIBRARY,
            CM_HIR_DECL_CAPTURE_REASON_LIBRARY_REJECTED);
        cm_hir_library_artifact_destroy(&library);
        return result;
    }
    owned = cm_hir_library_artifact_owned_data_const(&library);
    cm_hir_declaration_metadata_init(&candidate);
    candidate.owns_storage = 1;
    result.status = CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR;
    if (owned == NULL) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_LIBRARY,
            CM_HIR_DECL_CAPTURE_REASON_OWNED_DATA_MISSING);
        goto done;
    }
    state.owned = owned;
    if (!cm_decl_collect_modules(&state, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_MODULES,
            CM_HIR_DECL_CAPTURE_REASON_MODULE_CENSUS_INVALID);
        goto done;
    }
    if (!cm_decl_collect_namespace(&state, owned, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE,
            CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH);
        goto done;
    }
    if (!cm_decl_collect_items(&state, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_ITEMS,
            CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_identity(&state, &candidate)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_IDENTITY,
            CM_HIR_DECL_CAPTURE_REASON_IDENTITY_UNSUPPORTED);
        goto done;
    }
    if (!cm_decl_fill_modules(&state, &candidate)) {
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_MODULE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_MODULE_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_items_and_generics(&state, &candidate)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_ITEM_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_types_values_predicates(&state, &candidate, &result)) {
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID);
        goto done;
    }
    if (!cm_decl_fill_namespace(&state, &candidate)) {
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE_METADATA,
            CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_TARGET_UNMAPPED);
        goto done;
    }
    result.metadata_status = cm_hir_declaration_metadata_validate(&candidate);
    if (result.metadata_status != CM_HIR_DECL_METADATA_OK) {
        result.status = CM_HIR_DECL_CAPTURE_METADATA_FAILURE;
        cm_decl_capture_fail(&result, CM_HIR_DECL_CAPTURE_STAGE_VALIDATE,
            CM_HIR_DECL_CAPTURE_REASON_METADATA_INVALID);
        goto done;
    }
    if (cm_module_graph_lifetime_id(input->graph) != graph_lifetime
        || cm_module_graph_revision(input->graph) != input->revision
        || cm_import_resolver_lifetime_id(input->imports) != resolver_lifetime
        || cm_import_resolver_generation(input->imports)
            != resolver_generation
        || !cm_import_resolver_matches_graph(input->imports, input->graph)
        || input->hir->storage.lifetime_id != storage_lifetime
        || input->hir->semantic_generation != semantic_generation
        || input->hir->rewind_generation != rewind_generation) {
        result.status = CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY;
        cm_decl_capture_fail(&result,
            CM_HIR_DECL_CAPTURE_STAGE_FINAL_AUTHORITY,
            CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_CHANGED);
        goto done;
    }
    old = *output;
    *output = candidate;
    cm_hir_declaration_metadata_init(&candidate);
    cm_hir_declaration_metadata_destroy(&old);
    result.status = CM_HIR_DECL_CAPTURE_OK;
    result.failure_stage = CM_HIR_DECL_CAPTURE_STAGE_NONE;
    result.failure_reason = CM_HIR_DECL_CAPTURE_REASON_NONE;
    result.module_count = state.module_count;
    result.trait_count = state.trait_count;
    result.associated_count = state.associated_count;
    result.item_count = state.item_count;
    result.value_count = state.value_count;
    result.predicate_count = output->predicate_count;
    result.namespace_count = state.namespace_count;
    result.projected_semantic_attribute_count =
        state.projected_semantic_attribute_count;
    result.semantic_attributes = state.projected_semantic_attribute_count == 0u
        ? CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_EXACT_NONE
        : CM_HIR_DECL_CAPTURE_SEMANTIC_ATTRIBUTES_ABSENT_PROFILE_PROJECTION;
done:
    cm_hir_declaration_metadata_destroy(&candidate);
    cm_decl_state_destroy(&state);
    cm_hir_library_artifact_destroy(&library);
    return result;
}

const char *cm_hir_declaration_capture_status_name(
    CmHirDeclarationCaptureStatus status)
{
    switch (status) {
    case CM_HIR_DECL_CAPTURE_OK: return "ok";
    case CM_HIR_DECL_CAPTURE_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_DECL_CAPTURE_INVALID_AUTHORITY: return "invalid authority";
    case CM_HIR_DECL_CAPTURE_LIBRARY_FAILURE: return "library failure";
    case CM_HIR_DECL_CAPTURE_UNSUPPORTED_HIR: return "unsupported HIR";
    case CM_HIR_DECL_CAPTURE_METADATA_FAILURE: return "metadata failure";
    }
    return "unknown";
}

const char *cm_hir_declaration_capture_stage_name(
    CmHirDeclarationCaptureStage stage)
{
    switch (stage) {
    case CM_HIR_DECL_CAPTURE_STAGE_NONE: return "none";
    case CM_HIR_DECL_CAPTURE_STAGE_INPUT: return "input";
    case CM_HIR_DECL_CAPTURE_STAGE_AUTHORITY: return "authority";
    case CM_HIR_DECL_CAPTURE_STAGE_LIBRARY: return "library";
    case CM_HIR_DECL_CAPTURE_STAGE_MODULES: return "modules";
    case CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE: return "namespace";
    case CM_HIR_DECL_CAPTURE_STAGE_ITEMS: return "items";
    case CM_HIR_DECL_CAPTURE_STAGE_IDENTITY: return "identity";
    case CM_HIR_DECL_CAPTURE_STAGE_MODULE_METADATA: return "module-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_ITEM_METADATA: return "item-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_TYPE_METADATA: return "type-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_NAMESPACE_METADATA:
        return "namespace-metadata";
    case CM_HIR_DECL_CAPTURE_STAGE_VALIDATE: return "validate";
    case CM_HIR_DECL_CAPTURE_STAGE_FINAL_AUTHORITY:
        return "final-authority";
    }
    return "unknown";
}

const char *cm_hir_declaration_capture_reason_name(
    CmHirDeclarationCaptureReason reason)
{
    switch (reason) {
    case CM_HIR_DECL_CAPTURE_REASON_NONE: return "none";
    case CM_HIR_DECL_CAPTURE_REASON_INVALID_ARGUMENT:
        return "invalid-argument";
    case CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_MISMATCH:
        return "authority-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_CRATE_NOT_FOUND:
        return "crate-not-found";
    case CM_HIR_DECL_CAPTURE_REASON_LIBRARY_REJECTED:
        return "library-rejected";
    case CM_HIR_DECL_CAPTURE_REASON_OWNED_DATA_MISSING:
        return "owned-data-missing";
    case CM_HIR_DECL_CAPTURE_REASON_MODULE_CENSUS_INVALID:
        return "module-census-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROVENANCE_INVALID:
        return "semantic-attribute-provenance-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_SEMANTIC_ATTRIBUTE_PROJECTION_LIMIT:
        return "semantic-attribute-projection-limit";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_MODULE_MISSING:
        return "namespace-module-missing";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_LIMIT:
        return "namespace-limit";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_LOOKUP_FAILED:
        return "binding-lookup-failed";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_AUTHORITY_INVALID:
        return "binding-authority-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_NAME_INVALID:
        return "binding-name-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_LIBRARY_MISMATCH:
        return "binding-library-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_SHAPE_UNSUPPORTED:
        return "binding-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_INTRODUCTION_INVALID:
        return "binding-introduction-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_CENSUS_MISMATCH:
        return "binding-census-mismatch";
    case CM_HIR_DECL_CAPTURE_REASON_BINDING_DUPLICATE:
        return "binding-duplicate";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_DEFINITION_UNBOUND:
        return "item-definition-unbound";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_SOURCE_INVALID:
        return "item-source-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_TRAIT_SHAPE_UNSUPPORTED:
        return "trait-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_SHAPE_UNSUPPORTED:
        return "item-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_ATTRIBUTE_PROJECTION_UNSUPPORTED:
        return "item-attribute-projection-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_REEXPORT_ATTRIBUTE_PROJECTION_UNSUPPORTED:
        return "reexport-attribute-projection-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_VALUE_SHAPE_UNSUPPORTED:
        return "value-shape-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_REQUIRED_ITEMS_MISSING:
        return "required-items-missing";
    case CM_HIR_DECL_CAPTURE_REASON_IDENTITY_UNSUPPORTED:
        return "identity-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_MODULE_METADATA_INVALID:
        return "module-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_ITEM_METADATA_INVALID:
        return "item-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_TYPE_UNSUPPORTED:
        return "type-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_PREDICATE_UNSUPPORTED:
        return "predicate-unsupported";
    case CM_HIR_DECL_CAPTURE_REASON_TYPE_METADATA_INVALID:
        return "type-metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_NAMESPACE_TARGET_UNMAPPED:
        return "namespace-target-unmapped";
    case CM_HIR_DECL_CAPTURE_REASON_METADATA_INVALID:
        return "metadata-invalid";
    case CM_HIR_DECL_CAPTURE_REASON_AUTHORITY_CHANGED:
        return "authority-changed";
    }
    return "unknown";
}
