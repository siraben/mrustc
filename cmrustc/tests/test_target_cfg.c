#include "cm/driver/cfg.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *test, const char *message)
{
    fprintf(stderr, "target-cfg/%s: %s\n", test, message);
    failures += 1;
}

static int evaluates(const CmCfgSet *cfg, const char *predicate)
{
    CmCfgEvaluation result;

    result = cm_cfg_evaluate(&cfg->environment, predicate,
        strlen(predicate));
    return result.status == CM_MACRO_OK && result.value;
}

static void test_x86_64(void)
{
    const CmTargetDesc *target;
    CmCfgSet cfg;

    target = cm_target_find("x86_64-unknown-linux-gnu");
    if (!cm_target_cfg_set(&cfg, target)) {
        fail("x86-64", "supported target did not produce cfg facts");
        return;
    }
    if (!evaluates(&cfg, "all(unix, target_thread_local, "
            "target_arch = \"x86_64\", target_os = \"linux\", "
            "target_env = \"gnu\", target_abi = \"\", "
            "target_vendor = \"unknown\", "
            "target_family = \"unix\", target_pointer_width = \"64\", "
            "target_endian = \"little\", target_feature = \"sse2\", "
            "target_has_atomic_load_store, "
            "target_has_atomic = \"64\", "
            "target_has_atomic_equal_alignment = \"64\", "
            "target_has_atomic_load_store = \"64\")")) {
        fail("x86-64", "target facts differ");
    }
}

static void test_i386_and_i686(void)
{
    const CmTargetDesc *target;
    CmCfgSet cfg;

    target = cm_target_find("i386-unknown-linux-musl");
    if (!cm_target_cfg_set(&cfg, target)
        || !evaluates(&cfg, "all(unix, "
            "target_arch = \"x86\", target_env = \"musl\", "
            "target_pointer_width = \"32\", "
            "target_has_atomic = \"32\", "
            "not(target_has_atomic = \"64\"), "
            "not(target_feature = \"sse2\"))")) {
        fail("i386", "conservative i386 cfg facts differ");
    }
    target = cm_target_find("i686-unknown-linux-musl");
    if (!cm_target_cfg_set(&cfg, target)
        || !evaluates(&cfg, "all(target_feature = \"sse2\", "
            "target_has_atomic = \"64\", "
            "not(target_has_atomic_equal_alignment = \"64\"), "
            "target_has_atomic_load_store = \"64\")")) {
        fail("i686", "i686 feature or atomic cfg facts differ");
    }
}

static void test_invalid_descriptor(void)
{
    const CmTargetDesc *known;
    CmTargetDesc invalid;
    CmCfgSet cfg;

    known = cm_target_find("x86_64-unknown-linux-gnu");
    if (known == NULL) {
        fail("invalid", "known descriptor is missing");
        return;
    }
    invalid = *known;
    if (!cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "an exact copied descriptor was not canonical");
    }
    invalid = *known;
    invalid.pointer_bits = 48u;
    if (cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "unsupported pointer width produced cfg facts");
    }
    invalid = *known;
    invalid.vendor = NULL;
    if (cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "incomplete target produced cfg facts");
    }
    invalid = *known;
    invalid.vendor = "apple";
    if (cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "contradictory target vendor produced cfg facts");
    }
    invalid = *known;
    invalid.target_features = NULL;
    invalid.target_feature_count = 1u;
    if (cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "missing target-feature storage produced cfg facts");
    }
    invalid = *known;
    invalid.cfg_entries = NULL;
    invalid.cfg_entry_count = 1u;
    if (cm_target_cfg_set(&cfg, &invalid)) {
        fail("invalid", "missing target-cfg storage produced cfg facts");
    }
}

int main(void)
{
    test_x86_64();
    test_i386_and_i686();
    test_invalid_descriptor();
    if (failures != 0) {
        fprintf(stderr, "target cfg tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("target cfg tests: ok");
    return 0;
}
