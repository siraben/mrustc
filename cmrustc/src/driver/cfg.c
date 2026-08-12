#include "cm/driver/cfg.h"

#include <string.h>

static int cm_cfg_optional_string_equal(const char *left,
    const char *right)
{
    if (left == NULL || right == NULL) return left == right;
    return strcmp(left, right) == 0;
}

static int cm_target_cfg_descriptor_equal(const CmTargetDesc *left,
    const CmTargetDesc *right)
{
    size_t index;

    if (left == NULL || right == NULL
        || !cm_cfg_optional_string_equal(left->triple, right->triple)
        || !cm_cfg_optional_string_equal(left->architecture,
            right->architecture)
        || !cm_cfg_optional_string_equal(left->operating_system,
            right->operating_system)
        || !cm_cfg_optional_string_equal(left->environment,
            right->environment)
        || !cm_cfg_optional_string_equal(left->abi, right->abi)
        || !cm_cfg_optional_string_equal(left->vendor, right->vendor)
        || !cm_cfg_optional_string_equal(left->family, right->family)
        || left->pointer_bits != right->pointer_bits
        || left->endian != right->endian
        || left->target_feature_count != right->target_feature_count
        || left->cfg_entry_count != right->cfg_entry_count
        || (left->target_feature_count != 0u
            && (left->target_features == NULL
                || right->target_features == NULL))
        || (left->cfg_entry_count != 0u
            && (left->cfg_entries == NULL || right->cfg_entries == NULL))) {
        return 0;
    }
    for (index = 0u; index < left->target_feature_count; ++index) {
        if (!cm_cfg_optional_string_equal(left->target_features[index],
                right->target_features[index])) return 0;
    }
    for (index = 0u; index < left->cfg_entry_count; ++index) {
        if (!cm_cfg_optional_string_equal(left->cfg_entries[index].name,
                right->cfg_entries[index].name)
            || !cm_cfg_optional_string_equal(
                left->cfg_entries[index].value,
                right->cfg_entries[index].value)) return 0;
    }
    return 1;
}

int cm_target_cfg_set(CmCfgSet *cfg, const CmTargetDesc *target)
{
    const char *pointer_width;
    const char *endian;
    const CmTargetDesc *canonical;
    size_t index;

    if (cfg == NULL || target == NULL || target->triple == NULL
        || target->architecture == NULL || target->operating_system == NULL
        || target->environment == NULL || target->abi == NULL
        || target->vendor == NULL || target->family == NULL
        || (target->target_feature_count != 0u
            && target->target_features == NULL)
        || (target->cfg_entry_count != 0u
            && target->cfg_entries == NULL)) {
        return 0;
    }
    canonical = cm_target_find(target->triple);
    if (!cm_target_cfg_descriptor_equal(target, canonical)) return 0;
    for (index = 0u; index < target->target_feature_count; ++index) {
        if (target->target_features[index] == NULL
            || target->target_features[index][0] == '\0') return 0;
    }
    for (index = 0u; index < target->cfg_entry_count; ++index) {
        if (target->cfg_entries[index].name == NULL
            || target->cfg_entries[index].name[0] == '\0') return 0;
    }
    if (target->pointer_bits == 32u) {
        pointer_width = "32";
    } else if (target->pointer_bits == 64u) {
        pointer_width = "64";
    } else {
        return 0;
    }
    if (target->endian == CM_ENDIAN_LITTLE) {
        endian = "little";
    } else if (target->endian == CM_ENDIAN_BIG) {
        endian = "big";
    } else {
        return 0;
    }
    cm_cfg_set_init(cfg);
    cfg->environment.target_arch = target->architecture;
    cfg->environment.target_os = target->operating_system;
    cfg->environment.target_env = target->environment;
    cfg->environment.target_abi = target->abi;
    cfg->environment.target_vendor = target->vendor;
    cfg->environment.target_family = target->family;
    cfg->environment.target_pointer_width = pointer_width;
    cfg->environment.target_endian = endian;
    cfg->environment.target_features = target->target_features;
    cfg->environment.target_feature_count = target->target_feature_count;
    cfg->environment.entries = target->cfg_entries;
    cfg->environment.entry_count = target->cfg_entry_count;
    return 1;
}
