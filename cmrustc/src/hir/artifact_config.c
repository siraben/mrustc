#include "cm/hir/artifact_config.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

typedef struct CmArtifactString {
    const char *data;
    size_t length;
} CmArtifactString;

typedef struct CmArtifactCfgEntry {
    const char *name;
    size_t name_length;
    const char *value;
    size_t value_length;
    int has_value;
} CmArtifactCfgEntry;

typedef struct CmArtifactRenderedCfg {
    CmStrBuf text;
} CmArtifactRenderedCfg;

static int cm_artifact_bounded_length(const char *text, size_t maximum,
    size_t *out_length)
{
    size_t length;

    if (text == NULL || out_length == NULL) return 0;
    length = 0u;
    while (length <= maximum && text[length] != '\0') length += 1u;
    if (length > maximum) return 0;
    *out_length = length;
    return 1;
}

static int cm_artifact_bytes_compare(const char *left, size_t left_length,
    const char *right, size_t right_length)
{
    size_t common;
    int comparison;

    common = left_length < right_length ? left_length : right_length;
    comparison = common == 0u ? 0 : memcmp(left, right, common);
    if (comparison != 0) return comparison;
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static int cm_artifact_string_compare(const void *left_ptr,
    const void *right_ptr)
{
    const CmArtifactString *left;
    const CmArtifactString *right;

    left = (const CmArtifactString *)left_ptr;
    right = (const CmArtifactString *)right_ptr;
    return cm_artifact_bytes_compare(left->data, left->length,
        right->data, right->length);
}

static int cm_artifact_cfg_entry_compare(const void *left_ptr,
    const void *right_ptr)
{
    const CmArtifactCfgEntry *left;
    const CmArtifactCfgEntry *right;
    int comparison;

    left = (const CmArtifactCfgEntry *)left_ptr;
    right = (const CmArtifactCfgEntry *)right_ptr;
    comparison = cm_artifact_bytes_compare(left->name, left->name_length,
        right->name, right->name_length);
    if (comparison != 0) return comparison;
    if (left->has_value != right->has_value) {
        return left->has_value ? 1 : -1;
    }
    if (!left->has_value) return 0;
    return cm_artifact_bytes_compare(left->value, left->value_length,
        right->value, right->value_length);
}

static int cm_artifact_rendered_cfg_compare(const void *left_ptr,
    const void *right_ptr)
{
    const CmArtifactRenderedCfg *left;
    const CmArtifactRenderedCfg *right;

    left = (const CmArtifactRenderedCfg *)left_ptr;
    right = (const CmArtifactRenderedCfg *)right_ptr;
    return cm_artifact_bytes_compare(left->text.data, left->text.len,
        right->text.data, right->text.len);
}

static void cm_artifact_append_u32(CmByteBuf *buffer, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)(value >> 16);
    bytes[2] = (unsigned char)(value >> 8);
    bytes[3] = (unsigned char)value;
    cm_byte_buf_append(buffer, bytes, sizeof(bytes));
}

static void cm_artifact_append_string(CmByteBuf *buffer,
    const CmArtifactString *string)
{
    cm_artifact_append_u32(buffer, (uint32_t)string->length);
    cm_byte_buf_append(buffer, string->data, string->length);
}

static int cm_artifact_is_cfg_name(const char *name, size_t length)
{
    size_t index;
    unsigned char byte;

    if (length == 0u) return 0;
    byte = (unsigned char)name[0];
    if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
            || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
            || byte == (unsigned char)'_')) return 0;
    for (index = 1u; index < length; index += 1u) {
        byte = (unsigned char)name[index];
        if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
                || (byte >= (unsigned char)'a'
                    && byte <= (unsigned char)'z')
                || (byte >= (unsigned char)'0'
                    && byte <= (unsigned char)'9')
                || byte == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_artifact_string_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static int cm_artifact_is_reserved_cfg_name(const char *name)
{
    static const char *const reserved[] = {
        "feature", "target_abi", "target_arch", "target_endian",
        "target_env", "target_family", "target_feature", "target_os",
        "target_pointer_width", "target_vendor"
    };
    size_t index;

    for (index = 0u; index < CM_ARRAY_LEN(reserved); index += 1u) {
        if (strcmp(name, reserved[index]) == 0) return 1;
    }
    return 0;
}

static void cm_artifact_render_cfg_value(CmStrBuf *buffer,
    const char *value, size_t value_length)
{
    static const char hex[] = "0123456789abcdef";
    size_t index;
    unsigned char byte;

    cm_str_buf_push(buffer, '"');
    for (index = 0u; index < value_length; index += 1u) {
        byte = (unsigned char)value[index];
        if (byte == (unsigned char)'"' || byte == (unsigned char)'\\') {
            cm_str_buf_push(buffer, '\\');
            cm_str_buf_push(buffer, (char)byte);
        } else if (byte >= UINT8_C(0x20) && byte <= UINT8_C(0x7e)) {
            cm_str_buf_push(buffer, (char)byte);
        } else {
            cm_str_buf_append(buffer, "\\x");
            cm_str_buf_push(buffer, hex[byte >> 4]);
            cm_str_buf_push(buffer, hex[byte & UINT8_C(0x0f)]);
        }
    }
    cm_str_buf_push(buffer, '"');
}

static void cm_artifact_render_cfg(CmArtifactRenderedCfg *rendered,
    const char *name, size_t name_length, const char *value,
    size_t value_length, int has_value)
{
    cm_str_buf_init(&rendered->text);
    cm_str_buf_append_n(&rendered->text, name, name_length);
    if (has_value) {
        cm_str_buf_push(&rendered->text, '=');
        cm_artifact_render_cfg_value(&rendered->text, value, value_length);
    }
}

static void cm_artifact_rendered_destroy(CmArtifactRenderedCfg *cfgs,
    size_t count)
{
    size_t index;

    if (cfgs == NULL) return;
    for (index = 0u; index < count; index += 1u) {
        cm_str_buf_destroy(&cfgs[index].text);
    }
    cm_free(cfgs);
}

static int cm_artifact_has_string(const char *const *strings, size_t count,
    const char *needle)
{
    size_t index;

    for (index = 0u; index < count; index += 1u) {
        if (strings[index] != NULL && strcmp(strings[index], needle) == 0) {
            return 1;
        }
    }
    return 0;
}

static int cm_artifact_has_entry(const CmCfgEntry *entries, size_t count,
    const CmCfgEntry *needle)
{
    size_t index;

    for (index = 0u; index < count; index += 1u) {
        if (entries[index].name != NULL
            && strcmp(entries[index].name, needle->name) == 0
            && ((entries[index].value == NULL && needle->value == NULL)
                || (entries[index].value != NULL && needle->value != NULL
                    && strcmp(entries[index].value, needle->value) == 0))) {
            return 1;
        }
    }
    return 0;
}

static CmHirArtifactConfigStatus cm_artifact_validate_scalar_input(
    const CmTargetDesc *target, enum cm_edition edition,
    CmHirArtifactPanicStrategy panic_strategy, const CmCfgSet *cfg,
    const char **pointer_width, const char **endian,
    const char **panic_text)
{
    const CmCfgEnvironment *environment;
    const char *target_strings[7];
    const char *cfg_strings[8];
    size_t string_length;
    size_t index;

    if (target == NULL || cfg == NULL || pointer_width == NULL
        || endian == NULL || panic_text == NULL || target->triple == NULL
        || target->architecture == NULL || target->operating_system == NULL
        || target->environment == NULL || target->abi == NULL
        || target->vendor == NULL || target->family == NULL
        || (target->target_feature_count != 0u
            && target->target_features == NULL)
        || (target->cfg_entry_count != 0u && target->cfg_entries == NULL)) {
        return CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT;
    }
    if (edition != CM_EDITION_2015 && edition != CM_EDITION_2018
        && edition != CM_EDITION_2021 && edition != CM_EDITION_2024) {
        return CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_EDITION;
    }
    if (panic_strategy == CM_HIR_ARTIFACT_PANIC_ABORT) {
        *panic_text = "abort";
    } else if (panic_strategy == CM_HIR_ARTIFACT_PANIC_UNWIND) {
        *panic_text = "unwind";
    } else {
        return CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_PANIC_STRATEGY;
    }
    if (target->endian == CM_ENDIAN_LITTLE) {
        *endian = "little";
    } else if (target->endian == CM_ENDIAN_BIG) {
        *endian = "big";
    } else {
        return CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_ENDIAN;
    }
    if (target->pointer_bits == 32u) {
        *pointer_width = "32";
    } else if (target->pointer_bits == 64u) {
        *pointer_width = "64";
    } else {
        return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
    }
    environment = &cfg->environment;
    target_strings[0] = target->triple;
    target_strings[1] = target->architecture;
    target_strings[2] = target->operating_system;
    target_strings[3] = target->environment;
    target_strings[4] = target->abi;
    target_strings[5] = target->vendor;
    target_strings[6] = target->family;
    for (index = 0u; index < CM_ARRAY_LEN(target_strings); index += 1u) {
        if (!cm_artifact_bounded_length(target_strings[index],
                CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE, &string_length)) {
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
    }
    if (!cm_artifact_is_cfg_name(target->family, strlen(target->family))) {
        return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
    }
    cfg_strings[0] = environment->target_arch;
    cfg_strings[1] = environment->target_os;
    cfg_strings[2] = environment->target_env;
    cfg_strings[3] = environment->target_abi;
    cfg_strings[4] = environment->target_vendor;
    cfg_strings[5] = environment->target_family;
    cfg_strings[6] = environment->target_pointer_width;
    cfg_strings[7] = environment->target_endian;
    for (index = 0u; index < CM_ARRAY_LEN(cfg_strings); index += 1u) {
        if (!cm_artifact_bounded_length(cfg_strings[index],
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &string_length)) {
            return cfg_strings[index] == NULL
                ? CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG
                : CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
    }
    if (!cm_artifact_string_equal(environment->target_arch,
            target->architecture)
        || !cm_artifact_string_equal(environment->target_os,
            target->operating_system)
        || !cm_artifact_string_equal(environment->target_env,
            target->environment)
        || !cm_artifact_string_equal(environment->target_abi, target->abi)
        || !cm_artifact_string_equal(environment->target_vendor,
            target->vendor)
        || !cm_artifact_string_equal(environment->target_family,
            target->family)
        || !cm_artifact_string_equal(environment->target_pointer_width,
            *pointer_width)
        || !cm_artifact_string_equal(environment->target_endian, *endian)
        || (environment->feature_count != 0u
            && environment->features == NULL)
        || (environment->target_feature_count != 0u
            && environment->target_features == NULL)
        || (environment->entry_count != 0u
            && environment->entries == NULL)) {
        return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
    }
    return CM_HIR_ARTIFACT_CONFIG_OK;
}

static CmHirArtifactConfigStatus cm_artifact_collect_target_strings(
    const CmTargetDesc *target, CmArtifactString **out_features,
    CmArtifactCfgEntry **out_entries)
{
    CmArtifactString *features;
    CmArtifactCfgEntry *entries;
    size_t index;

    features = NULL;
    entries = NULL;
    if (target->target_feature_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || target->cfg_entry_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT) {
        return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
    }
    if (target->target_feature_count != 0u) {
        features = (CmArtifactString *)cm_alloc_zeroed(
            target->target_feature_count, sizeof(*features));
    }
    for (index = 0u; index < target->target_feature_count; index += 1u) {
        if (!cm_artifact_bounded_length(target->target_features[index],
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &features[index].length)
            || features[index].length == 0u) {
            cm_free(features);
            return target->target_features[index] == NULL
                ? CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT
                : CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        features[index].data = target->target_features[index];
    }
    if (target->target_feature_count > 1u) qsort(features,
        target->target_feature_count, sizeof(*features),
        cm_artifact_string_compare);
    for (index = 1u; index < target->target_feature_count; index += 1u) {
        if (cm_artifact_string_compare(&features[index - 1u],
                &features[index]) == 0) {
            cm_free(features);
            return CM_HIR_ARTIFACT_CONFIG_DUPLICATE;
        }
    }
    if (target->cfg_entry_count != 0u) entries =
        (CmArtifactCfgEntry *)cm_alloc_zeroed(target->cfg_entry_count,
            sizeof(*entries));
    for (index = 0u; index < target->cfg_entry_count; index += 1u) {
        if (!cm_artifact_bounded_length(target->cfg_entries[index].name,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &entries[index].name_length)) {
            cm_free(entries);
            cm_free(features);
            return target->cfg_entries[index].name == NULL
                ? CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT
                : CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        if (!cm_artifact_is_cfg_name(target->cfg_entries[index].name,
                entries[index].name_length)) {
            cm_free(entries);
            cm_free(features);
            return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
        }
        entries[index].name = target->cfg_entries[index].name;
        entries[index].value = target->cfg_entries[index].value;
        entries[index].has_value = entries[index].value != NULL;
        if (entries[index].has_value
            && !cm_artifact_bounded_length(entries[index].value,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE,
                &entries[index].value_length)) {
            cm_free(entries);
            cm_free(features);
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
    }
    if (target->cfg_entry_count > 1u) qsort(entries,
        target->cfg_entry_count, sizeof(*entries),
        cm_artifact_cfg_entry_compare);
    for (index = 1u; index < target->cfg_entry_count; index += 1u) {
        if (cm_artifact_cfg_entry_compare(&entries[index - 1u],
                &entries[index]) == 0) {
            cm_free(entries);
            cm_free(features);
            return CM_HIR_ARTIFACT_CONFIG_DUPLICATE;
        }
    }
    *out_features = features;
    *out_entries = entries;
    return CM_HIR_ARTIFACT_CONFIG_OK;
}

static CmHirArtifactConfigStatus cm_artifact_build_descriptor(
    const CmTargetDesc *target, const CmArtifactString *features,
    const CmArtifactCfgEntry *entries, CmByteBuf *descriptor)
{
    static const char header[] = "cmrustc-target-v1";
    const char *fields[7];
    CmArtifactString string;
    size_t field_count;
    size_t index;

    fields[0] = target->triple;
    fields[1] = target->architecture;
    fields[2] = target->operating_system;
    fields[3] = target->environment;
    fields[4] = target->abi;
    fields[5] = target->vendor;
    fields[6] = target->family;
    cm_byte_buf_append(descriptor, header, sizeof(header));
    field_count = CM_ARRAY_LEN(fields);
    for (index = 0u; index < field_count; index += 1u) {
        string.data = fields[index];
        if (!cm_artifact_bounded_length(string.data,
                CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE, &string.length)) {
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        cm_artifact_append_string(descriptor, &string);
    }
    cm_artifact_append_u32(descriptor, (uint32_t)target->pointer_bits);
    cm_byte_buf_push(descriptor, target->endian == CM_ENDIAN_LITTLE
        ? UINT8_C(0) : UINT8_C(1));
    cm_artifact_append_u32(descriptor,
        (uint32_t)target->target_feature_count);
    for (index = 0u; index < target->target_feature_count; index += 1u) {
        cm_artifact_append_string(descriptor, &features[index]);
    }
    cm_artifact_append_u32(descriptor, (uint32_t)target->cfg_entry_count);
    for (index = 0u; index < target->cfg_entry_count; index += 1u) {
        string.data = entries[index].name;
        string.length = entries[index].name_length;
        cm_artifact_append_string(descriptor, &string);
        cm_byte_buf_push(descriptor, entries[index].has_value
            ? UINT8_C(1) : UINT8_C(0));
        if (entries[index].has_value) {
            string.data = entries[index].value;
            string.length = entries[index].value_length;
            cm_artifact_append_string(descriptor, &string);
        }
    }
    if (descriptor->len > CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE) {
        return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
    }
    return CM_HIR_ARTIFACT_CONFIG_OK;
}

static CmHirArtifactConfigStatus cm_artifact_build_cfgs(
    const CmTargetDesc *target, const CmCfgSet *cfg,
    CmArtifactRenderedCfg **out_rendered, size_t *out_count)
{
    static const char *const target_names[] = {
        "target_arch", "target_os", "target_env", "target_abi",
        "target_vendor", "target_family", "target_pointer_width",
        "target_endian"
    };
    const char *target_values[8];
    const CmCfgEnvironment *environment;
    CmArtifactRenderedCfg *rendered;
    size_t count;
    size_t index;
    size_t position;
    size_t length;

    environment = &cfg->environment;
    if (environment->feature_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || environment->target_feature_count
            > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || environment->entry_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT) {
        return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
    }
    count = CM_ARRAY_LEN(target_names) + 1u;
    if (environment->feature_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT - count
        || environment->target_feature_count
            > CM_HIR_ARTIFACT_MAX_CFG_COUNT - count
                - environment->feature_count
        || environment->entry_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT - count
                - environment->feature_count
                - environment->target_feature_count) {
        return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
    }
    count += environment->feature_count
        + environment->target_feature_count + environment->entry_count;
    rendered = (CmArtifactRenderedCfg *)cm_alloc_zeroed(count,
        sizeof(*rendered));
    target_values[0] = environment->target_arch;
    target_values[1] = environment->target_os;
    target_values[2] = environment->target_env;
    target_values[3] = environment->target_abi;
    target_values[4] = environment->target_vendor;
    target_values[5] = environment->target_family;
    target_values[6] = environment->target_pointer_width;
    target_values[7] = environment->target_endian;
    position = 0u;
    for (index = 0u; index < CM_ARRAY_LEN(target_names); index += 1u) {
        length = strlen(target_values[index]);
        cm_artifact_render_cfg(&rendered[position], target_names[index],
            strlen(target_names[index]), target_values[index], length, 1);
        position += 1u;
    }
    cm_artifact_render_cfg(&rendered[position], target->family,
        strlen(target->family), NULL, 0u, 0);
    position += 1u;
    for (index = 0u; index < environment->feature_count; index += 1u) {
        if (!cm_artifact_bounded_length(environment->features[index],
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &length)
            || length == 0u) {
            cm_artifact_rendered_destroy(rendered, position);
            return environment->features[index] == NULL
                ? CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT
                : CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        cm_artifact_render_cfg(&rendered[position], "feature", 7u,
            environment->features[index], length, 1);
        position += 1u;
    }
    for (index = 0u; index < environment->target_feature_count;
        index += 1u) {
        if (!cm_artifact_bounded_length(
                environment->target_features[index],
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &length)
            || length == 0u
            || !cm_artifact_has_string(target->target_features,
                target->target_feature_count,
                environment->target_features[index])) {
            cm_artifact_rendered_destroy(rendered, position);
            return environment->target_features[index] == NULL
                ? CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT
                : CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
        }
        cm_artifact_render_cfg(&rendered[position], "target_feature", 14u,
            environment->target_features[index], length, 1);
        position += 1u;
    }
    if (environment->target_feature_count != target->target_feature_count) {
        cm_artifact_rendered_destroy(rendered, position);
        return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
    }
    for (index = 0u; index < environment->entry_count; index += 1u) {
        const CmCfgEntry *entry;
        size_t value_length;

        entry = &environment->entries[index];
        if (!cm_artifact_bounded_length(entry->name,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &length)) {
            cm_artifact_rendered_destroy(rendered, position);
            return entry->name == NULL
                ? CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT
                : CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        if (!cm_artifact_is_cfg_name(entry->name, length)
            || cm_artifact_is_reserved_cfg_name(entry->name)) {
            cm_artifact_rendered_destroy(rendered, position);
            return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
        }
        value_length = 0u;
        if (entry->value != NULL
            && !cm_artifact_bounded_length(entry->value,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE, &value_length)) {
            cm_artifact_rendered_destroy(rendered, position);
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        cm_artifact_render_cfg(&rendered[position], entry->name, length,
            entry->value, value_length, entry->value != NULL);
        position += 1u;
    }
    for (index = 0u; index < target->cfg_entry_count; index += 1u) {
        if (!cm_artifact_has_entry(environment->entries,
                environment->entry_count, &target->cfg_entries[index])) {
            cm_artifact_rendered_destroy(rendered, position);
            return CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG;
        }
    }
    if (count > 1u) qsort(rendered, count, sizeof(*rendered),
        cm_artifact_rendered_cfg_compare);
    for (index = 0u; index < count; index += 1u) {
        if (rendered[index].text.len > CM_HIR_ARTIFACT_MAX_CFG_SIZE) {
            cm_artifact_rendered_destroy(rendered, count);
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
        if (index != 0u
            && cm_artifact_rendered_cfg_compare(&rendered[index - 1u],
                &rendered[index]) == 0) {
            cm_artifact_rendered_destroy(rendered, count);
            return CM_HIR_ARTIFACT_CONFIG_DUPLICATE;
        }
    }
    *out_rendered = rendered;
    *out_count = count;
    return CM_HIR_ARTIFACT_CONFIG_OK;
}

void cm_hir_artifact_config_init(CmHirArtifactConfig *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    cm_byte_buf_init(&config->descriptor_storage);
    cm_byte_buf_init(&config->cfg_storage);
}

void cm_hir_artifact_config_destroy(CmHirArtifactConfig *config)
{
    if (config == NULL) return;
    cm_free(config->cfgs);
    cm_byte_buf_destroy(&config->descriptor_storage);
    cm_byte_buf_destroy(&config->cfg_storage);
    cm_hir_artifact_config_init(config);
}

CmHirArtifactConfigStatus cm_hir_artifact_config_build(
    const CmTargetDesc *target, enum cm_edition edition,
    CmHirArtifactPanicStrategy panic_strategy,
    const CmCfgSet *effective_cfg, CmHirArtifactConfig *out_config)
{
    CmHirArtifactConfig candidate;
    CmArtifactString *features;
    CmArtifactCfgEntry *entries;
    CmArtifactRenderedCfg *rendered;
    CmHirArtifactConfigStatus status;
    const char *pointer_width;
    const char *endian;
    const char *panic_text;
    size_t count;
    size_t index;
    size_t total_size;

    if (out_config == NULL) return CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT;
    features = NULL;
    entries = NULL;
    rendered = NULL;
    count = 0u;
    status = cm_artifact_validate_scalar_input(target, edition,
        panic_strategy, effective_cfg, &pointer_width, &endian,
        &panic_text);
    if (status != CM_HIR_ARTIFACT_CONFIG_OK) return status;
    status = cm_artifact_collect_target_strings(target, &features, &entries);
    if (status != CM_HIR_ARTIFACT_CONFIG_OK) return status;
    cm_hir_artifact_config_init(&candidate);
    status = cm_artifact_build_descriptor(target, features, entries,
        &candidate.descriptor_storage);
    cm_free(entries);
    cm_free(features);
    if (status != CM_HIR_ARTIFACT_CONFIG_OK) {
        cm_hir_artifact_config_destroy(&candidate);
        return status;
    }
    status = cm_artifact_build_cfgs(target, effective_cfg, &rendered,
        &count);
    if (status != CM_HIR_ARTIFACT_CONFIG_OK) {
        cm_hir_artifact_config_destroy(&candidate);
        return status;
    }
    total_size = 0u;
    for (index = 0u; index < count; index += 1u) {
        if (!cm_size_add(total_size, rendered[index].text.len,
                &total_size)) {
            cm_artifact_rendered_destroy(rendered, count);
            cm_hir_artifact_config_destroy(&candidate);
            return CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED;
        }
    }
    cm_byte_buf_reserve(&candidate.cfg_storage, total_size);
    candidate.cfgs = (CmHirArtifactBytes *)cm_alloc_zeroed(count,
        sizeof(*candidate.cfgs));
    for (index = 0u; index < count; index += 1u) {
        size_t offset;

        offset = candidate.cfg_storage.len;
        cm_byte_buf_append(&candidate.cfg_storage,
            rendered[index].text.data, rendered[index].text.len);
        candidate.cfgs[index].data = candidate.cfg_storage.data + offset;
        candidate.cfgs[index].length = rendered[index].text.len;
    }
    candidate.edition = (uint32_t)edition;
    candidate.target_descriptor.data = candidate.descriptor_storage.data;
    candidate.target_descriptor.length = candidate.descriptor_storage.len;
    candidate.panic_strategy.data = panic_text;
    candidate.panic_strategy.length = strlen(panic_text);
    candidate.cfg_count = count;
    cm_artifact_rendered_destroy(rendered, count);
    cm_hir_artifact_config_destroy(out_config);
    *out_config = candidate;
    return CM_HIR_ARTIFACT_CONFIG_OK;
}

const char *cm_hir_artifact_config_status_name(
    CmHirArtifactConfigStatus status)
{
    switch (status) {
    case CM_HIR_ARTIFACT_CONFIG_OK: return "ok";
    case CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_ARTIFACT_CONFIG_LIMIT_EXCEEDED:
        return "limit exceeded";
    case CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_EDITION:
        return "unsupported edition";
    case CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_PANIC_STRATEGY:
        return "unsupported panic strategy";
    case CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_ENDIAN:
        return "unsupported endian";
    case CM_HIR_ARTIFACT_CONFIG_DUPLICATE: return "duplicate";
    case CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG:
        return "noncanonical cfg";
    default: return "unknown artifact config status";
    }
}
