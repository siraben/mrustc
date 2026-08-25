#include "cm/hir/artifact_identity.h"

#include <string.h>

typedef char CmHirArtifactIdentityMustBeSha256[
    CM_HIR_ARTIFACT_IDENTITY_SIZE == CM_SHA256_DIGEST_SIZE ? 1 : -1];

static const unsigned char cm_hir_artifact_identity_domain[] = {
    'c', 'm', 'r', 'u', 's', 't', 'c', '-', 'g', '3', '-',
    'a', 'r', 't', 'i', 'f', 'a', 'c', 't', '-',
    'i', 'd', 'e', 'n', 't', 'i', 't', 'y', '-', 'v', '1'
};

static const unsigned char cm_hir_source_closure_domain[] = {
    'c', 'm', 'r', 'u', 's', 't', 'c', '-', 'g', '3', '-',
    's', 'o', 'u', 'r', 'c', 'e', '-', 'c', 'l', 'o', 's', 'u', 'r', 'e',
    '-', 'v', '1'
};

static int cm_hir_artifact_bytes_valid(CmHirArtifactBytes value,
    size_t minimum, size_t maximum)
{
    return value.length >= minimum && value.length <= maximum
        && (value.data != NULL || value.length == 0u);
}

static int cm_hir_artifact_bytes_compare(CmHirArtifactBytes left,
    CmHirArtifactBytes right)
{
    size_t common;
    int order;

    common = left.length < right.length ? left.length : right.length;
    order = common == 0u ? 0 : memcmp(left.data, right.data, common);
    if (order != 0) {
        return order;
    }
    if (left.length < right.length) {
        return -1;
    }
    if (left.length > right.length) {
        return 1;
    }
    return 0;
}

static void cm_hir_artifact_hash_u64(CmSha256 *context, uint64_t value)
{
    unsigned char bytes[8];
    size_t index;

    for (index = 0u; index < sizeof(bytes); index += 1u) {
        bytes[sizeof(bytes) - index - 1u]
            = (unsigned char)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    cm_sha256_update(context, bytes, sizeof(bytes));
}

static void cm_hir_artifact_hash_field(CmSha256 *context,
    unsigned char tag, const void *data, size_t length)
{
    cm_sha256_update(context, &tag, 1u);
    cm_hir_artifact_hash_u64(context, (uint64_t)length);
    if (length != 0u) {
        cm_sha256_update(context, data, length);
    }
}

static void cm_hir_artifact_hash_u32_field(CmSha256 *context,
    unsigned char tag, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24u) & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 16u) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 8u) & UINT32_C(0xff));
    bytes[3] = (unsigned char)(value & UINT32_C(0xff));
    cm_hir_artifact_hash_field(context, tag, bytes, sizeof(bytes));
}

static void cm_hir_artifact_hash_count(CmSha256 *context,
    unsigned char tag, size_t count)
{
    unsigned char bytes[8];
    uint64_t value;
    size_t index;

    value = (uint64_t)count;
    for (index = 0u; index < sizeof(bytes); index += 1u) {
        bytes[sizeof(bytes) - index - 1u]
            = (unsigned char)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    cm_hir_artifact_hash_field(context, tag, bytes, sizeof(bytes));
}

static int cm_hir_artifact_source_path_valid(CmHirArtifactBytes path)
{
    const unsigned char *bytes;
    size_t segment_start;
    size_t index;
    size_t segment_length;

    if (!cm_hir_artifact_bytes_valid(path, 1u,
            CM_HIR_ARTIFACT_MAX_SOURCE_PATH_SIZE)) {
        return 0;
    }
    bytes = (const unsigned char *)path.data;
    if (bytes[0] == (unsigned char)'/'
        || bytes[path.length - 1u] == (unsigned char)'/'
        || (path.length >= 2u
            && ((bytes[0] >= (unsigned char)'A'
                    && bytes[0] <= (unsigned char)'Z')
                || (bytes[0] >= (unsigned char)'a'
                    && bytes[0] <= (unsigned char)'z'))
            && bytes[1] == (unsigned char)':')) {
        return 0;
    }

    segment_start = 0u;
    for (index = 0u; index <= path.length; index += 1u) {
        if (index != path.length && bytes[index] != (unsigned char)'/') {
            if (bytes[index] == (unsigned char)'\\'
                || bytes[index] == (unsigned char)'\0') {
                return 0;
            }
            continue;
        }
        segment_length = index - segment_start;
        if (segment_length == 0u
            || (segment_length == 1u
                && bytes[segment_start] == (unsigned char)'.')
            || (segment_length == 2u
                && bytes[segment_start] == (unsigned char)'.'
                && bytes[segment_start + 1u] == (unsigned char)'.')) {
            return 0;
        }
        segment_start = index + 1u;
    }
    return 1;
}

CmHirArtifactIdentityStatus cm_hir_artifact_source_closure_digest(
    const CmHirArtifactSourceEntry *sources, size_t source_count,
    CmHirArtifactDigest *out_digest)
{
    CmSha256 context;
    CmHirArtifactDigest candidate;
    size_t total_size;
    size_t index;

    if (out_digest == NULL || sources == NULL || source_count == 0u) {
        return CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT;
    }
    if (source_count > CM_HIR_ARTIFACT_MAX_SOURCE_COUNT) {
        return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
    }

    total_size = 0u;
    for (index = 0u; index < source_count; index += 1u) {
        if (sources[index].logical_path.length
            > CM_HIR_ARTIFACT_MAX_SOURCE_PATH_SIZE) {
            return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
        }
        if (!cm_hir_artifact_source_path_valid(sources[index].logical_path)) {
            return CM_HIR_ARTIFACT_IDENTITY_INVALID_SOURCE_PATH;
        }
        if (!cm_hir_artifact_bytes_valid(sources[index].contents, 0u,
                CM_HIR_ARTIFACT_MAX_SOURCE_FILE_SIZE)) {
            if (sources[index].contents.length
                > CM_HIR_ARTIFACT_MAX_SOURCE_FILE_SIZE) {
                return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
            }
            return CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT;
        }
        if (index != 0u
            && cm_hir_artifact_bytes_compare(
                sources[index - 1u].logical_path,
                sources[index].logical_path) >= 0) {
            return CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES;
        }
        if (sources[index].contents.length
            > CM_HIR_ARTIFACT_MAX_SOURCE_CLOSURE_SIZE - total_size) {
            return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
        }
        total_size += sources[index].contents.length;
    }

    cm_sha256_init(&context);
    cm_hir_artifact_hash_field(&context, (unsigned char)'D',
        cm_hir_source_closure_domain, sizeof(cm_hir_source_closure_domain));
    cm_hir_artifact_hash_count(&context, (unsigned char)'N', source_count);
    for (index = 0u; index < source_count; index += 1u) {
        cm_hir_artifact_hash_field(&context, (unsigned char)'P',
            sources[index].logical_path.data,
            sources[index].logical_path.length);
        cm_hir_artifact_hash_field(&context, (unsigned char)'B',
            sources[index].contents.data, sources[index].contents.length);
    }
    cm_sha256_final(&context, candidate.bytes);
    *out_digest = candidate;
    return CM_HIR_ARTIFACT_IDENTITY_OK;
}

CmHirArtifactIdentityStatus cm_hir_artifact_identity_compute(
    const CmHirArtifactIdentityInput *input,
    CmHirArtifactDigest *out_identity)
{
    CmSha256 context;
    CmHirArtifactDigest candidate;
    size_t index;

    if (input == NULL || out_identity == NULL) {
        return CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT;
    }
    if (input->schema_major == 0u || input->profile == 0u
        || input->edition == 0u
        || input->crate_name.length == 0u
        || input->crate_disambiguator.length == 0u
        || input->target_descriptor.length == 0u
        || input->panic_strategy.length == 0u
        || input->crate_name.data == NULL
        || input->crate_disambiguator.data == NULL
        || input->target_descriptor.data == NULL
        || input->panic_strategy.data == NULL
        || (input->cfgs == NULL && input->cfg_count != 0u)
        || (input->dependency_identities == NULL
            && input->dependency_count != 0u)) {
        return CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT;
    }
    if (input->crate_name.length > CM_HIR_ARTIFACT_MAX_NAME_SIZE
        || input->crate_disambiguator.length
            > CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE
        || input->target_descriptor.length
            > CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE
        || input->panic_strategy.length > CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE
        || input->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || input->dependency_count > CM_HIR_ARTIFACT_MAX_DEPENDENCY_COUNT) {
        return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
    }
    for (index = 0u; index < input->cfg_count; index += 1u) {
        if (!cm_hir_artifact_bytes_valid(input->cfgs[index], 1u,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE)) {
            if (input->cfgs[index].length > CM_HIR_ARTIFACT_MAX_CFG_SIZE) {
                return CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED;
            }
            return CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT;
        }
        if (index != 0u
            && cm_hir_artifact_bytes_compare(input->cfgs[index - 1u],
                input->cfgs[index]) >= 0) {
            return CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_CFG;
        }
    }
    for (index = 1u; index < input->dependency_count; index += 1u) {
        if (memcmp(input->dependency_identities[index - 1u].bytes,
                input->dependency_identities[index].bytes,
                CM_HIR_ARTIFACT_IDENTITY_SIZE) >= 0) {
            return CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_DEPENDENCIES;
        }
    }

    cm_sha256_init(&context);
    cm_hir_artifact_hash_field(&context, (unsigned char)'D',
        cm_hir_artifact_identity_domain,
        sizeof(cm_hir_artifact_identity_domain));
    cm_hir_artifact_hash_u32_field(&context, (unsigned char)'A',
        input->schema_major);
    cm_hir_artifact_hash_u32_field(&context, (unsigned char)'a',
        input->schema_minor);
    cm_hir_artifact_hash_u32_field(&context, (unsigned char)'P',
        input->profile);
    cm_hir_artifact_hash_field(&context, (unsigned char)'C',
        input->crate_name.data, input->crate_name.length);
    cm_hir_artifact_hash_field(&context, (unsigned char)'I',
        input->crate_disambiguator.data, input->crate_disambiguator.length);
    cm_hir_artifact_hash_u32_field(&context, (unsigned char)'E',
        input->edition);
    cm_hir_artifact_hash_field(&context, (unsigned char)'T',
        input->target_descriptor.data, input->target_descriptor.length);
    cm_hir_artifact_hash_field(&context, (unsigned char)'R',
        input->panic_strategy.data, input->panic_strategy.length);
    cm_hir_artifact_hash_count(&context, (unsigned char)'F',
        input->cfg_count);
    for (index = 0u; index < input->cfg_count; index += 1u) {
        cm_hir_artifact_hash_field(&context, (unsigned char)'f',
            input->cfgs[index].data, input->cfgs[index].length);
    }
    cm_hir_artifact_hash_field(&context, (unsigned char)'S',
        input->source_closure.bytes, CM_HIR_ARTIFACT_IDENTITY_SIZE);
    cm_hir_artifact_hash_count(&context, (unsigned char)'N',
        input->dependency_count);
    for (index = 0u; index < input->dependency_count; index += 1u) {
        cm_hir_artifact_hash_field(&context, (unsigned char)'n',
            input->dependency_identities[index].bytes,
            CM_HIR_ARTIFACT_IDENTITY_SIZE);
    }
    cm_sha256_final(&context, candidate.bytes);
    *out_identity = candidate;
    return CM_HIR_ARTIFACT_IDENTITY_OK;
}

const char *cm_hir_artifact_identity_status_name(
    CmHirArtifactIdentityStatus status)
{
    switch (status) {
    case CM_HIR_ARTIFACT_IDENTITY_OK:
        return "ok";
    case CM_HIR_ARTIFACT_IDENTITY_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_ARTIFACT_IDENTITY_LIMIT_EXCEEDED:
        return "limit exceeded";
    case CM_HIR_ARTIFACT_IDENTITY_INVALID_SOURCE_PATH:
        return "invalid source path";
    case CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_CFG:
        return "noncanonical cfg";
    case CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_DEPENDENCIES:
        return "noncanonical dependencies";
    case CM_HIR_ARTIFACT_IDENTITY_NONCANONICAL_SOURCES:
        return "noncanonical sources";
    }
    return "unknown artifact identity status";
}
