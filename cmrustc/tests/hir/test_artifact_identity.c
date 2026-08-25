#include "cm/hir/artifact_identity.h"

#include <assert.h>
#include <string.h>

static CmHirArtifactBytes test_bytes(const char *text)
{
    CmHirArtifactBytes bytes;

    bytes.data = text;
    bytes.length = strlen(text);
    return bytes;
}

static void test_fill_digest(CmHirArtifactDigest *digest,
    unsigned char first)
{
    size_t index;

    for (index = 0u; index < sizeof(digest->bytes); index += 1u) {
        digest->bytes[index] = (unsigned char)(first + (unsigned char)index);
    }
}

static void test_expect_sentinel(const CmHirArtifactDigest *digest,
    unsigned char value)
{
    size_t index;

    for (index = 0u; index < sizeof(digest->bytes); index += 1u) {
        assert(digest->bytes[index] == value);
    }
}

static void test_source_closure_and_twin_roots(CmHirArtifactDigest *out)
{
    static const char root_a_lib[] = "pub mod support;\n";
    static const char root_a_support[] = "pub fn answer() -> u32 { 42 }\n";
    static const char root_b_lib[] = "pub mod support;\n";
    static const char root_b_support[] = "pub fn answer() -> u32 { 42 }\n";
    CmHirArtifactSourceEntry root_a[2];
    CmHirArtifactSourceEntry root_b[2];
    CmHirArtifactDigest twin;
    CmHirArtifactDigest changed;
    CmHirArtifactSourceEntry framed_a;
    CmHirArtifactSourceEntry framed_b;
    CmHirArtifactDigest framed_a_digest;
    CmHirArtifactDigest framed_b_digest;

    /* The physical root names are deliberately not inputs to this API. */
    root_a[0].logical_path = test_bytes("src/lib.rs");
    root_a[0].contents = test_bytes(root_a_lib);
    root_a[1].logical_path = test_bytes("src/support.rs");
    root_a[1].contents = test_bytes(root_a_support);
    root_b[0].logical_path = test_bytes("src/lib.rs");
    root_b[0].contents = test_bytes(root_b_lib);
    root_b[1].logical_path = test_bytes("src/support.rs");
    root_b[1].contents = test_bytes(root_b_support);

    assert(cm_hir_artifact_source_closure_digest(root_a, 2u, out)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(cm_hir_artifact_source_closure_digest(root_b, 2u, &twin)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(memcmp(out->bytes, twin.bytes, sizeof(out->bytes)) == 0);

    root_b[1].contents = test_bytes("pub fn answer() -> u32 { 43 }\n");
    assert(cm_hir_artifact_source_closure_digest(root_b, 2u, &changed)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(memcmp(out->bytes, changed.bytes, sizeof(out->bytes)) != 0);

    framed_a.logical_path = test_bytes("a");
    framed_a.contents = test_bytes("bc");
    framed_b.logical_path = test_bytes("ab");
    framed_b.contents = test_bytes("c");
    assert(cm_hir_artifact_source_closure_digest(&framed_a, 1u,
        &framed_a_digest) == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(cm_hir_artifact_source_closure_digest(&framed_b, 1u,
        &framed_b_digest) == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(memcmp(framed_a_digest.bytes, framed_b_digest.bytes,
        sizeof(framed_a_digest.bytes)) != 0);
}

static void test_source_rejections(void)
{
    static const char byte = 'x';
    static const char *const invalid_paths[] = {
        "/src/lib.rs",
        "src//lib.rs",
        "src/./lib.rs",
        "src/../lib.rs",
        "src/lib.rs/",
        "src\\lib.rs",
        "C:\\src\\lib.rs"
    };
    CmHirArtifactSourceEntry source;
    CmHirArtifactSourceEntry pair[2];
    CmHirArtifactSourceEntry total[5];
    CmHirArtifactDigest digest;
    size_t index;

    memset(&digest, 0xa5, sizeof(digest));
    assert(cm_hir_artifact_source_closure_digest(NULL, 0u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT);
    test_expect_sentinel(&digest, UINT8_C(0xa5));

    source.contents = test_bytes("");
    for (index = 0u;
        index < sizeof(invalid_paths) / sizeof(invalid_paths[0]);
        index += 1u) {
        source.logical_path = test_bytes(invalid_paths[index]);
        assert(cm_hir_artifact_source_closure_digest(&source, 1u, &digest)
            == CM_HIR_ARTIFACT_IDENTITY_INVALID_SOURCE_PATH);
        test_expect_sentinel(&digest, UINT8_C(0xa5));
    }

    source.logical_path.data = &byte;
    source.logical_path.length = CM_HIR_ARTIFACT_MAX_SOURCE_PATH_SIZE + 1u;
    assert(cm_hir_artifact_source_closure_digest(&source, 1u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    test_expect_sentinel(&digest, UINT8_C(0xa5));

    source.logical_path = test_bytes("src/lib.rs");
    source.contents.data = NULL;
    source.contents.length = 1u;
    assert(cm_hir_artifact_source_closure_digest(&source, 1u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT);
    source.contents.data = &byte;
    source.contents.length = CM_HIR_ARTIFACT_MAX_SOURCE_FILE_SIZE + 1u;
    assert(cm_hir_artifact_source_closure_digest(&source, 1u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);

    pair[0].logical_path = test_bytes("src/z.rs");
    pair[0].contents = test_bytes("");
    pair[1].logical_path = test_bytes("src/a.rs");
    pair[1].contents = test_bytes("");
    assert(cm_hir_artifact_source_closure_digest(pair, 2u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES);
    pair[1].logical_path = pair[0].logical_path;
    assert(cm_hir_artifact_source_closure_digest(pair, 2u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES);

    for (index = 0u; index < 5u; index += 1u) {
        static const char *const paths[5] = {
            "a", "b", "c", "d", "e"
        };

        total[index].logical_path = test_bytes(paths[index]);
        total[index].contents.data = &byte;
        total[index].contents.length = CM_HIR_ARTIFACT_MAX_SOURCE_FILE_SIZE;
    }
    assert(cm_hir_artifact_source_closure_digest(total, 5u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    test_expect_sentinel(&digest, UINT8_C(0xa5));

    assert(cm_hir_artifact_source_closure_digest(&source,
            CM_HIR_ARTIFACT_MAX_SOURCE_COUNT + 1u, &digest)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
}

static CmHirArtifactIdentityInput test_baseline_input(
    CmHirArtifactDigest source_closure, CmHirArtifactBytes cfgs[2],
    CmHirArtifactDigest dependencies[2])
{
    CmHirArtifactIdentityInput input;

    memset(&input, 0, sizeof(input));
    input.schema_major = UINT32_C(3);
    input.schema_minor = UINT32_C(2);
    input.profile = UINT32_C(1);
    input.crate_name = test_bytes("g3_producer");
    input.crate_disambiguator = test_bytes("fixture-0001");
    input.edition = UINT32_C(2021);
    input.target_descriptor = test_bytes(
        "arch=x86_64;os=linux;env=gnu;vendor=unknown;pointer_width=64;"
        "endian=little");
    input.panic_strategy = test_bytes("abort");
    cfgs[0] = test_bytes("debug_assertions");
    cfgs[1] = test_bytes("target_arch=\"x86_64\"");
    input.cfgs = cfgs;
    input.cfg_count = 2u;
    input.source_closure = source_closure;
    test_fill_digest(&input.link_manifest, UINT8_C(0xc0));
    test_fill_digest(&dependencies[0], UINT8_C(0x10));
    test_fill_digest(&dependencies[1], UINT8_C(0x80));
    input.dependency_identities = dependencies;
    input.dependency_count = 2u;
    return input;
}

static void test_expect_changed(const CmHirArtifactDigest *baseline,
    const CmHirArtifactIdentityInput *input)
{
    CmHirArtifactDigest changed;

    assert(cm_hir_artifact_identity_compute(input, &changed)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(memcmp(baseline->bytes, changed.bytes,
        sizeof(baseline->bytes)) != 0);
}

static void test_every_identity_input_changes_identity(
    CmHirArtifactDigest source_closure)
{
    static const unsigned char expected[CM_HIR_ARTIFACT_IDENTITY_SIZE] = {
        UINT8_C(0x48), UINT8_C(0xa5), UINT8_C(0xd6), UINT8_C(0x1c),
        UINT8_C(0xf6), UINT8_C(0xe6), UINT8_C(0x17), UINT8_C(0x17),
        UINT8_C(0xfa), UINT8_C(0x1b), UINT8_C(0x45), UINT8_C(0x58),
        UINT8_C(0xee), UINT8_C(0xe1), UINT8_C(0xf1), UINT8_C(0x7b),
        UINT8_C(0xef), UINT8_C(0x9c), UINT8_C(0x4b), UINT8_C(0x58),
        UINT8_C(0x5c), UINT8_C(0x24), UINT8_C(0xaf), UINT8_C(0x60),
        UINT8_C(0x9b), UINT8_C(0xcd), UINT8_C(0x27), UINT8_C(0xcd),
        UINT8_C(0xee), UINT8_C(0x76), UINT8_C(0x4a), UINT8_C(0x63)
    };
    CmHirArtifactBytes cfgs[2];
    CmHirArtifactDigest dependencies[2];
    CmHirArtifactIdentityInput baseline_input;
    CmHirArtifactIdentityInput changed_input;
    CmHirArtifactDigest baseline;
    CmHirArtifactDigest repeated;
    CmHirArtifactBytes changed_cfgs[2];
    CmHirArtifactDigest changed_dependencies[2];

    baseline_input = test_baseline_input(source_closure, cfgs, dependencies);
    assert(cm_hir_artifact_identity_compute(&baseline_input, &baseline)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(cm_hir_artifact_identity_compute(&baseline_input, &repeated)
        == CM_HIR_ARTIFACT_IDENTITY_OK);
    assert(memcmp(baseline.bytes, repeated.bytes,
        sizeof(baseline.bytes)) == 0);
    assert(memcmp(baseline.bytes, expected, sizeof(expected)) == 0);

    changed_input = baseline_input;
    changed_input.schema_major += 1u;
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.schema_minor += 1u;
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.profile += 1u;
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.crate_name = test_bytes("g3_producer_changed");
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.crate_disambiguator = test_bytes("fixture-0002");
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.edition = UINT32_C(2018);
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.target_descriptor = test_bytes(
        "arch=aarch64;os=linux;env=gnu;vendor=unknown;pointer_width=64;"
        "endian=little");
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.panic_strategy = test_bytes("unwind");
    test_expect_changed(&baseline, &changed_input);

    changed_cfgs[0] = cfgs[0];
    changed_cfgs[1] = test_bytes("target_arch=\"aarch64\"");
    changed_input = baseline_input;
    changed_input.cfgs = changed_cfgs;
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.cfg_count = 1u;
    test_expect_changed(&baseline, &changed_input);

    changed_input = baseline_input;
    changed_input.source_closure.bytes[0] ^= UINT8_C(1);
    test_expect_changed(&baseline, &changed_input);

    changed_input = baseline_input;
    changed_input.link_manifest.bytes[0] ^= UINT8_C(1);
    test_expect_changed(&baseline, &changed_input);

    changed_dependencies[0] = dependencies[0];
    changed_dependencies[1] = dependencies[1];
    changed_dependencies[0].bytes[0] += UINT8_C(1);
    changed_input = baseline_input;
    changed_input.dependency_identities = changed_dependencies;
    test_expect_changed(&baseline, &changed_input);
    changed_input = baseline_input;
    changed_input.dependency_count = 1u;
    test_expect_changed(&baseline, &changed_input);
}

static void test_identity_rejections(CmHirArtifactDigest source_closure)
{
    static const char byte = 'x';
    CmHirArtifactBytes cfgs[2];
    CmHirArtifactDigest dependencies[2];
    CmHirArtifactIdentityInput input;
    CmHirArtifactDigest output;

    input = test_baseline_input(source_closure, cfgs, dependencies);
    memset(&output, 0xa5, sizeof(output));
    assert(cm_hir_artifact_identity_compute(NULL, &output)
        == CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT);
    assert(cm_hir_artifact_identity_compute(&input, NULL)
        == CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT);

    input.cfgs = NULL;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT);
    test_expect_sentinel(&output, UINT8_C(0xa5));

    input = test_baseline_input(source_closure, cfgs, dependencies);
    cfgs[0] = test_bytes("z");
    cfgs[1] = test_bytes("a");
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_CFG);
    cfgs[1] = cfgs[0];
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_CFG);

    input = test_baseline_input(source_closure, cfgs, dependencies);
    dependencies[0].bytes[0] = UINT8_C(0xf0);
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_DEPENDENCIES);
    dependencies[1] = dependencies[0];
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_DEPENDENCIES);

    input = test_baseline_input(source_closure, cfgs, dependencies);
    input.crate_name.data = &byte;
    input.crate_name.length = CM_HIR_ARTIFACT_MAX_NAME_SIZE + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    input = test_baseline_input(source_closure, cfgs, dependencies);
    input.crate_disambiguator.data = &byte;
    input.crate_disambiguator.length
        = CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    input = test_baseline_input(source_closure, cfgs, dependencies);
    input.target_descriptor.data = &byte;
    input.target_descriptor.length = CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    input = test_baseline_input(source_closure, cfgs, dependencies);
    cfgs[0].data = &byte;
    cfgs[0].length = CM_HIR_ARTIFACT_MAX_CFG_SIZE + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    input = test_baseline_input(source_closure, cfgs, dependencies);
    input.cfg_count = CM_HIR_ARTIFACT_MAX_CFG_COUNT + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    input = test_baseline_input(source_closure, cfgs, dependencies);
    input.dependency_count = CM_HIR_ARTIFACT_MAX_DEPENDENCY_COUNT + 1u;
    assert(cm_hir_artifact_identity_compute(&input, &output)
        == CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED);
    test_expect_sentinel(&output, UINT8_C(0xa5));
}

static void test_status_names(void)
{
    assert(strcmp(cm_hir_artifact_identity_status_name(
        CM_HIR_ARTIFACT_IDENTITY_OK), "ok") == 0);
    assert(strcmp(cm_hir_artifact_identity_status_name(
        CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES),
        "noncanonical sources") == 0);
    assert(strcmp(cm_hir_artifact_identity_status_name(
        (CmHirArtifactIdentityStatus)999),
        "unknown artifact identity status") == 0);
}

int main(void)
{
    CmHirArtifactDigest source_closure;

    test_source_closure_and_twin_roots(&source_closure);
    test_source_rejections();
    test_every_identity_input_changes_identity(source_closure);
    test_identity_rejections(source_closure);
    test_status_names();
    return 0;
}
