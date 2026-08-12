#include "metadata_codec.h"

#include <string.h>

static const unsigned char cm_hir_metadata_magic[
    CM_HIR_METADATA_MAGIC_SIZE] = {
    (unsigned char)'C', (unsigned char)'M', (unsigned char)'H',
    (unsigned char)'I', (unsigned char)'R', (unsigned char)'M',
    (unsigned char)'1', UINT8_C(0)
};

static CmHirMetadataStatus cm_hir_metadata_writer_fail(
    CmHirMetadataWriter *writer, CmHirMetadataStatus status)
{
    if (writer != NULL && writer->status == CM_HIR_METADATA_OK) {
        writer->status = status;
    }
    return writer == NULL ? status : writer->status;
}

static CmHirMetadataStatus cm_hir_metadata_writer_grow(
    CmHirMetadataWriter *writer, size_t length, size_t *out_length)
{
    size_t new_length;

    if (writer == NULL || out_length == NULL || writer->buffer == NULL) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (writer->status != CM_HIR_METADATA_OK) return writer->status;
    if (length > (size_t)-1 - writer->buffer->len) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_LENGTH_OVERFLOW);
    }
    new_length = writer->buffer->len + length;
    if (new_length > writer->maximum_length) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_LIMIT_EXCEEDED);
    }
    *out_length = new_length;
    return CM_HIR_METADATA_OK;
}

void cm_hir_metadata_writer_init(CmHirMetadataWriter *writer,
    CmByteBuf *buffer, size_t maximum_length)
{
    if (writer == NULL) return;
    writer->buffer = buffer;
    writer->maximum_length = maximum_length;
    writer->status = buffer == NULL
        ? CM_HIR_METADATA_INVALID_ARGUMENT : CM_HIR_METADATA_OK;
    if (buffer != NULL && buffer->len > maximum_length) {
        writer->status = CM_HIR_METADATA_LIMIT_EXCEEDED;
    }
}

CmHirMetadataStatus cm_hir_metadata_writer_status(
    const CmHirMetadataWriter *writer)
{
    return writer == NULL ? CM_HIR_METADATA_INVALID_ARGUMENT
        : writer->status;
}

CmHirMetadataStatus cm_hir_metadata_write_bytes(CmHirMetadataWriter *writer,
    const void *data, size_t length)
{
    size_t new_length;

    if (length != 0u && data == NULL) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (cm_hir_metadata_writer_grow(writer, length, &new_length)
            != CM_HIR_METADATA_OK) {
        return cm_hir_metadata_writer_status(writer);
    }
    (void)new_length;
    cm_byte_buf_append(writer->buffer, data, length);
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_write_u8(CmHirMetadataWriter *writer,
    uint8_t value)
{
    unsigned char encoded[1];

    encoded[0] = (unsigned char)value;
    return cm_hir_metadata_write_bytes(writer, encoded, sizeof(encoded));
}

CmHirMetadataStatus cm_hir_metadata_write_u16(CmHirMetadataWriter *writer,
    uint16_t value)
{
    unsigned char encoded[2];

    encoded[0] = (unsigned char)(value & UINT16_C(0xff));
    encoded[1] = (unsigned char)((value >> 8u) & UINT16_C(0xff));
    return cm_hir_metadata_write_bytes(writer, encoded, sizeof(encoded));
}

CmHirMetadataStatus cm_hir_metadata_write_u32(CmHirMetadataWriter *writer,
    uint32_t value)
{
    unsigned char encoded[4];

    encoded[0] = (unsigned char)(value & UINT32_C(0xff));
    encoded[1] = (unsigned char)((value >> 8u) & UINT32_C(0xff));
    encoded[2] = (unsigned char)((value >> 16u) & UINT32_C(0xff));
    encoded[3] = (unsigned char)((value >> 24u) & UINT32_C(0xff));
    return cm_hir_metadata_write_bytes(writer, encoded, sizeof(encoded));
}

CmHirMetadataStatus cm_hir_metadata_write_u64(CmHirMetadataWriter *writer,
    uint64_t value)
{
    unsigned char encoded[8];
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        encoded[index] = (unsigned char)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    return cm_hir_metadata_write_bytes(writer, encoded, sizeof(encoded));
}

CmHirMetadataStatus cm_hir_metadata_write_section(
    CmHirMetadataWriter *writer, const unsigned char tag[4],
    const void *data, size_t length)
{
    unsigned char copied_tag[4];
    CmByteBuf frame;
    CmHirMetadataWriter frame_writer;
    size_t frame_length;
    size_t new_length;
    CmHirMetadataStatus status;

    if (writer == NULL || tag == NULL || (length != 0u && data == NULL)) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    memcpy(copied_tag, tag, sizeof(copied_tag));
    if (length > (size_t)-1 - CM_HIR_METADATA_SECTION_HEADER_SIZE) {
        return cm_hir_metadata_writer_fail(writer,
            CM_HIR_METADATA_LENGTH_OVERFLOW);
    }
    frame_length = CM_HIR_METADATA_SECTION_HEADER_SIZE + length;
    if (cm_hir_metadata_writer_grow(writer, frame_length, &new_length)
            != CM_HIR_METADATA_OK) {
        return writer->status;
    }
    (void)new_length;
    cm_byte_buf_init(&frame);
    cm_hir_metadata_writer_init(&frame_writer, &frame, frame_length);
    status = cm_hir_metadata_write_bytes(&frame_writer, copied_tag,
        sizeof(copied_tag));
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u64(&frame_writer, (uint64_t)length);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_bytes(&frame_writer, data, length);
    if (status != CM_HIR_METADATA_OK || frame.len != frame_length) {
        cm_byte_buf_destroy(&frame);
        return cm_hir_metadata_writer_fail(writer,
            status != CM_HIR_METADATA_OK ? status
                : CM_HIR_METADATA_LENGTH_OVERFLOW);
    }
    cm_byte_buf_append(writer->buffer, frame.data, frame.len);
    cm_byte_buf_destroy(&frame);
    return CM_HIR_METADATA_OK;
}

static CmHirMetadataStatus cm_hir_metadata_reader_fail(
    CmHirMetadataReader *reader, CmHirMetadataStatus status)
{
    if (reader != NULL && reader->status == CM_HIR_METADATA_OK) {
        reader->status = status;
    }
    return reader == NULL ? status : reader->status;
}

void cm_hir_metadata_reader_init(CmHirMetadataReader *reader,
    const void *data, size_t length)
{
    if (reader == NULL) return;
    reader->data = (const unsigned char *)data;
    reader->length = length;
    reader->cursor = 0u;
    reader->status = length != 0u && data == NULL
        ? CM_HIR_METADATA_INVALID_ARGUMENT : CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_reader_status(
    const CmHirMetadataReader *reader)
{
    return reader == NULL ? CM_HIR_METADATA_INVALID_ARGUMENT
        : reader->status;
}

size_t cm_hir_metadata_reader_remaining(const CmHirMetadataReader *reader)
{
    if (reader == NULL || reader->status != CM_HIR_METADATA_OK
        || reader->cursor > reader->length) return 0u;
    return reader->length - reader->cursor;
}

CmHirMetadataStatus cm_hir_metadata_read_bytes(CmHirMetadataReader *reader,
    size_t length, const unsigned char **out_data)
{
    const unsigned char *value;

    if (reader == NULL || out_data == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (reader->status != CM_HIR_METADATA_OK) return reader->status;
    if (reader->cursor > reader->length
        || length > reader->length - reader->cursor) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_TRUNCATED);
    }
    value = reader->data == NULL ? NULL : reader->data + reader->cursor;
    reader->cursor += length;
    *out_data = value;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_read_u8(CmHirMetadataReader *reader,
    uint8_t *out_value)
{
    const unsigned char *encoded;

    if (out_value == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (cm_hir_metadata_read_bytes(reader, 1u, &encoded)
            != CM_HIR_METADATA_OK) return cm_hir_metadata_reader_status(reader);
    *out_value = (uint8_t)encoded[0];
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_read_u16(CmHirMetadataReader *reader,
    uint16_t *out_value)
{
    const unsigned char *encoded;
    uint16_t value;

    if (out_value == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (cm_hir_metadata_read_bytes(reader, 2u, &encoded)
            != CM_HIR_METADATA_OK) return cm_hir_metadata_reader_status(reader);
    value = (uint16_t)encoded[0];
    value |= (uint16_t)((uint16_t)encoded[1] << 8u);
    *out_value = value;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_read_u32(CmHirMetadataReader *reader,
    uint32_t *out_value)
{
    const unsigned char *encoded;
    uint32_t value;
    unsigned int index;

    if (out_value == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (cm_hir_metadata_read_bytes(reader, 4u, &encoded)
            != CM_HIR_METADATA_OK) return cm_hir_metadata_reader_status(reader);
    value = UINT32_C(0);
    for (index = 0u; index < 4u; ++index) {
        value |= (uint32_t)encoded[index] << (index * 8u);
    }
    *out_value = value;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_read_u64(CmHirMetadataReader *reader,
    uint64_t *out_value)
{
    const unsigned char *encoded;
    uint64_t value;
    unsigned int index;

    if (out_value == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (cm_hir_metadata_read_bytes(reader, 8u, &encoded)
            != CM_HIR_METADATA_OK) return cm_hir_metadata_reader_status(reader);
    value = UINT64_C(0);
    for (index = 0u; index < 8u; ++index) {
        value |= (uint64_t)encoded[index] << (index * 8u);
    }
    *out_value = value;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_reader_finish(
    CmHirMetadataReader *reader)
{
    if (reader == NULL) return CM_HIR_METADATA_INVALID_ARGUMENT;
    if (reader->status != CM_HIR_METADATA_OK) return reader->status;
    if (reader->cursor != reader->length) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_TRAILING_BYTES);
    }
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_read_section(
    CmHirMetadataReader *reader, CmHirMetadataSection *out_section)
{
    CmHirMetadataReader trial;
    const unsigned char *tag;
    const unsigned char *contents;
    uint64_t wire_length;
    size_t length;

    if (reader == NULL || out_section == NULL) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_INVALID_ARGUMENT);
    }
    if (reader->status != CM_HIR_METADATA_OK) return reader->status;
    if (reader->cursor == reader->length) return CM_HIR_METADATA_DONE;
    trial = *reader;
    if (cm_hir_metadata_read_bytes(&trial, 4u, &tag)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u64(&trial, &wire_length)
            != CM_HIR_METADATA_OK) {
        return cm_hir_metadata_reader_fail(reader, trial.status);
    }
    if (wire_length > (uint64_t)(size_t)-1) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_LENGTH_OVERFLOW);
    }
    length = (size_t)wire_length;
    if (length > (size_t)-1 - trial.cursor) {
        return cm_hir_metadata_reader_fail(reader,
            CM_HIR_METADATA_LENGTH_OVERFLOW);
    }
    if (cm_hir_metadata_read_bytes(&trial, length, &contents)
            != CM_HIR_METADATA_OK) {
        return cm_hir_metadata_reader_fail(reader, trial.status);
    }
    memcpy(out_section->tag, tag, sizeof(out_section->tag));
    out_section->data = contents;
    out_section->length = length;
    *reader = trial;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_validate_sections(
    const void *data, size_t length)
{
    CmHirMetadataReader reader;
    CmHirMetadataSection section;
    CmHirMetadataStatus status;

    cm_hir_metadata_reader_init(&reader, data, length);
    if (reader.status != CM_HIR_METADATA_OK) return reader.status;
    for (;;) {
        status = cm_hir_metadata_read_section(&reader, &section);
        if (status == CM_HIR_METADATA_DONE) return CM_HIR_METADATA_OK;
        if (status != CM_HIR_METADATA_OK) return status;
    }
}

uint32_t cm_hir_metadata_crc32(const void *data, size_t length)
{
    const unsigned char *bytes;
    uint32_t crc;
    size_t index;
    unsigned int bit;

    if (length != 0u && data == NULL) return UINT32_C(0);
    bytes = (const unsigned char *)data;
    crc = UINT32_C(0xffffffff);
    for (index = 0u; index < length; ++index) {
        crc ^= (uint32_t)bytes[index];
        for (bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u)
                ^ ((crc & UINT32_C(1)) != 0u
                    ? UINT32_C(0xedb88320) : UINT32_C(0));
        }
    }
    return crc ^ UINT32_C(0xffffffff);
}

CmHirMetadataStatus cm_hir_metadata_encode_envelope_version(
    CmByteBuf *output, uint16_t major, uint16_t minor, uint32_t flags,
    const void *payload, size_t payload_length)
{
    CmByteBuf encoded;
    CmHirMetadataWriter writer;
    CmHirMetadataStatus status;
    size_t encoded_length;

    if (output == NULL || (payload_length != 0u && payload == NULL)) {
        return CM_HIR_METADATA_INVALID_ARGUMENT;
    }
    if ((flags & ~CM_HIR_METADATA_SUPPORTED_FLAGS) != UINT32_C(0)) {
        return CM_HIR_METADATA_UNSUPPORTED_FLAGS;
    }
    if (payload_length > (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE) {
        return CM_HIR_METADATA_LIMIT_EXCEEDED;
    }
    status = cm_hir_metadata_validate_sections(payload, payload_length);
    if (status != CM_HIR_METADATA_OK) return status;
    if (payload_length > (size_t)-1 - CM_HIR_METADATA_HEADER_SIZE) {
        return CM_HIR_METADATA_LENGTH_OVERFLOW;
    }
    encoded_length = CM_HIR_METADATA_HEADER_SIZE + payload_length;
    cm_byte_buf_init(&encoded);
    cm_hir_metadata_writer_init(&writer, &encoded, encoded_length);
    status = cm_hir_metadata_write_bytes(&writer, cm_hir_metadata_magic,
        sizeof(cm_hir_metadata_magic));
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u16(&writer,
            major);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u16(&writer,
            minor);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u32(&writer, flags);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u32(&writer,
            (uint32_t)CM_HIR_METADATA_HEADER_SIZE);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u64(&writer,
            (uint64_t)payload_length);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u32(&writer,
            cm_hir_metadata_crc32(payload, payload_length));
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_bytes(&writer, payload,
            payload_length);
    if (status != CM_HIR_METADATA_OK || encoded.len != encoded_length) {
        cm_byte_buf_destroy(&encoded);
        return status != CM_HIR_METADATA_OK ? status
            : CM_HIR_METADATA_LENGTH_OVERFLOW;
    }
    cm_byte_buf_destroy(output);
    *output = encoded;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_encode_envelope(CmByteBuf *output,
    uint32_t flags, const void *payload, size_t payload_length)
{
    return cm_hir_metadata_encode_envelope_version(output,
        (uint16_t)CM_HIR_METADATA_MAJOR,
        (uint16_t)CM_HIR_METADATA_MINOR, flags, payload, payload_length);
}

CmHirMetadataStatus cm_hir_metadata_decode_envelope_version(
    const void *encoded, size_t encoded_length, uint16_t expected_major,
    uint16_t expected_minor, CmHirMetadataEnvelope *out_envelope)
{
    CmHirMetadataReader reader;
    const unsigned char *magic;
    const unsigned char *payload;
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
    uint32_t header_length;
    uint32_t reserved_before_payload;
    uint32_t payload_crc;
    uint32_t reserved_after_crc;
    uint64_t wire_payload_length;
    size_t payload_length;
    size_t total_length;
    CmHirMetadataStatus status;

    if (out_envelope != NULL) memset(out_envelope, 0, sizeof(*out_envelope));
    if (out_envelope == NULL || (encoded_length != 0u && encoded == NULL)) {
        return CM_HIR_METADATA_INVALID_ARGUMENT;
    }
    if (encoded_length < CM_HIR_METADATA_HEADER_SIZE) {
        return CM_HIR_METADATA_TRUNCATED;
    }
    cm_hir_metadata_reader_init(&reader, encoded, encoded_length);
    if (cm_hir_metadata_read_bytes(&reader, CM_HIR_METADATA_MAGIC_SIZE,
            &magic) != CM_HIR_METADATA_OK) return reader.status;
    if (memcmp(magic, cm_hir_metadata_magic,
            sizeof(cm_hir_metadata_magic)) != 0) {
        return CM_HIR_METADATA_WRONG_MAGIC;
    }
    if (cm_hir_metadata_read_u16(&reader, &major) != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u16(&reader, &minor) != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(&reader, &flags) != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(&reader, &header_length)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(&reader, &reserved_before_payload)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u64(&reader, &wire_payload_length)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(&reader, &payload_crc)
            != CM_HIR_METADATA_OK
        || cm_hir_metadata_read_u32(&reader, &reserved_after_crc)
            != CM_HIR_METADATA_OK) return reader.status;
    if (major != expected_major || minor != expected_minor) {
        return CM_HIR_METADATA_UNSUPPORTED_VERSION;
    }
    if ((flags & ~CM_HIR_METADATA_SUPPORTED_FLAGS) != UINT32_C(0)) {
        return CM_HIR_METADATA_UNSUPPORTED_FLAGS;
    }
    if (header_length != (uint32_t)CM_HIR_METADATA_HEADER_SIZE) {
        return CM_HIR_METADATA_INVALID_HEADER_LENGTH;
    }
    if (reserved_before_payload != UINT32_C(0)
        || reserved_after_crc != UINT32_C(0)) {
        return CM_HIR_METADATA_NONZERO_RESERVED;
    }
    if (wire_payload_length > (uint64_t)(size_t)-1) {
        return CM_HIR_METADATA_LENGTH_OVERFLOW;
    }
    payload_length = (size_t)wire_payload_length;
    if (payload_length > (size_t)-1 - (size_t)header_length) {
        return CM_HIR_METADATA_LENGTH_OVERFLOW;
    }
    if (payload_length > (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE) {
        return CM_HIR_METADATA_LIMIT_EXCEEDED;
    }
    total_length = (size_t)header_length + payload_length;
    if (total_length > encoded_length) return CM_HIR_METADATA_TRUNCATED;
    if (total_length < encoded_length) return CM_HIR_METADATA_TRAILING_BYTES;
    if (cm_hir_metadata_read_bytes(&reader, payload_length, &payload)
            != CM_HIR_METADATA_OK) return reader.status;
    if (cm_hir_metadata_crc32(payload, payload_length) != payload_crc) {
        return CM_HIR_METADATA_CRC_MISMATCH;
    }
    status = cm_hir_metadata_validate_sections(payload, payload_length);
    if (status != CM_HIR_METADATA_OK) return status;
    out_envelope->major = major;
    out_envelope->minor = minor;
    out_envelope->flags = flags;
    out_envelope->payload = payload;
    out_envelope->payload_length = payload_length;
    return CM_HIR_METADATA_OK;
}

CmHirMetadataStatus cm_hir_metadata_decode_envelope(const void *encoded,
    size_t encoded_length, CmHirMetadataEnvelope *out_envelope)
{
    return cm_hir_metadata_decode_envelope_version(encoded, encoded_length,
        (uint16_t)CM_HIR_METADATA_MAJOR,
        (uint16_t)CM_HIR_METADATA_MINOR, out_envelope);
}

const char *cm_hir_metadata_status_name(CmHirMetadataStatus status)
{
    switch (status) {
    case CM_HIR_METADATA_OK: return "ok";
    case CM_HIR_METADATA_DONE: return "done";
    case CM_HIR_METADATA_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_METADATA_LIMIT_EXCEEDED: return "limit exceeded";
    case CM_HIR_METADATA_TRUNCATED: return "truncated";
    case CM_HIR_METADATA_TRAILING_BYTES: return "trailing bytes";
    case CM_HIR_METADATA_WRONG_MAGIC: return "wrong magic";
    case CM_HIR_METADATA_UNSUPPORTED_VERSION: return "unsupported version";
    case CM_HIR_METADATA_UNSUPPORTED_FLAGS: return "unsupported flags";
    case CM_HIR_METADATA_INVALID_HEADER_LENGTH:
        return "invalid header length";
    case CM_HIR_METADATA_NONZERO_RESERVED: return "nonzero reserved field";
    case CM_HIR_METADATA_LENGTH_OVERFLOW: return "length overflow";
    case CM_HIR_METADATA_CRC_MISMATCH: return "CRC mismatch";
    }
    return "unknown metadata status";
}
