#include "cm/rlib.h"

#include <string.h>

#define CM_RLIB_GLOBAL_HEADER_SIZE ((size_t)8u)
#define CM_RLIB_MEMBER_HEADER_SIZE ((size_t)60u)
#define CM_RLIB_PAYLOAD_OFFSET \
    (CM_RLIB_GLOBAL_HEADER_SIZE + CM_RLIB_MEMBER_HEADER_SIZE)

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
static const unsigned char cm_rlib_member_name[CM_RLIB_NAME_SIZE] = {
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

static void cm_rlib_write_size(unsigned char field[CM_RLIB_SIZE_SIZE], size_t value)
{
    unsigned char reversed[CM_RLIB_SIZE_SIZE];
    size_t digit_count;
    size_t index;

    memset(field, ' ', CM_RLIB_SIZE_SIZE);
    digit_count = 0;
    do {
        reversed[digit_count] = (unsigned char)('0' + (value % 10u));
        digit_count += 1;
        value /= 10u;
    } while (value != 0);
    for (index = 0; index < digit_count; index += 1) {
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
        digit_count += 1;
    }
    if (digit_count == 0
        || (digit_count > 1 && field[0] == (unsigned char)'0')) {
        return CM_RLIB_INVALID_HEADER;
    }
    for (index = digit_count; index < CM_RLIB_SIZE_SIZE; index += 1) {
        if (field[index] != (unsigned char)' ') {
            return CM_RLIB_INVALID_HEADER;
        }
    }

    value = 0;
    for (index = 0; index < digit_count; index += 1) {
        digit = (unsigned int)(field[index] - (unsigned char)'0');
        if (value > ((size_t)-1 - (size_t)digit) / 10u) {
            return CM_RLIB_INVALID_HEADER;
        }
        value = value * 10u + (size_t)digit;
    }
    if (value > CM_RLIB_MAX_METADATA_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }
    *out_size = value;
    return CM_RLIB_OK;
}

CmRlibStatus cm_rlib_encode_metadata(
    CmByteBuf *output,
    const void *metadata,
    size_t metadata_length
)
{
    CmByteBuf candidate;
    unsigned char member_header[CM_RLIB_MEMBER_HEADER_SIZE];
    CmByteBuf previous;

    if (output == NULL || (metadata == NULL && metadata_length != 0)) {
        return CM_RLIB_INVALID_ARGUMENT;
    }
    if (metadata_length > CM_RLIB_MAX_METADATA_SIZE) {
        return CM_RLIB_LIMIT_EXCEEDED;
    }

    memset(member_header, ' ', sizeof(member_header));
    memcpy(member_header + CM_RLIB_NAME_OFFSET,
        cm_rlib_member_name, CM_RLIB_NAME_SIZE);
    memcpy(member_header + CM_RLIB_TIMESTAMP_OFFSET,
        cm_rlib_timestamp, CM_RLIB_TIMESTAMP_SIZE);
    memcpy(member_header + CM_RLIB_UID_OFFSET,
        cm_rlib_owner, CM_RLIB_UID_SIZE);
    memcpy(member_header + CM_RLIB_GID_OFFSET,
        cm_rlib_owner, CM_RLIB_GID_SIZE);
    memcpy(member_header + CM_RLIB_MODE_OFFSET,
        cm_rlib_mode, CM_RLIB_MODE_SIZE);
    cm_rlib_write_size(member_header + CM_RLIB_SIZE_OFFSET, metadata_length);
    memcpy(member_header + CM_RLIB_TRAILER_OFFSET,
        cm_rlib_trailer, CM_RLIB_TRAILER_SIZE);

    cm_byte_buf_init(&candidate);
    cm_byte_buf_append(&candidate,
        cm_rlib_global_header, CM_RLIB_GLOBAL_HEADER_SIZE);
    cm_byte_buf_append(&candidate, member_header, sizeof(member_header));
    cm_byte_buf_append(&candidate, metadata, metadata_length);
    if (metadata_length % 2u != 0) {
        cm_byte_buf_push(&candidate, (unsigned char)'\n');
    }

    previous = *output;
    *output = candidate;
    cm_byte_buf_destroy(&previous);
    return CM_RLIB_OK;
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
    if (memcmp(bytes, cm_rlib_global_header, CM_RLIB_GLOBAL_HEADER_SIZE) != 0) {
        return CM_RLIB_WRONG_MAGIC;
    }
    if (archive_length < CM_RLIB_PAYLOAD_OFFSET) {
        return CM_RLIB_TRUNCATED;
    }

    header = bytes + CM_RLIB_GLOBAL_HEADER_SIZE;
    if (memcmp(header + CM_RLIB_NAME_OFFSET,
        cm_rlib_member_name, CM_RLIB_NAME_SIZE) != 0) {
        return CM_RLIB_WRONG_MEMBER;
    }
    if (memcmp(header + CM_RLIB_TIMESTAMP_OFFSET,
            cm_rlib_timestamp, CM_RLIB_TIMESTAMP_SIZE) != 0
        || memcmp(header + CM_RLIB_UID_OFFSET,
            cm_rlib_owner, CM_RLIB_UID_SIZE) != 0
        || memcmp(header + CM_RLIB_GID_OFFSET,
            cm_rlib_owner, CM_RLIB_GID_SIZE) != 0
        || memcmp(header + CM_RLIB_MODE_OFFSET,
            cm_rlib_mode, CM_RLIB_MODE_SIZE) != 0
        || memcmp(header + CM_RLIB_TRAILER_OFFSET,
            cm_rlib_trailer, CM_RLIB_TRAILER_SIZE) != 0) {
        return CM_RLIB_INVALID_HEADER;
    }
    status = cm_rlib_parse_size(
        header + CM_RLIB_SIZE_OFFSET, &metadata_length);
    if (status != CM_RLIB_OK) {
        return status;
    }

    exact_length = CM_RLIB_PAYLOAD_OFFSET + metadata_length;
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

    out_metadata->data = bytes + CM_RLIB_PAYLOAD_OFFSET;
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
    }
    return "unknown rlib status";
}
