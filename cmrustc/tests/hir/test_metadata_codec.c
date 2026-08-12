#include "metadata_codec.h"

#include <assert.h>
#include <string.h>

#define TEST_MAJOR_OFFSET 8u
#define TEST_MINOR_OFFSET 10u
#define TEST_FLAGS_OFFSET 12u
#define TEST_HEADER_LENGTH_OFFSET 16u
#define TEST_RESERVED_BEFORE_OFFSET 20u
#define TEST_PAYLOAD_LENGTH_OFFSET 24u
#define TEST_CRC_OFFSET 32u
#define TEST_RESERVED_AFTER_OFFSET 36u
#define TEST_PAYLOAD_OFFSET CM_HIR_METADATA_HEADER_SIZE

static void test_put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8u) & UINT16_C(0xff));
}

static void test_put_u32(unsigned char *bytes, uint32_t value)
{
    unsigned int index;

    for (index = 0u; index < 4u; ++index) {
        bytes[index] = (unsigned char)(value & UINT32_C(0xff));
        value >>= 8u;
    }
}

static void test_put_u64(unsigned char *bytes, uint64_t value)
{
    unsigned int index;

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (unsigned char)(value & UINT64_C(0xff));
        value >>= 8u;
    }
}

static CmByteBuf test_copy_bytes(const unsigned char *bytes, size_t length)
{
    CmByteBuf copy;

    cm_byte_buf_init(&copy);
    cm_byte_buf_append(&copy, bytes, length);
    return copy;
}

static void test_recompute_crc(CmByteBuf *encoded)
{
    uint32_t crc;

    assert(encoded->len >= TEST_PAYLOAD_OFFSET);
    crc = cm_hir_metadata_crc32(encoded->data + TEST_PAYLOAD_OFFSET,
        encoded->len - TEST_PAYLOAD_OFFSET);
    test_put_u32(encoded->data + TEST_CRC_OFFSET, crc);
}

static void test_expect_decode(const unsigned char *bytes, size_t length,
    CmHirMetadataStatus expected)
{
    CmHirMetadataEnvelope envelope;

    memset(&envelope, 0xa5, sizeof(envelope));
    assert(cm_hir_metadata_decode_envelope(bytes, length, &envelope)
        == expected);
    if (expected != CM_HIR_METADATA_OK) {
        CmHirMetadataEnvelope zero;

        memset(&zero, 0, sizeof(zero));
        assert(memcmp(&envelope, &zero, sizeof(envelope)) == 0);
    }
}

static void test_integer_primitives(void)
{
    static const unsigned char suffix[3] = {
        UINT8_C(0x5a), UINT8_C(0x00), UINT8_C(0xff)
    };
    static const unsigned char expected[] = {
        UINT8_C(0xab),
        UINT8_C(0x34), UINT8_C(0x12),
        UINT8_C(0xef), UINT8_C(0xcd), UINT8_C(0xab), UINT8_C(0x89),
        UINT8_C(0xef), UINT8_C(0xcd), UINT8_C(0xab), UINT8_C(0x89),
        UINT8_C(0x67), UINT8_C(0x45), UINT8_C(0x23), UINT8_C(0x01),
        UINT8_C(0x5a), UINT8_C(0x00), UINT8_C(0xff)
    };
    CmByteBuf bytes;
    CmHirMetadataWriter writer;
    CmHirMetadataReader reader;
    const unsigned char *read_suffix;
    uint8_t value8;
    uint16_t value16;
    uint32_t value32;
    uint64_t value64;

    cm_byte_buf_init(&bytes);
    cm_hir_metadata_writer_init(&writer, &bytes, sizeof(expected));
    assert(cm_hir_metadata_write_u8(&writer, UINT8_C(0xab))
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_write_u16(&writer, UINT16_C(0x1234))
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_write_u32(&writer, UINT32_C(0x89abcdef))
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_write_u64(&writer,
        UINT64_C(0x0123456789abcdef)) == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_write_bytes(&writer, suffix, sizeof(suffix))
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_writer_status(&writer) == CM_HIR_METADATA_OK);
    assert(bytes.len == sizeof(expected));
    assert(memcmp(bytes.data, expected, sizeof(expected)) == 0);

    cm_hir_metadata_reader_init(&reader, bytes.data, bytes.len);
    assert(cm_hir_metadata_read_u8(&reader, &value8) == CM_HIR_METADATA_OK);
    assert(value8 == UINT8_C(0xab));
    assert(cm_hir_metadata_read_u16(&reader, &value16)
        == CM_HIR_METADATA_OK);
    assert(value16 == UINT16_C(0x1234));
    assert(cm_hir_metadata_read_u32(&reader, &value32)
        == CM_HIR_METADATA_OK);
    assert(value32 == UINT32_C(0x89abcdef));
    assert(cm_hir_metadata_read_u64(&reader, &value64)
        == CM_HIR_METADATA_OK);
    assert(value64 == UINT64_C(0x0123456789abcdef));
    assert(cm_hir_metadata_read_bytes(&reader, sizeof(suffix), &read_suffix)
        == CM_HIR_METADATA_OK);
    assert(memcmp(read_suffix, suffix, sizeof(suffix)) == 0);
    assert(cm_hir_metadata_reader_remaining(&reader) == 0u);
    assert(cm_hir_metadata_reader_finish(&reader) == CM_HIR_METADATA_OK);
    cm_byte_buf_destroy(&bytes);
}

static void test_primitive_bounds(void)
{
    static const unsigned char bytes[3] = {
        UINT8_C(1), UINT8_C(2), UINT8_C(3)
    };
    CmByteBuf output;
    CmHirMetadataWriter writer;
    CmHirMetadataReader reader;
    const unsigned char *pointer;
    uint32_t value;
    size_t cursor;

    cm_byte_buf_init(&output);
    cm_hir_metadata_writer_init(&writer, &output, 2u);
    assert(cm_hir_metadata_write_u16(&writer, UINT16_C(0x0102))
        == CM_HIR_METADATA_OK);
    assert(output.len == 2u);
    assert(cm_hir_metadata_write_u8(&writer, UINT8_C(3))
        == CM_HIR_METADATA_LIMIT_EXCEEDED);
    assert(output.len == 2u);
    assert(cm_hir_metadata_write_u8(&writer, UINT8_C(4))
        == CM_HIR_METADATA_LIMIT_EXCEEDED);
    assert(output.len == 2u);
    cm_byte_buf_destroy(&output);

    cm_hir_metadata_reader_init(&reader, bytes, sizeof(bytes));
    value = UINT32_C(0xdeadbeef);
    cursor = reader.cursor;
    assert(cm_hir_metadata_read_u32(&reader, &value)
        == CM_HIR_METADATA_TRUNCATED);
    assert(value == UINT32_C(0xdeadbeef));
    assert(reader.cursor == cursor);

    cm_hir_metadata_reader_init(&reader, bytes, sizeof(bytes));
    pointer = (const unsigned char *)(uintptr_t)UINTPTR_MAX;
    cursor = reader.cursor;
    assert(cm_hir_metadata_read_bytes(&reader, 4u, &pointer)
        == CM_HIR_METADATA_TRUNCATED);
    assert(pointer == (const unsigned char *)(uintptr_t)UINTPTR_MAX);
    assert(reader.cursor == cursor);

    cm_hir_metadata_reader_init(&reader, bytes, sizeof(bytes));
    assert(cm_hir_metadata_read_bytes(&reader, 1u, &pointer)
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_reader_finish(&reader)
        == CM_HIR_METADATA_TRAILING_BYTES);
    assert(cm_hir_metadata_reader_status(&reader)
        == CM_HIR_METADATA_TRAILING_BYTES);

    cm_hir_metadata_reader_init(&reader, NULL, 1u);
    assert(cm_hir_metadata_reader_status(&reader)
        == CM_HIR_METADATA_INVALID_ARGUMENT);
    cm_hir_metadata_writer_init(&writer, NULL, 0u);
    assert(cm_hir_metadata_writer_status(&writer)
        == CM_HIR_METADATA_INVALID_ARGUMENT);
}

static void test_sections(CmByteBuf *out_payload)
{
    static const unsigned char tag_test[4] = {
        (unsigned char)'T', (unsigned char)'E',
        (unsigned char)'S', (unsigned char)'T'
    };
    static const unsigned char tag_zero[4] = {
        (unsigned char)'Z', (unsigned char)'E',
        (unsigned char)'R', (unsigned char)'O'
    };
    static const unsigned char contents[3] = {
        UINT8_C(1), UINT8_C(2), UINT8_C(3)
    };
    static const unsigned char expected[] = {
        (unsigned char)'T', (unsigned char)'E',
        (unsigned char)'S', (unsigned char)'T',
        UINT8_C(3), UINT8_C(0), UINT8_C(0), UINT8_C(0),
        UINT8_C(0), UINT8_C(0), UINT8_C(0), UINT8_C(0),
        UINT8_C(1), UINT8_C(2), UINT8_C(3),
        (unsigned char)'Z', (unsigned char)'E',
        (unsigned char)'R', (unsigned char)'O',
        UINT8_C(0), UINT8_C(0), UINT8_C(0), UINT8_C(0),
        UINT8_C(0), UINT8_C(0), UINT8_C(0), UINT8_C(0)
    };
    CmHirMetadataWriter writer;
    CmHirMetadataReader reader;
    CmHirMetadataSection section;
    CmHirMetadataSection saved;
    CmByteBuf malformed;

    cm_byte_buf_init(out_payload);
    cm_hir_metadata_writer_init(&writer, out_payload, sizeof(expected));
    assert(cm_hir_metadata_write_section(&writer, tag_test, contents,
        sizeof(contents)) == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_write_section(&writer, tag_zero, NULL, 0u)
        == CM_HIR_METADATA_OK);
    assert(out_payload->len == sizeof(expected));
    assert(memcmp(out_payload->data, expected, sizeof(expected)) == 0);
    assert(cm_hir_metadata_validate_sections(out_payload->data,
        out_payload->len) == CM_HIR_METADATA_OK);

    cm_hir_metadata_reader_init(&reader, out_payload->data, out_payload->len);
    assert(cm_hir_metadata_read_section(&reader, &section)
        == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, tag_test, sizeof(tag_test)) == 0);
    assert(section.length == sizeof(contents));
    assert(memcmp(section.data, contents, sizeof(contents)) == 0);
    assert(cm_hir_metadata_read_section(&reader, &section)
        == CM_HIR_METADATA_OK);
    assert(memcmp(section.tag, tag_zero, sizeof(tag_zero)) == 0);
    assert(section.length == 0u);
    saved = section;
    assert(cm_hir_metadata_read_section(&reader, &section)
        == CM_HIR_METADATA_DONE);
    assert(memcmp(&section, &saved, sizeof(section)) == 0);

    malformed = test_copy_bytes(expected, 11u);
    memset(&section, 0xa5, sizeof(section));
    saved = section;
    cm_hir_metadata_reader_init(&reader, malformed.data, malformed.len);
    assert(cm_hir_metadata_read_section(&reader, &section)
        == CM_HIR_METADATA_TRUNCATED);
    assert(reader.cursor == 0u);
    assert(memcmp(&section, &saved, sizeof(section)) == 0);
    cm_byte_buf_destroy(&malformed);

    malformed = test_copy_bytes(expected, sizeof(expected));
    test_put_u64(malformed.data + 4u, UINT64_MAX);
    cm_hir_metadata_reader_init(&reader, malformed.data, malformed.len);
    assert(cm_hir_metadata_read_section(&reader, &section)
        == CM_HIR_METADATA_LENGTH_OVERFLOW);
    assert(reader.cursor == 0u);
    cm_byte_buf_destroy(&malformed);

    cm_byte_buf_init(&malformed);
    cm_hir_metadata_writer_init(&writer, &malformed, 14u);
    assert(cm_hir_metadata_write_section(&writer, tag_test, contents,
        sizeof(contents)) == CM_HIR_METADATA_LIMIT_EXCEEDED);
    assert(malformed.len == 0u);
    cm_byte_buf_destroy(&malformed);

    malformed = test_copy_bytes(contents, sizeof(contents));
    cm_hir_metadata_writer_init(&writer, &malformed,
        sizeof(contents) + CM_HIR_METADATA_SECTION_HEADER_SIZE
            + sizeof(contents));
    assert(cm_hir_metadata_write_section(&writer, tag_test,
        malformed.data, malformed.len) == CM_HIR_METADATA_OK);
    assert(malformed.len == sizeof(contents)
        + CM_HIR_METADATA_SECTION_HEADER_SIZE + sizeof(contents));
    assert(memcmp(malformed.data, contents, sizeof(contents)) == 0);
    assert(memcmp(malformed.data + sizeof(contents), tag_test,
        sizeof(tag_test)) == 0);
    assert(memcmp(malformed.data + sizeof(contents)
        + CM_HIR_METADATA_SECTION_HEADER_SIZE, contents,
        sizeof(contents)) == 0);
    cm_byte_buf_destroy(&malformed);
}

static void test_envelope(const CmByteBuf *payload)
{
    static const unsigned char expected[] = {
        UINT8_C(0x43), UINT8_C(0x4d), UINT8_C(0x48), UINT8_C(0x49),
        UINT8_C(0x52), UINT8_C(0x4d), UINT8_C(0x31), UINT8_C(0x00),
        UINT8_C(0x01), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x28), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x1b), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x1a), UINT8_C(0xa8), UINT8_C(0xfa), UINT8_C(0x36),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x54), UINT8_C(0x45), UINT8_C(0x53), UINT8_C(0x54),
        UINT8_C(0x03), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x01), UINT8_C(0x02), UINT8_C(0x03),
        UINT8_C(0x5a), UINT8_C(0x45), UINT8_C(0x52), UINT8_C(0x4f),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00),
        UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00), UINT8_C(0x00)
    };
    CmByteBuf first;
    CmByteBuf second;
    CmByteBuf changed;
    CmHirMetadataEnvelope envelope;
    size_t length;

    cm_byte_buf_init(&first);
    cm_byte_buf_init(&second);
    assert(cm_hir_metadata_encode_envelope(&first, UINT32_C(0), payload->data,
        payload->len) == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_encode_envelope(&second, UINT32_C(0), payload->data,
        payload->len) == CM_HIR_METADATA_OK);
    assert(first.len == sizeof(expected));
    assert(second.len == first.len);
    assert(memcmp(first.data, expected, sizeof(expected)) == 0);
    assert(memcmp(second.data, first.data, first.len) == 0);
    assert(cm_hir_metadata_decode_envelope(first.data, first.len, &envelope)
        == CM_HIR_METADATA_OK);
    assert(envelope.major == (uint16_t)CM_HIR_METADATA_MAJOR);
    assert(envelope.minor == (uint16_t)CM_HIR_METADATA_MINOR);
    assert(envelope.flags == UINT32_C(0));
    assert(envelope.payload_length == payload->len);
    assert(memcmp(envelope.payload, payload->data, payload->len) == 0);
    assert(cm_hir_metadata_crc32("123456789", 9u)
        == UINT32_C(0xcbf43926));

    for (length = 0u; length < first.len; ++length) {
        test_expect_decode(first.data, length, CM_HIR_METADATA_TRUNCATED);
    }

    changed = test_copy_bytes(first.data, first.len);
    cm_byte_buf_push(&changed, UINT8_C(0));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_TRAILING_BYTES);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    changed.data[0] ^= UINT8_C(1);
    test_expect_decode(changed.data, changed.len, CM_HIR_METADATA_WRONG_MAGIC);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u16(changed.data + TEST_MAJOR_OFFSET, UINT16_C(2));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_UNSUPPORTED_VERSION);
    test_put_u16(changed.data + TEST_MAJOR_OFFSET, UINT16_C(1));
    test_put_u16(changed.data + TEST_MINOR_OFFSET, UINT16_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_UNSUPPORTED_VERSION);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u32(changed.data + TEST_FLAGS_OFFSET, UINT32_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_UNSUPPORTED_FLAGS);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u32(changed.data + TEST_HEADER_LENGTH_OFFSET, UINT32_C(39));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_INVALID_HEADER_LENGTH);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u32(changed.data + TEST_RESERVED_BEFORE_OFFSET, UINT32_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_NONZERO_RESERVED);
    test_put_u32(changed.data + TEST_RESERVED_BEFORE_OFFSET, UINT32_C(0));
    test_put_u32(changed.data + TEST_RESERVED_AFTER_OFFSET, UINT32_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_NONZERO_RESERVED);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u64(changed.data + TEST_PAYLOAD_LENGTH_OFFSET,
        (uint64_t)payload->len - UINT64_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_TRAILING_BYTES);
    test_put_u64(changed.data + TEST_PAYLOAD_LENGTH_OFFSET,
        (uint64_t)payload->len + UINT64_C(1));
    test_expect_decode(changed.data, changed.len, CM_HIR_METADATA_TRUNCATED);
    test_put_u64(changed.data + TEST_PAYLOAD_LENGTH_OFFSET, UINT64_MAX);
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_LENGTH_OVERFLOW);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u64(changed.data + TEST_PAYLOAD_LENGTH_OFFSET,
        (uint64_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE + UINT64_C(1));
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_LIMIT_EXCEEDED);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    changed.data[TEST_CRC_OFFSET] ^= UINT8_C(1);
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_CRC_MISMATCH);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    test_put_u64(changed.data + TEST_PAYLOAD_OFFSET + 4u, UINT64_MAX);
    test_recompute_crc(&changed);
    test_expect_decode(changed.data, changed.len,
        CM_HIR_METADATA_LENGTH_OVERFLOW);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(first.data, first.len);
    cm_byte_buf_push(&changed, UINT8_C(0xa5));
    test_put_u64(changed.data + TEST_PAYLOAD_LENGTH_OFFSET,
        (uint64_t)(changed.len - TEST_PAYLOAD_OFFSET));
    test_recompute_crc(&changed);
    test_expect_decode(changed.data, changed.len, CM_HIR_METADATA_TRUNCATED);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes((const unsigned char *)"keep", 4u);
    assert(cm_hir_metadata_encode_envelope(&changed, UINT32_C(1), payload->data,
        payload->len) == CM_HIR_METADATA_UNSUPPORTED_FLAGS);
    assert(changed.len == 4u && memcmp(changed.data, "keep", 4u) == 0);
    assert(cm_hir_metadata_encode_envelope(&changed, UINT32_C(0), "x", 1u)
        == CM_HIR_METADATA_TRUNCATED);
    assert(changed.len == 4u && memcmp(changed.data, "keep", 4u) == 0);
    assert(cm_hir_metadata_encode_envelope(&changed, UINT32_C(0),
        (const void *)(uintptr_t)1u,
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE + 1u)
        == CM_HIR_METADATA_LIMIT_EXCEEDED);
    assert(changed.len == 4u && memcmp(changed.data, "keep", 4u) == 0);
    cm_byte_buf_destroy(&changed);

    changed = test_copy_bytes(payload->data, payload->len);
    assert(cm_hir_metadata_encode_envelope(&changed, UINT32_C(0), changed.data,
        changed.len) == CM_HIR_METADATA_OK);
    assert(changed.len == first.len);
    assert(memcmp(changed.data, first.data, first.len) == 0);
    cm_byte_buf_destroy(&changed);

    assert(strcmp(cm_hir_metadata_status_name(CM_HIR_METADATA_CRC_MISMATCH),
        "CRC mismatch") == 0);
    cm_byte_buf_destroy(&second);
    cm_byte_buf_destroy(&first);
}

static void test_empty_envelope(void)
{
    CmByteBuf encoded;
    CmHirMetadataEnvelope envelope;

    cm_byte_buf_init(&encoded);
    assert(cm_hir_metadata_encode_envelope(&encoded, UINT32_C(0), NULL, 0u)
        == CM_HIR_METADATA_OK);
    assert(encoded.len == CM_HIR_METADATA_HEADER_SIZE);
    assert(cm_hir_metadata_decode_envelope(encoded.data, encoded.len,
        &envelope)
        == CM_HIR_METADATA_OK);
    assert(envelope.payload_length == 0u);
    assert(envelope.payload == encoded.data + CM_HIR_METADATA_HEADER_SIZE);
    assert(cm_hir_metadata_encode_envelope_version(&encoded,
        (uint16_t)CM_HIR_METADATA_MAJOR,
        (uint16_t)CM_HIR_METADATA_SEMANTIC_MINOR, UINT32_C(0), NULL, 0u)
        == CM_HIR_METADATA_OK);
    assert(cm_hir_metadata_decode_envelope(encoded.data, encoded.len,
        &envelope) == CM_HIR_METADATA_UNSUPPORTED_VERSION);
    assert(cm_hir_metadata_decode_envelope_version(encoded.data,
        encoded.len, (uint16_t)CM_HIR_METADATA_MAJOR,
        (uint16_t)CM_HIR_METADATA_SEMANTIC_MINOR, &envelope)
        == CM_HIR_METADATA_OK);
    assert(envelope.major == (uint16_t)CM_HIR_METADATA_MAJOR);
    assert(envelope.minor == (uint16_t)CM_HIR_METADATA_SEMANTIC_MINOR);
    cm_byte_buf_destroy(&encoded);
}

int main(void)
{
    CmByteBuf payload;

    test_integer_primitives();
    test_primitive_bounds();
    test_sections(&payload);
    test_envelope(&payload);
    test_empty_envelope();
    cm_byte_buf_destroy(&payload);
    return 0;
}
