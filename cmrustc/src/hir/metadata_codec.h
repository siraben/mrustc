#ifndef CMRUSTC_HIR_METADATA_CODEC_H
#define CMRUSTC_HIR_METADATA_CODEC_H

#include "cm/buf.h"

#define CM_HIR_METADATA_MAGIC_SIZE 8u
#define CM_HIR_METADATA_HEADER_SIZE 40u
#define CM_HIR_METADATA_SECTION_HEADER_SIZE 12u
#define CM_HIR_METADATA_MAX_PAYLOAD_SIZE 67108864u
#define CM_HIR_METADATA_MAJOR 1u
#define CM_HIR_METADATA_MINOR 0u
#define CM_HIR_METADATA_SEMANTIC_MINOR 1u
#define CM_HIR_METADATA_DECLARATION_MAJOR 2u
#define CM_HIR_METADATA_DECLARATION_MINOR 3u
#define CM_HIR_METADATA_SUPPORTED_FLAGS UINT32_C(0)

typedef enum CmHirMetadataStatus {
    CM_HIR_METADATA_OK = 0,
    CM_HIR_METADATA_DONE,
    CM_HIR_METADATA_INVALID_ARGUMENT,
    CM_HIR_METADATA_LIMIT_EXCEEDED,
    CM_HIR_METADATA_TRUNCATED,
    CM_HIR_METADATA_TRAILING_BYTES,
    CM_HIR_METADATA_WRONG_MAGIC,
    CM_HIR_METADATA_UNSUPPORTED_VERSION,
    CM_HIR_METADATA_UNSUPPORTED_FLAGS,
    CM_HIR_METADATA_INVALID_HEADER_LENGTH,
    CM_HIR_METADATA_NONZERO_RESERVED,
    CM_HIR_METADATA_LENGTH_OVERFLOW,
    CM_HIR_METADATA_CRC_MISMATCH
} CmHirMetadataStatus;

typedef struct CmHirMetadataWriter {
    CmByteBuf *buffer;
    size_t maximum_length;
    CmHirMetadataStatus status;
} CmHirMetadataWriter;

typedef struct CmHirMetadataReader {
    const unsigned char *data;
    size_t length;
    size_t cursor;
    CmHirMetadataStatus status;
} CmHirMetadataReader;

typedef struct CmHirMetadataSection {
    unsigned char tag[4];
    const unsigned char *data;
    size_t length;
} CmHirMetadataSection;

typedef struct CmHirMetadataEnvelope {
    uint16_t major;
    uint16_t minor;
    uint32_t flags;
    const unsigned char *payload;
    size_t payload_length;
} CmHirMetadataEnvelope;

/* Appends explicit little-endian values without serializing C objects. */
void cm_hir_metadata_writer_init(CmHirMetadataWriter *writer,
    CmByteBuf *buffer, size_t maximum_length);
CmHirMetadataStatus cm_hir_metadata_writer_status(
    const CmHirMetadataWriter *writer);
CmHirMetadataStatus cm_hir_metadata_write_u8(CmHirMetadataWriter *writer,
    uint8_t value);
CmHirMetadataStatus cm_hir_metadata_write_u16(CmHirMetadataWriter *writer,
    uint16_t value);
CmHirMetadataStatus cm_hir_metadata_write_u32(CmHirMetadataWriter *writer,
    uint32_t value);
CmHirMetadataStatus cm_hir_metadata_write_u64(CmHirMetadataWriter *writer,
    uint64_t value);
CmHirMetadataStatus cm_hir_metadata_write_bytes(CmHirMetadataWriter *writer,
    const void *data, size_t length);

/* Appends one `[tag: 4][length: u64-le][contents]` frame atomically. */
CmHirMetadataStatus cm_hir_metadata_write_section(
    CmHirMetadataWriter *writer, const unsigned char tag[4],
    const void *data, size_t length);

/* Bounded reads leave the cursor and output unchanged on failure. */
void cm_hir_metadata_reader_init(CmHirMetadataReader *reader,
    const void *data, size_t length);
CmHirMetadataStatus cm_hir_metadata_reader_status(
    const CmHirMetadataReader *reader);
size_t cm_hir_metadata_reader_remaining(const CmHirMetadataReader *reader);
CmHirMetadataStatus cm_hir_metadata_read_u8(CmHirMetadataReader *reader,
    uint8_t *out_value);
CmHirMetadataStatus cm_hir_metadata_read_u16(CmHirMetadataReader *reader,
    uint16_t *out_value);
CmHirMetadataStatus cm_hir_metadata_read_u32(CmHirMetadataReader *reader,
    uint32_t *out_value);
CmHirMetadataStatus cm_hir_metadata_read_u64(CmHirMetadataReader *reader,
    uint64_t *out_value);
CmHirMetadataStatus cm_hir_metadata_read_bytes(CmHirMetadataReader *reader,
    size_t length, const unsigned char **out_data);
CmHirMetadataStatus cm_hir_metadata_reader_finish(
    CmHirMetadataReader *reader);

/* Returns DONE only at an exact section-stream boundary. */
CmHirMetadataStatus cm_hir_metadata_read_section(
    CmHirMetadataReader *reader, CmHirMetadataSection *out_section);
CmHirMetadataStatus cm_hir_metadata_validate_sections(
    const void *data, size_t length);

/* Standard CRC-32/ISO-HDLC (IEEE polynomial), with the usual zero seed. */
uint32_t cm_hir_metadata_crc32(const void *data, size_t length);

/*
 * Encodes or validates one complete cmhir-meta-v1 envelope. `payload` must be
 * a complete framed-section stream. Encoding replaces output only on success.
 */
CmHirMetadataStatus cm_hir_metadata_encode_envelope(CmByteBuf *output,
    uint32_t flags, const void *payload, size_t payload_length);
CmHirMetadataStatus cm_hir_metadata_decode_envelope(const void *encoded,
    size_t encoded_length, CmHirMetadataEnvelope *out_envelope);

/* Internal additive-version entry points; the legacy wrappers remain v1.0. */
CmHirMetadataStatus cm_hir_metadata_encode_envelope_version(
    CmByteBuf *output, uint16_t major, uint16_t minor, uint32_t flags,
    const void *payload, size_t payload_length);
CmHirMetadataStatus cm_hir_metadata_decode_envelope_version(
    const void *encoded, size_t encoded_length, uint16_t expected_major,
    uint16_t expected_minor, CmHirMetadataEnvelope *out_envelope);

const char *cm_hir_metadata_status_name(CmHirMetadataStatus status);

#endif
