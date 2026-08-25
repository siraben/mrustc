#include "cm/rlib.h"

#include <string.h>

#define CM_RLIB_GLOBAL_HEADER_SIZE ((size_t)8u)
#define CM_RLIB_MEMBER_HEADER_SIZE ((size_t)60u)

#define CM_RLIB_NAME_OFFSET ((size_t)0u)
#define CM_RLIB_NAME_SIZE ((size_t)16u)
#define CM_RLIB_TIMESTAMP_OFFSET ((size_t)16u)
#define CM_RLIB_TIMESTAMP_SIZE ((size_t)12u)
#define CM_RLIB_UID_OFFSET ((size_t)28u)
#define CM_RLIB_UID_SIZE ((size_t)6u)
#define CM_RLIB_GID_OFFSET ((size_t)34u)
#define CM_RLIB_GID_SIZE ((size_t)6u)
#define CM_RLIB_MODE_OFFSET ((size_t)40u)
#define CM_RLIB_MODE_SIZE ((size_t)8u)
#define CM_RLIB_SIZE_OFFSET ((size_t)48u)
#define CM_RLIB_SIZE_SIZE ((size_t)10u)
#define CM_RLIB_TRAILER_OFFSET ((size_t)58u)
#define CM_RLIB_TRAILER_SIZE ((size_t)2u)

static const unsigned char cm_rlib_global_header[CM_RLIB_GLOBAL_HEADER_SIZE] = {
    '!', '<', 'a', 'r', 'c', 'h', '>', '\n'
};
static const unsigned char cm_rlib_metadata_member_name[CM_RLIB_NAME_SIZE] = {
    'c', 'm', 'r', 'u', 's', 't', 'c', '.',
    'r', 'm', 'e', 't', 'a', '/', ' ', ' '
};
static const unsigned char cm_rlib_timestamp[CM_RLIB_TIMESTAMP_SIZE] = {
    '0', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char cm_rlib_owner[CM_RLIB_UID_SIZE] = {
    '0', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char cm_rlib_mode[CM_RLIB_MODE_SIZE] = {
    '1', '0', '0', '6', '4', '4', ' ', ' '
};
static const unsigned char cm_rlib_trailer[CM_RLIB_TRAILER_SIZE] = {
    '`', '\n'
};

static int cm_rlib_name_character(unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z')
        || (value >= (unsigned char)'a' && value <= (unsigned char)'z')
        || (value >= (unsigned char)'0' && value <= (unsigned char)'9')
        || value == (unsigned char)'.'
        || value == (unsigned char)'_'
        || value == (unsigned char)'-';
}

static CmRlibStatus cm_rlib_validate_name(const char *name, size_t *out_length)
{
    size_t length;

    if (name == NULL) {
        return CM_RLIB_INVALID_MEMBER_NAME;
    }
    length = 0;
    while (name[length] != '\0') {
        if (length == CM_RLIB_MAX_MEMBER_NAME
            || !cm_rlib_name_character((unsigned char)name[length])) {
            return CM_RLIB_INVALID_MEMBER_NAME;
        }
        length += 1u;
    }
    if (length == 0) {
        return CM_RLIB_INVALID_MEMBER_NAME;
    }
    if (out_length != NULL) {
        *out_length = length;
    }
    return CM_RLIB_OK;
}

static void cm_rlib_write_name(
    unsigned char field[CM_RLIB_NAME_SIZE],
    const char *name,
    size_t name_length
)
{
    memset(field, ' ', CM_RLIB_NAME_SIZE);
    memcpy(field, name, name_length);
    field[name_length] = (unsigned char)'/';
}

static CmRlibStatus cm_rlib_parse_name(
    const unsigned char field[CM_RLIB_NAME_SIZE],
    char out_name[CM_RLIB_MAX_MEMBER_NAME + 1u]
)
{
    size_t slash;
    size_t index;

    slash = 0;
    while (slash < CM_RLIB_NAME_SIZE
        && field[slash] != (unsigned char)'/') {
        if (!cm_rlib_name_character(field[slash])) {
            return CM_RLIB_INVALID_MEMBER_NAME;
        }
        slash += 1u;
    }
    if (slash == 0 || slash > CM_RLIB_MAX_MEMBER_NAME
        || slash == CM_RLIB_NAME_SIZE) {
        return CM_RLIB_INVALID_MEMBER_NAME;
    }
    for (index = slash + 1u; index < CM_RLIB_NAME_SIZE; index += 1u) {
        if (field[index] != (unsigned char)' ') {
            return CM_RLIB_INVALID_MEMBER_NAME;
        }
    }
    memcpy(out_name, field, slash);
    out_name[slash] = '\0';
    return CM_RLIB_OK;
}

static void cm_rlib_write_size(unsigned char field[CM_RLIB_SIZE_SIZE], size_t value)
{
    unsigned char reversed[CM_RLIB_SIZE_SIZE];
    size_t digit_count;
    size_t index;

    memset(field, ' ', CM_RLIB_SIZE_SIZE);
    digit_count = 0;
    do {
        reversed[digit_count] = (unsigned char)('0' + (value % 10u));
        digit_count += 1u;
        value /= 10u;
    } while (value != 0);
    for (index = 0; index < digit_count; index += 1u) {
        field[index] = reversed[digit_count - index - 1u];
    }
}

static CmRlibStatus cm_rlib_parse_size(
    const unsigned char field[CM_RLIB_SIZE_SIZE],
    size_t *out_size
)
{
    size_t digit_count;
    size_t index;
    size_t value;
    unsigned int digit;

    digit_count = 0;
    while (digit_count < CM_RLIB_SIZE_SIZE
        && field[digit_count] >= (unsigned char)'0'
        && field[digit_count] <= (unsigned char)'9') {
        digit_count += 1u;
    }
    if (digit_count == 0
        || (digit_count > 1u && field[0] == (unsigned char)'0')) {
        return CM_RLIB_INVALID_HEADER;
    }
    for (index = digit_count; index < CM_RLIB_SIZE_SIZE; index += 1u) {
        if (field[index] != (unsigned char)' ') {
            return CM_RLIB_INVALID_HEADER;
        }
    }

    value = 0;
    for (index = 0; index < digit_count; index += 1u) {
        digit = (unsigned int)(field[index] - (unsigned char)'0');
        if (value > ((size_t)-1 - (size_t)digit) / 10u) {
            return CM_RLIB_INVALID_HEADER;
        }
        value = value * 10u + (size_t)digit;
    }
    if (value > CM_RLIB_MAX_MEMBER_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }
    *out_size = value;
    return CM_RLIB_OK;
}

static int cm_rlib_header_is_canonical(const unsigned char *header)
{
    return memcmp(header + CM_RLIB_TIMESTAMP_OFFSET,
            cm_rlib_timestamp, CM_RLIB_TIMESTAMP_SIZE) == 0
        && memcmp(header + CM_RLIB_UID_OFFSET,
            cm_rlib_owner, CM_RLIB_UID_SIZE) == 0
        && memcmp(header + CM_RLIB_GID_OFFSET,
            cm_rlib_owner, CM_RLIB_GID_SIZE) == 0
        && memcmp(header + CM_RLIB_MODE_OFFSET,
            cm_rlib_mode, CM_RLIB_MODE_SIZE) == 0
        && memcmp(header + CM_RLIB_TRAILER_OFFSET,
            cm_rlib_trailer, CM_RLIB_TRAILER_SIZE) == 0;
}

CmRlibStatus cm_rlib_encode_members(
    CmByteBuf *output,
    const CmRlibMember *members,
    size_t member_count
)
{
    CmByteBuf candidate;
    CmByteBuf previous;
    unsigned char header[CM_RLIB_MEMBER_HEADER_SIZE];
    size_t name_length;
    size_t archive_length;
    size_t index;
    CmRlibStatus status;

    if (output == NULL || (members == NULL && member_count != 0)) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    if (member_count > CM_RLIB_MAX_MEMBER_COUNT) {
        return CM_RLIB_TOO_MANY_MEMBERS;
    }

    archive_length = CM_RLIB_GLOBAL_HEADER_SIZE;
    for (index = 0; index < member_count; index += 1u) {
        status = cm_rlib_validate_name(members[index].name, &name_length);
        if (status != CM_RLIB_OK) {
            return status;
        }
        if (index != 0
            && strcmp(members[index - 1u].name, members[index].name) >= 0) {
            return CM_RLIB_NONCANONICAL_ORDER;
        }
        if (members[index].data == NULL && members[index].length != 0) {
            return CM_RLIB_INVALID_ARGUMENT;
        }
        if (members[index].length > CM_RLIB_MAX_MEMBER_SIZE) {
            return CM_RLIB_LIMIT_EXCEEDED;
        }
        if (archive_length > CM_RLIB_MAX_ARCHIVE_SIZE
                - CM_RLIB_MEMBER_HEADER_SIZE
            || members[index].length > CM_RLIB_MAX_ARCHIVE_SIZE
                - archive_length - CM_RLIB_MEMBER_HEADER_SIZE) {
            return CM_RLIB_LIMIT_EXCEEDED;
        }
        archive_length += CM_RLIB_MEMBER_HEADER_SIZE + members[index].length;
        if (members[index].length % 2u != 0) {
            if (archive_length == CM_RLIB_MAX_ARCHIVE_SIZE) {
                return CM_RLIB_LIMIT_EXCEEDED;
            }
            archive_length += 1u;
        }
    }

    cm_byte_buf_init(&candidate);
    cm_byte_buf_append(&candidate,
        cm_rlib_global_header, CM_RLIB_GLOBAL_HEADER_SIZE);
    for (index = 0; index < member_count; index += 1u) {
        status = cm_rlib_validate_name(members[index].name, &name_length);
        if (status != CM_RLIB_OK) {
            cm_byte_buf_destroy(&candidate);
            return status;
        }
        memset(header, ' ', sizeof(header));
        cm_rlib_write_name(header + CM_RLIB_NAME_OFFSET,
            members[index].name, name_length);
        memcpy(header + CM_RLIB_TIMESTAMP_OFFSET,
            cm_rlib_timestamp, CM_RLIB_TIMESTAMP_SIZE);
        memcpy(header + CM_RLIB_UID_OFFSET,
            cm_rlib_owner, CM_RLIB_UID_SIZE);
        memcpy(header + CM_RLIB_GID_OFFSET,
            cm_rlib_owner, CM_RLIB_GID_SIZE);
        memcpy(header + CM_RLIB_MODE_OFFSET,
            cm_rlib_mode, CM_RLIB_MODE_SIZE);
        cm_rlib_write_size(header + CM_RLIB_SIZE_OFFSET,
            members[index].length);
        memcpy(header + CM_RLIB_TRAILER_OFFSET,
            cm_rlib_trailer, CM_RLIB_TRAILER_SIZE);
        cm_byte_buf_append(&candidate, header, sizeof(header));
        cm_byte_buf_append(&candidate,
            members[index].data, members[index].length);
        if (members[index].length % 2u != 0) {
            cm_byte_buf_push(&candidate, (unsigned char)'\n');
        }
    }

    previous = *output;
    *output = candidate;
    cm_byte_buf_destroy(&previous);
    return CM_RLIB_OK;
}

CmRlibStatus cm_rlib_decode_members(
    const void *archive,
    size_t archive_length,
    CmRlibArchiveView *out_archive
)
{
    const unsigned char *bytes;
    const unsigned char *header;
    CmRlibMemberView member;
    size_t offset;
    size_t payload_end;
    CmRlibStatus status;

    if (out_archive == NULL) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    memset(out_archive, 0, sizeof(*out_archive));
    if (archive == NULL) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    if (archive_length > CM_RLIB_MAX_ARCHIVE_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }
    bytes = (const unsigned char *)archive;
    if (archive_length < CM_RLIB_GLOBAL_HEADER_SIZE) {
        return CM_RLIB_TRUNCATED;
    }
    if (memcmp(bytes, cm_rlib_global_header, CM_RLIB_GLOBAL_HEADER_SIZE) != 0) {
        return CM_RLIB_WRONG_MAGIC;
    }

    offset = CM_RLIB_GLOBAL_HEADER_SIZE;
    while (offset < archive_length) {
        if (out_archive->member_count == CM_RLIB_MAX_MEMBER_COUNT) {
            memset(out_archive, 0, sizeof(*out_archive));
            return CM_RLIB_TOO_MANY_MEMBERS;
        }
        if (archive_length - offset < CM_RLIB_MEMBER_HEADER_SIZE) {
            status = out_archive->member_count == 0
                ? CM_RLIB_TRUNCATED : CM_RLIB_TRAILING_BYTES;
            memset(out_archive, 0, sizeof(*out_archive));
            return status;
        }
        header = bytes + offset;
        memset(&member, 0, sizeof(member));
        status = cm_rlib_parse_name(
            header + CM_RLIB_NAME_OFFSET, member.name);
        if (status != CM_RLIB_OK) {
            memset(out_archive, 0, sizeof(*out_archive));
            return status;
        }
        if (!cm_rlib_header_is_canonical(header)) {
            memset(out_archive, 0, sizeof(*out_archive));
            return CM_RLIB_INVALID_HEADER;
        }
        status = cm_rlib_parse_size(
            header + CM_RLIB_SIZE_OFFSET, &member.length);
        if (status != CM_RLIB_OK) {
            memset(out_archive, 0, sizeof(*out_archive));
            return status;
        }
        if (out_archive->member_count != 0
            && strcmp(out_archive->members[
                    out_archive->member_count - 1u].name, member.name) >= 0) {
            memset(out_archive, 0, sizeof(*out_archive));
            return CM_RLIB_NONCANONICAL_ORDER;
        }

        offset += CM_RLIB_MEMBER_HEADER_SIZE;
        if (member.length > archive_length - offset) {
            memset(out_archive, 0, sizeof(*out_archive));
            return CM_RLIB_TRUNCATED;
        }
        member.data = bytes + offset;
        payload_end = offset + member.length;
        if (member.length % 2u != 0) {
            if (payload_end == archive_length) {
                memset(out_archive, 0, sizeof(*out_archive));
                return CM_RLIB_TRUNCATED;
            }
            if (bytes[payload_end] != (unsigned char)'\n') {
                memset(out_archive, 0, sizeof(*out_archive));
                return CM_RLIB_INVALID_PADDING;
            }
            payload_end += 1u;
        }
        out_archive->members[out_archive->member_count] = member;
        out_archive->member_count += 1u;
        offset = payload_end;
    }
    return CM_RLIB_OK;
}

CmRlibStatus cm_rlib_find_member(
    const CmRlibArchiveView *archive,
    const char *name,
    CmRlibMemberView *out_member
)
{
    size_t index;
    int order;

    if (out_member == NULL) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    memset(out_member, 0, sizeof(*out_member));
    if (archive == NULL || cm_rlib_validate_name(name, NULL) != CM_RLIB_OK
        || archive->member_count > CM_RLIB_MAX_MEMBER_COUNT) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    for (index = 0; index < archive->member_count; index += 1u) {
        order = strcmp(archive->members[index].name, name);
        if (order == 0) {
            *out_member = archive->members[index];
            return CM_RLIB_OK;
        }
        if (order > 0) {
            break;
        }
    }
    return CM_RLIB_WRONG_MEMBER;
}

CmRlibStatus cm_rlib_encode_metadata(
    CmByteBuf *output,
    const void *metadata,
    size_t metadata_length
)
{
    CmRlibMember member;

    if (output == NULL || (metadata == NULL && metadata_length != 0)) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    if (metadata_length > CM_RLIB_MAX_METADATA_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }
    member.name = "cmrustc.rmeta";
    member.data = metadata;
    member.length = metadata_length;
    return cm_rlib_encode_members(output, &member, 1u);
}

CmRlibStatus cm_rlib_decode_metadata(
    const void *archive,
    size_t archive_length,
    CmRlibMetadataView *out_metadata
)
{
    const unsigned char *bytes;
    const unsigned char *header;
    size_t metadata_length;
    size_t exact_length;
    CmRlibStatus status;

    if (out_metadata == NULL) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    out_metadata->data = NULL;
    out_metadata->length = 0;
    if (archive == NULL) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    bytes = (const unsigned char *)archive;
    if (archive_length < CM_RLIB_GLOBAL_HEADER_SIZE) {
        return CM_RLIB_TRUNCATED;
    }
    if (memcmp(bytes, cm_rlib_global_header,
        CM_RLIB_GLOBAL_HEADER_SIZE) != 0) {
        return CM_RLIB_WRONG_MAGIC;
    }
    if (archive_length < CM_RLIB_GLOBAL_HEADER_SIZE
            + CM_RLIB_MEMBER_HEADER_SIZE) {
        return CM_RLIB_TRUNCATED;
    }
    header = bytes + CM_RLIB_GLOBAL_HEADER_SIZE;
    if (memcmp(header + CM_RLIB_NAME_OFFSET,
        cm_rlib_metadata_member_name, CM_RLIB_NAME_SIZE) != 0) {
        return CM_RLIB_WRONG_MEMBER;
    }
    if (!cm_rlib_header_is_canonical(header)) {
        return CM_RLIB_INVALID_HEADER;
    }
    status = cm_rlib_parse_size(
        header + CM_RLIB_SIZE_OFFSET, &metadata_length);
    if (status != CM_RLIB_OK) {
        return status;
    }
    if (metadata_length > CM_RLIB_MAX_METADATA_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }

    exact_length = CM_RLIB_GLOBAL_HEADER_SIZE
        + CM_RLIB_MEMBER_HEADER_SIZE + metadata_length;
    if (metadata_length % 2u != 0) {
        exact_length += 1u;
    }
    if (archive_length < exact_length) {
        return CM_RLIB_TRUNCATED;
    }
    if (metadata_length % 2u != 0
        && bytes[exact_length - 1u] != (unsigned char)'\n') {
        return CM_RLIB_INVALID_PADDING;
    }
    if (archive_length > exact_length) {
        return CM_RLIB_TRAILING_BYTES;
    }

    out_metadata->data = bytes + CM_RLIB_GLOBAL_HEADER_SIZE
        + CM_RLIB_MEMBER_HEADER_SIZE;
    out_metadata->length = metadata_length;
    return CM_RLIB_OK;
}

const char *cm_rlib_status_name(CmRlibStatus status)
{
    switch (status) {
    case CM_RLIB_OK:
        return "ok";
    case CM_RLIB_INVALID_ARGUMENT:
        return "invalid argument";
    case CM_RLIB_LIMIT_EXCEEDED:
        return "limit exceeded";
    case CM_RLIB_WRONG_MAGIC:
        return "wrong magic";
    case CM_RLIB_TRUNCATED:
        return "truncated";
    case CM_RLIB_INVALID_HEADER:
        return "invalid header";
    case CM_RLIB_WRONG_MEMBER:
        return "wrong member";
    case CM_RLIB_INVALID_PADDING:
        return "invalid padding";
    case CM_RLIB_TRAILING_BYTES:
        return "trailing bytes";
    case CM_RLIB_INVALID_MEMBER_NAME:
        return "invalid member name";
    case CM_RLIB_TOO_MANY_MEMBERS:
        return "too many members";
    case CM_RLIB_NONCANONICAL_ORDER:
        return "noncanonical member order";
    }
    return "unknown rlib status";
}
