#include "cm/hir/executable_rlib.h"

#include "cm/sha256.h"

#include <string.h>

static CmHirExecutableRlibStatus cm_exec_rlib_archive_status(
    CmRlibStatus status)
{
    if (status == CM_RLIB_LIMIT_EXCEEDED
        || status == CM_RLIB_TOO_MANY_MEMBERS)
        return CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED;
    if (status == CM_RLIB_INVALID_ARGUMENT)
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    return CM_HIR_EXECUTABLE_RLIB_INVALID_ARCHIVE;
}

static CmHirExecutableRlibStatus cm_exec_rlib_metadata_status(
    CmHirExecutableMetadataStatus status)
{
    switch (status) {
    case CM_HIR_EXEC_METADATA_OK:
        return CM_HIR_EXECUTABLE_RLIB_OK;
    case CM_HIR_EXEC_METADATA_INVALID_ARGUMENT:
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    case CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED:
        return CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED;
    case CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR:
        return CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR;
    case CM_HIR_EXEC_METADATA_INVALID_FORMAT:
        return CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA;
    case CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH:
        return CM_HIR_EXECUTABLE_RLIB_IDENTITY_MISMATCH;
    }
    return CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA;
}

static int cm_exec_rlib_declared_name_valid(
    const CmHirExecutableString *name)
{
    size_t index;

    if (name == NULL || name->data == NULL || name->length == 0u
        || name->length > CM_RLIB_MAX_MEMBER_NAME)
        return 0;
    if (name->length == sizeof(CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER) - 1u
        && memcmp(name->data, CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER,
            name->length) == 0)
        return 0;
    for (index = 0u; index < name->length; index += 1u) {
        unsigned char byte = name->data[index];
        if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
                || (byte >= (unsigned char)'a'
                    && byte <= (unsigned char)'z')
                || (byte >= (unsigned char)'0'
                    && byte <= (unsigned char)'9')
                || byte == (unsigned char)'_'
                || byte == (unsigned char)'.'
                || byte == (unsigned char)'-'))
            return 0;
    }
    return 1;
}

static int cm_exec_rlib_name_equal(const CmHirExecutableString *declared,
    const char *archive_name)
{
    size_t length;

    length = strlen(archive_name);
    return declared->length == length
        && (length == 0u
            || memcmp(declared->data, archive_name, length) == 0);
}

static void cm_exec_rlib_copy_name(
    char destination[CM_RLIB_MAX_MEMBER_NAME + 1u],
    const CmHirExecutableString *source)
{
    memcpy(destination, source->data, source->length);
    destination[source->length] = '\0';
}

static int cm_exec_rlib_digest_matches(const unsigned char *data,
    size_t length, const CmHirArtifactDigest *expected)
{
    CmSha256 sha;
    unsigned char digest[CM_SHA256_DIGEST_SIZE];

    cm_sha256_init(&sha);
    cm_sha256_update(&sha, data, length);
    cm_sha256_final(&sha, digest);
    return memcmp(digest, expected->bytes, sizeof(digest)) == 0;
}

void cm_hir_executable_rlib_init(CmHirExecutableRlib *rlib)
{
    if (rlib == NULL) return;
    memset(rlib, 0, sizeof(*rlib));
    cm_hir_executable_metadata_init(&rlib->metadata);
}

void cm_hir_executable_rlib_destroy(CmHirExecutableRlib *rlib)
{
    if (rlib == NULL) return;
    cm_hir_executable_metadata_destroy(&rlib->metadata);
    memset(rlib, 0, sizeof(*rlib));
    cm_hir_executable_metadata_init(&rlib->metadata);
}

CmHirExecutableRlibStatus cm_hir_executable_rlib_encode(
    const CmHirExecutableMetadata *metadata, CmByteBuf *output)
{
    CmByteBuf encoded_metadata;
    CmRlibMember members[CM_RLIB_MAX_MEMBER_COUNT];
    char object_names[CM_HIR_EXECUTABLE_RLIB_MAX_OBJECTS]
        [CM_RLIB_MAX_MEMBER_NAME + 1u];
    CmHirExecutableMetadataStatus metadata_status;
    CmRlibStatus archive_status;
    size_t object_index;
    size_t member_index;
    int metadata_inserted;

    if (metadata == NULL || output == NULL)
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    if (metadata->object_count > CM_HIR_EXECUTABLE_RLIB_MAX_OBJECTS)
        return CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED;
    for (object_index = 0u; object_index < metadata->object_count;
        object_index += 1u) {
        if (metadata->objects == NULL
            || !cm_exec_rlib_declared_name_valid(
                &metadata->objects[object_index].archive_member_name))
            return CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR;
    }

    cm_byte_buf_init(&encoded_metadata);
    metadata_status = cm_hir_executable_metadata_encode(metadata,
        &encoded_metadata);
    if (metadata_status != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&encoded_metadata);
        return cm_exec_rlib_metadata_status(metadata_status);
    }
    if (encoded_metadata.len > CM_RLIB_MAX_METADATA_SIZE) {
        cm_byte_buf_destroy(&encoded_metadata);
        return CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED;
    }

    object_index = 0u;
    member_index = 0u;
    metadata_inserted = 0;
    while (object_index < metadata->object_count) {
        const CmHirExecutableLinkObject *object
            = &metadata->objects[object_index];
        cm_exec_rlib_copy_name(object_names[object_index],
            &object->archive_member_name);
        if (!metadata_inserted
            && strcmp(CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER,
                object_names[object_index]) < 0) {
            members[member_index].name
                = CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER;
            members[member_index].data = encoded_metadata.data;
            members[member_index].length = encoded_metadata.len;
            member_index += 1u;
            metadata_inserted = 1;
        }
        members[member_index].name = object_names[object_index];
        members[member_index].data = object->object_bytes;
        members[member_index].length = object->object_bytes_length;
        member_index += 1u;
        object_index += 1u;
    }
    if (!metadata_inserted) {
        members[member_index].name = CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER;
        members[member_index].data = encoded_metadata.data;
        members[member_index].length = encoded_metadata.len;
        member_index += 1u;
    }

    archive_status = cm_rlib_encode_members(output, members, member_index);
    cm_byte_buf_destroy(&encoded_metadata);
    if (archive_status != CM_RLIB_OK)
        return cm_exec_rlib_archive_status(archive_status);
    return CM_HIR_EXECUTABLE_RLIB_OK;
}

static CmHirExecutableRlibStatus cm_exec_rlib_decode(
    const void *archive, size_t archive_length,
    const CmHirExecutableMetadataExpectation *expectation,
    const CmHirArtifactConfig *configuration,
    CmHirExecutableRlib *output)
{
    CmRlibArchiveView archive_view;
    CmRlibMemberView metadata_member;
    CmHirExecutableRlib candidate;
    CmHirExecutableMetadataStatus metadata_status;
    CmRlibStatus archive_status;
    size_t archive_index;
    size_t object_index;

    if (output == NULL || archive == NULL
        || ((expectation == NULL) == (configuration == NULL)))
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    archive_status = cm_rlib_decode_members(archive, archive_length,
        &archive_view);
    if (archive_status != CM_RLIB_OK)
        return cm_exec_rlib_archive_status(archive_status);
    archive_status = cm_rlib_find_member(&archive_view,
        CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER, &metadata_member);
    if (archive_status != CM_RLIB_OK)
        return CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH;
    if (metadata_member.length > CM_RLIB_MAX_METADATA_SIZE)
        return CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED;

    cm_hir_executable_rlib_init(&candidate);
    metadata_status = expectation != NULL
        ? cm_hir_executable_metadata_decode(metadata_member.data,
            metadata_member.length, expectation, &candidate.metadata)
        : cm_hir_executable_metadata_decode_configured(metadata_member.data,
            metadata_member.length, configuration, &candidate.metadata);
    if (metadata_status != CM_HIR_EXEC_METADATA_OK) {
        cm_hir_executable_rlib_destroy(&candidate);
        return cm_exec_rlib_metadata_status(metadata_status);
    }
    if (candidate.metadata.object_count
            > CM_HIR_EXECUTABLE_RLIB_MAX_OBJECTS
        || archive_view.member_count
            != candidate.metadata.object_count + 1u) {
        cm_hir_executable_rlib_destroy(&candidate);
        return CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH;
    }

    archive_index = 0u;
    for (object_index = 0u;
        object_index < candidate.metadata.object_count;
        object_index += 1u) {
        const CmHirExecutableLinkObject *object
            = &candidate.metadata.objects[object_index];
        const CmRlibMemberView *member;

        if (!cm_exec_rlib_declared_name_valid(
                &object->archive_member_name)) {
            cm_hir_executable_rlib_destroy(&candidate);
            return CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA;
        }
        while (archive_index < archive_view.member_count
            && strcmp(archive_view.members[archive_index].name,
                CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER) == 0)
            archive_index += 1u;
        if (archive_index == archive_view.member_count) {
            cm_hir_executable_rlib_destroy(&candidate);
            return CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH;
        }
        member = &archive_view.members[archive_index];
        if (!cm_exec_rlib_name_equal(&object->archive_member_name,
                member->name)) {
            cm_hir_executable_rlib_destroy(&candidate);
            return CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH;
        }
        if (object->byte_length != (uint64_t)member->length
            || !cm_exec_rlib_digest_matches(member->data, member->length,
                &object->object_digest)) {
            cm_hir_executable_rlib_destroy(&candidate);
            return CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH;
        }
        cm_exec_rlib_copy_name(
            candidate.objects[object_index].archive_member_name,
            &object->archive_member_name);
        candidate.objects[object_index].data = member->data;
        candidate.objects[object_index].length = member->length;
        archive_index += 1u;
    }
    while (archive_index < archive_view.member_count
        && strcmp(archive_view.members[archive_index].name,
            CM_HIR_EXECUTABLE_RLIB_METADATA_MEMBER) == 0)
        archive_index += 1u;
    if (archive_index != archive_view.member_count) {
        cm_hir_executable_rlib_destroy(&candidate);
        return CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH;
    }
    candidate.object_count = candidate.metadata.object_count;

    cm_hir_executable_rlib_destroy(output);
    *output = candidate;
    return CM_HIR_EXECUTABLE_RLIB_OK;
}

CmHirExecutableRlibStatus cm_hir_executable_rlib_decode(
    const void *archive, size_t archive_length,
    const CmHirExecutableMetadataExpectation *expectation,
    CmHirExecutableRlib *output)
{
    if (expectation == NULL)
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    return cm_exec_rlib_decode(archive, archive_length, expectation, NULL,
        output);
}

CmHirExecutableRlibStatus cm_hir_executable_rlib_decode_configured(
    const void *archive, size_t archive_length,
    const CmHirArtifactConfig *configuration,
    CmHirExecutableRlib *output)
{
    if (configuration == NULL)
        return CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT;
    return cm_exec_rlib_decode(archive, archive_length, NULL, configuration,
        output);
}

const char *cm_hir_executable_rlib_status_name(
    CmHirExecutableRlibStatus status)
{
    switch (status) {
    case CM_HIR_EXECUTABLE_RLIB_OK: return "ok";
    case CM_HIR_EXECUTABLE_RLIB_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_HIR_EXECUTABLE_RLIB_LIMIT_EXCEEDED:
        return "limit exceeded";
    case CM_HIR_EXECUTABLE_RLIB_UNSUPPORTED_DESCRIPTOR:
        return "unsupported descriptor";
    case CM_HIR_EXECUTABLE_RLIB_INVALID_ARCHIVE:
        return "invalid archive";
    case CM_HIR_EXECUTABLE_RLIB_INVALID_METADATA:
        return "invalid metadata";
    case CM_HIR_EXECUTABLE_RLIB_IDENTITY_MISMATCH:
        return "identity mismatch";
    case CM_HIR_EXECUTABLE_RLIB_MEMBER_MISMATCH:
        return "archive member mismatch";
    case CM_HIR_EXECUTABLE_RLIB_OBJECT_MISMATCH:
        return "object mismatch";
    }
    return "unknown executable rlib status";
}
