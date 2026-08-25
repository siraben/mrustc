#include "cm/hir/artifact_config.h"

#include <assert.h>
#include <string.h>

static void test_cfg_for_target(CmCfgSet *cfg, const CmTargetDesc *target)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->environment.target_arch = target->architecture;
    cfg->environment.target_os = target->operating_system;
    cfg->environment.target_env = target->environment;
    cfg->environment.target_abi = target->abi;
    cfg->environment.target_vendor = target->vendor;
    cfg->environment.target_family = target->family;
    cfg->environment.target_pointer_width = target->pointer_bits == 32u
        ? "32" : "64";
    cfg->environment.target_endian = target->endian == CM_ENDIAN_LITTLE
        ? "little" : "big";
    cfg->environment.target_features = target->target_features;
    cfg->environment.target_feature_count = target->target_feature_count;
    cfg->environment.entries = target->cfg_entries;
    cfg->environment.entry_count = target->cfg_entry_count;
}

static int test_bytes_compare(const CmHirArtifactBytes *left,
    const CmHirArtifactBytes *right)
{
    size_t common;
    int comparison;

    common = left->length < right->length ? left->length : right->length;
    comparison = common == 0u ? 0
        : memcmp(left->data, right->data, common);
    if (comparison != 0) return comparison;
    if (left->length < right->length) return -1;
    if (left->length > right->length) return 1;
    return 0;
}

static int test_has_cfg(const CmHirArtifactConfig *config,
    const char *expected)
{
    size_t expected_length;
    size_t index;

    expected_length = strlen(expected);
    for (index = 0u; index < config->cfg_count; index += 1u) {
        if (config->cfgs[index].length == expected_length
            && memcmp(config->cfgs[index].data, expected,
                expected_length) == 0) return 1;
    }
    return 0;
}

static void test_assert_canonical(const CmHirArtifactConfig *config)
{
    size_t index;

    assert(config->target_descriptor.data
        == config->descriptor_storage.data);
    assert(config->target_descriptor.length
        == config->descriptor_storage.len);
    assert(config->target_descriptor.length != 0u);
    assert(config->cfg_count != 0u);
    for (index = 1u; index < config->cfg_count; index += 1u) {
        assert(test_bytes_compare(&config->cfgs[index - 1u],
            &config->cfgs[index]) < 0);
    }
}

static void test_all_current_targets(void)
{
    static const char *const triples[] = {
        "i386-unknown-linux-musl",
        "i686-unknown-linux-musl",
        "x86_64-unknown-linux-gnu",
        "x86_64-unknown-linux-musl"
    };
    CmHirArtifactConfig configs[4];
    CmCfgSet cfg;
    const CmTargetDesc *target;
    size_t index;
    size_t other;

    for (index = 0u; index < 4u; index += 1u) {
        cm_hir_artifact_config_init(&configs[index]);
        target = cm_target_find(triples[index]);
        assert(target != NULL);
        test_cfg_for_target(&cfg, target);
        assert(cm_hir_artifact_config_build(target, CM_EDITION_2021,
            CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &configs[index])
            == CM_HIR_ARTIFACT_CONFIG_OK);
        test_assert_canonical(&configs[index]);
        assert(configs[index].edition == UINT32_C(2021));
        assert(configs[index].panic_strategy.length == 5u);
        assert(memcmp(configs[index].panic_strategy.data, "abort", 5u)
            == 0);
        assert(test_has_cfg(&configs[index], "target_os=\"linux\""));
        assert(test_has_cfg(&configs[index], "unix"));
        if (target->target_feature_count != 0u) {
            assert(test_has_cfg(&configs[index],
                "target_feature=\"sse2\""));
        }
        for (other = 0u; other < index; other += 1u) {
            assert(test_bytes_compare(&configs[index].target_descriptor,
                &configs[other].target_descriptor) != 0);
        }
    }
    for (index = 0u; index < 4u; index += 1u) {
        cm_hir_artifact_config_destroy(&configs[index]);
    }
}

static void test_order_independence_and_escaping(void)
{
    const CmTargetDesc *known;
    CmTargetDesc reversed;
    const char *features[3];
    CmCfgEntry entries[16];
    const char *crate_features[2];
    CmCfgSet canonical_cfg;
    CmCfgSet reversed_cfg;
    CmHirArtifactConfig canonical;
    CmHirArtifactConfig reordered;
    size_t index;

    known = cm_target_find("x86_64-unknown-linux-gnu");
    assert(known != NULL);
    assert(known->target_feature_count == 3u);
    assert(known->cfg_entry_count == 16u);
    reversed = *known;
    for (index = 0u; index < 3u; index += 1u) {
        features[index] = known->target_features[2u - index];
    }
    for (index = 0u; index < 16u; index += 1u) {
        entries[index] = known->cfg_entries[15u - index];
    }
    reversed.target_features = features;
    reversed.cfg_entries = entries;
    test_cfg_for_target(&canonical_cfg, known);
    crate_features[0] = "quote\"slash\\";
    crate_features[1] = "line\nfeed";
    canonical_cfg.environment.features = crate_features;
    canonical_cfg.environment.feature_count = 2u;
    test_cfg_for_target(&reversed_cfg, &reversed);
    reversed_cfg.environment.features = crate_features;
    reversed_cfg.environment.feature_count = 2u;
    cm_hir_artifact_config_init(&canonical);
    cm_hir_artifact_config_init(&reordered);
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2024,
        CM_HIR_ARTIFACT_PANIC_UNWIND, &canonical_cfg, &canonical)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(cm_hir_artifact_config_build(&reversed, CM_EDITION_2024,
        CM_HIR_ARTIFACT_PANIC_UNWIND, &reversed_cfg, &reordered)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&canonical.target_descriptor,
        &reordered.target_descriptor) == 0);
    assert(canonical.cfg_count == reordered.cfg_count);
    for (index = 0u; index < canonical.cfg_count; index += 1u) {
        assert(test_bytes_compare(&canonical.cfgs[index],
            &reordered.cfgs[index]) == 0);
    }
    assert(test_has_cfg(&canonical, "feature=\"quote\\\"slash\\\\\""));
    assert(test_has_cfg(&canonical, "feature=\"line\\x0afeed\""));
    assert(memcmp(canonical.panic_strategy.data, "unwind", 6u) == 0);
    cm_hir_artifact_config_destroy(&reordered);
    cm_hir_artifact_config_destroy(&canonical);
}

static void test_perturbed_descriptor_fields(void)
{
    const CmTargetDesc *known;
    CmTargetDesc changed;
    CmCfgSet cfg;
    CmHirArtifactConfig baseline;
    CmHirArtifactConfig candidate;
    const char *feature_values[3];
    CmCfgEntry entry_values[16];
    size_t index;

    known = cm_target_find("x86_64-unknown-linux-gnu");
    assert(known != NULL);
    cm_hir_artifact_config_init(&baseline);
    cm_hir_artifact_config_init(&candidate);
    test_cfg_for_target(&cfg, known);
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &baseline)
        == CM_HIR_ARTIFACT_CONFIG_OK);

#define TEST_CHANGED_STRING(member, value, cfg_member) \
    do { \
        changed = *known; \
        changed.member = (value); \
        test_cfg_for_target(&cfg, &changed); \
        cfg.environment.cfg_member = changed.member; \
        assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021, \
            CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate) \
            == CM_HIR_ARTIFACT_CONFIG_OK); \
        assert(test_bytes_compare(&baseline.target_descriptor, \
            &candidate.target_descriptor) != 0); \
    } while (0)

    changed = *known;
    changed.triple = "x86_64-example-linux-gnu";
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&baseline.target_descriptor,
        &candidate.target_descriptor) != 0);
    TEST_CHANGED_STRING(architecture, "amd64", target_arch);
    TEST_CHANGED_STRING(operating_system, "example-os", target_os);
    TEST_CHANGED_STRING(environment, "example-env", target_env);
    TEST_CHANGED_STRING(abi, "example-abi", target_abi);
    TEST_CHANGED_STRING(vendor, "example-vendor", target_vendor);
    TEST_CHANGED_STRING(family, "posix", target_family);
#undef TEST_CHANGED_STRING

    changed = *known;
    changed.pointer_bits = 32u;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&baseline.target_descriptor,
        &candidate.target_descriptor) != 0);
    changed = *known;
    changed.endian = CM_ENDIAN_BIG;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&baseline.target_descriptor,
        &candidate.target_descriptor) != 0);

    changed = *known;
    for (index = 0u; index < 3u; index += 1u) {
        feature_values[index] = known->target_features[index];
    }
    feature_values[2] = "sse3";
    changed.target_features = feature_values;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&baseline.target_descriptor,
        &candidate.target_descriptor) != 0);

    changed = *known;
    for (index = 0u; index < 16u; index += 1u) {
        entry_values[index] = known->cfg_entries[index];
    }
    entry_values[1].value = "9";
    changed.cfg_entries = entry_values;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &candidate)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    assert(test_bytes_compare(&baseline.target_descriptor,
        &candidate.target_descriptor) != 0);

    cm_hir_artifact_config_destroy(&candidate);
    cm_hir_artifact_config_destroy(&baseline);
}

static void test_rejections_and_transaction(void)
{
    const CmTargetDesc *known;
    CmTargetDesc changed;
    CmCfgSet cfg;
    CmHirArtifactConfig output;
    const void *saved_descriptor;
    size_t saved_length;
    size_t saved_cfg_count;
    const char *duplicate_features[3];
    CmCfgEntry duplicate_entries[16];
    CmCfgEntry reserved_entry;
    CmCfgEntry missing_entries[15];
    const char *duplicate_crate_features[2];
    CmCfgEntry duplicate_effective_entries[17];
    size_t index;

    known = cm_target_find("x86_64-unknown-linux-gnu");
    assert(known != NULL);
    test_cfg_for_target(&cfg, known);
    cm_hir_artifact_config_init(&output);
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_OK);
    saved_descriptor = output.target_descriptor.data;
    saved_length = output.target_descriptor.length;
    saved_cfg_count = output.cfg_count;

    assert(cm_hir_artifact_config_build(NULL, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT);
    assert(cm_hir_artifact_config_build(known, (enum cm_edition)999,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_EDITION);
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        (CmHirArtifactPanicStrategy)999, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_PANIC_STRATEGY);
    changed = *known;
    changed.endian = (CmEndian)999;
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_UNSUPPORTED_ENDIAN);
    changed = *known;
    changed.pointer_bits = 48u;
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG);
    changed = *known;
    changed.vendor = NULL;
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_INVALID_ARGUMENT);

    changed = *known;
    duplicate_features[0] = known->target_features[0];
    duplicate_features[1] = known->target_features[0];
    duplicate_features[2] = known->target_features[2];
    changed.target_features = duplicate_features;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_DUPLICATE);

    changed = *known;
    for (index = 0u; index < 16u; index += 1u) {
        duplicate_entries[index] = known->cfg_entries[index];
    }
    duplicate_entries[1] = duplicate_entries[0];
    changed.cfg_entries = duplicate_entries;
    test_cfg_for_target(&cfg, &changed);
    assert(cm_hir_artifact_config_build(&changed, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_DUPLICATE);

    test_cfg_for_target(&cfg, known);
    cfg.environment.target_arch = "aarch64";
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG);
    test_cfg_for_target(&cfg, known);
    cfg.environment.target_features = NULL;
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG);
    test_cfg_for_target(&cfg, known);
    duplicate_crate_features[0] = "same";
    duplicate_crate_features[1] = "same";
    cfg.environment.features = duplicate_crate_features;
    cfg.environment.feature_count = 2u;
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_DUPLICATE);
    test_cfg_for_target(&cfg, known);
    for (index = 0u; index < 16u; index += 1u) {
        duplicate_effective_entries[index] = known->cfg_entries[index];
    }
    duplicate_effective_entries[16] = known->cfg_entries[0];
    cfg.environment.entries = duplicate_effective_entries;
    cfg.environment.entry_count = 17u;
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_DUPLICATE);
    test_cfg_for_target(&cfg, known);
    reserved_entry.name = "target_arch";
    reserved_entry.value = "x86_64";
    cfg.environment.entries = &reserved_entry;
    cfg.environment.entry_count = 1u;
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG);
    test_cfg_for_target(&cfg, known);
    for (index = 0u; index < 15u; index += 1u) {
        missing_entries[index] = known->cfg_entries[index];
    }
    cfg.environment.entries = missing_entries;
    cfg.environment.entry_count = 15u;
    assert(cm_hir_artifact_config_build(known, CM_EDITION_2021,
        CM_HIR_ARTIFACT_PANIC_ABORT, &cfg, &output)
        == CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG);

    assert(output.target_descriptor.data == saved_descriptor);
    assert(output.target_descriptor.length == saved_length);
    assert(output.cfg_count == saved_cfg_count);
    cm_hir_artifact_config_destroy(&output);
}

static void test_status_names(void)
{
    assert(strcmp(cm_hir_artifact_config_status_name(
        CM_HIR_ARTIFACT_CONFIG_OK), "ok") == 0);
    assert(strcmp(cm_hir_artifact_config_status_name(
        CM_HIR_ARTIFACT_CONFIG_NONCANONICAL_CFG),
        "noncanonical cfg") == 0);
    assert(strcmp(cm_hir_artifact_config_status_name(
        (CmHirArtifactConfigStatus)999),
        "unknown artifact config status") == 0);
}

int main(void)
{
    test_all_current_targets();
    test_order_independence_and_escaping();
    test_perturbed_descriptor_fields();
    test_rejections_and_transaction();
    test_status_names();
    return 0;
}
