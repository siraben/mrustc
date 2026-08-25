#include "cm/hir/executable_metadata.h"

#include "cm/alloc.h"
#include "cm/sha256.h"
#include "metadata_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CM_EXEC_SECTION_COUNT 14u
#define CM_EXEC_FAMILY_COUNT 14u
#define CM_EXEC_STRING_MAX ((size_t)1048576u)

typedef struct CmExecFamily {
    uint8_t state;
    uint32_t count;
    uint32_t crc;
} CmExecFamily;

static const unsigned char cm_exec_tags[CM_EXEC_SECTION_COUNT][4] = {
    { 'C', 'R', 'A', 'T' }, { 'M', 'A', 'N', 'F' },
    { 'M', 'O', 'D', 'S' }, { 'N', 'O', 'M', 'D' },
    { 'A', 'I', 'T', 'M' }, { 'G', 'P', 'A', 'R' },
    { 'T', 'Y', 'P', 'E' }, { 'I', 'T', 'E', 'M' },
    { 'V', 'A', 'L', 'U' }, { 'P', 'R', 'E', 'D' },
    { 'I', 'M', 'P', 'L' }, { 'N', 'S', 'P', 'C' },
    { 'B', 'O', 'D', 'Y' }, { 'L', 'I', 'N', 'K' }
};

static int cm_exec_size_count(size_t count)
{
    return count <= CM_HIR_EXEC_METADATA_MAX_RECORDS
        && count <= (size_t)UINT32_MAX;
}

static int cm_exec_string_valid(CmHirExecutableString value,
    size_t minimum, size_t maximum)
{
    size_t index;
    if (value.length < minimum || value.length > maximum
        || (value.length != 0u && value.data == NULL)) return 0;
    for (index = 0u; index < value.length; index += 1u) {
        if (value.data[index] == 0u) return 0;
    }
    return 1;
}

static int cm_exec_bytes_valid(CmHirExecutableString value,
    size_t minimum, size_t maximum)
{
    return value.length >= minimum && value.length <= maximum
        && (value.length == 0u || value.data != NULL);
}

static int cm_exec_string_compare(CmHirExecutableString left,
    CmHirExecutableString right)
{
    size_t common = left.length < right.length ? left.length : right.length;
    int result = common == 0u ? 0 : memcmp(left.data, right.data, common);
    if (result != 0) return result;
    return left.length < right.length ? -1 : left.length > right.length;
}

static int cm_exec_string_equal(CmHirExecutableString left,
    CmHirExecutableString right)
{
    return left.length == right.length
        && (left.length == 0u || memcmp(left.data, right.data,
            left.length) == 0);
}

static int cm_exec_qsort_string_compare(const void *left, const void *right)
{
    return cm_exec_string_compare(*(const CmHirExecutableString *)left,
        *(const CmHirExecutableString *)right);
}

static int cm_exec_digest_equal(const CmHirArtifactDigest *left,
    const CmHirArtifactDigest *right)
{
    return memcmp(left->bytes, right->bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE) == 0;
}

static int cm_exec_portable_identifier(CmHirExecutableString value)
{
    size_t index;
    unsigned char byte;
    if (!cm_exec_string_valid(value, 1u, 255u)) return 0;
    byte = value.data[0];
    if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
        || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
        || byte == (unsigned char)'_')) return 0;
    for (index = 1u; index < value.length; index += 1u) {
        byte = value.data[index];
        if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
            || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
            || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')
            || byte == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_exec_member_name(CmHirExecutableString value)
{
    size_t index;
    if (!cm_exec_string_valid(value, 1u, 15u)) return 0;
    if (value.length == 13u
        && memcmp(value.data, "cmrustc.rmeta", 13u) == 0) return 0;
    for (index = 0u; index < value.length; index += 1u) {
        unsigned char byte = value.data[index];
        if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
            || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
            || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')
            || byte == (unsigned char)'_' || byte == (unsigned char)'.'
            || byte == (unsigned char)'-')) return 0;
    }
    return 1;
}

static int cm_exec_primitive(uint8_t value)
{
    return value >= (uint8_t)CM_HIR_EXEC_PRIMITIVE_BOOL
        && value <= (uint8_t)CM_HIR_EXEC_PRIMITIVE_F64;
}

static CmHirMetadataStatus cm_exec_write_string(CmHirMetadataWriter *writer,
    CmHirExecutableString value)
{
    CmHirMetadataStatus status;
    if (value.length > (size_t)UINT32_MAX) return CM_HIR_METADATA_LIMIT_EXCEEDED;
    status = cm_hir_metadata_write_u32(writer, (uint32_t)value.length);
    if (status == CM_HIR_METADATA_OK) {
        status = cm_hir_metadata_write_bytes(writer, value.data, value.length);
    }
    return status;
}

static void cm_exec_writer(CmHirMetadataWriter *writer, CmByteBuf *buffer)
{
    cm_byte_buf_init(buffer);
    cm_hir_metadata_writer_init(writer, buffer,
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
}

static CmHirExecutableMetadataStatus cm_exec_writer_status(
    const CmHirMetadataWriter *writer)
{
    CmHirMetadataStatus status = cm_hir_metadata_writer_status(writer);
    return status == CM_HIR_METADATA_OK ? CM_HIR_EXEC_METADATA_OK
        : status == CM_HIR_METADATA_LIMIT_EXCEEDED
            ? CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED
            : CM_HIR_EXEC_METADATA_INVALID_FORMAT;
}

static void cm_exec_sha256(const void *data, size_t length,
    CmHirArtifactDigest *digest)
{
    CmSha256 sha;
    cm_sha256_init(&sha);
    cm_sha256_update(&sha, data, length);
    cm_sha256_final(&sha, digest->bytes);
}

void cm_hir_executable_metadata_init(CmHirExecutableMetadata *metadata)
{
    if (metadata != NULL) memset(metadata, 0, sizeof(*metadata));
}

static void cm_exec_free_string(CmHirExecutableString *string)
{
    cm_free(string->data);
    string->data = NULL;
    string->length = 0u;
}

void cm_hir_executable_metadata_destroy(CmHirExecutableMetadata *metadata)
{
    size_t index;
    if (metadata == NULL) return;
    if (!metadata->owns_storage) {
        memset(metadata, 0, sizeof(*metadata));
        return;
    }
    cm_exec_free_string(&metadata->crate_name);
    cm_exec_free_string(&metadata->crate_disambiguator);
    cm_exec_free_string(&metadata->target_descriptor);
    cm_exec_free_string(&metadata->panic_strategy);
    for (index = 0u; index < metadata->cfg_count; index += 1u)
        cm_exec_free_string(&metadata->cfgs[index]);
    for (index = 0u; index < metadata->module_count; index += 1u)
        cm_exec_free_string(&metadata->modules[index].name);
    for (index = 0u; index < metadata->trait_count; index += 1u)
        cm_exec_free_string(&metadata->traits[index].name);
    for (index = 0u; index < metadata->value_count; index += 1u) {
        cm_exec_free_string(&metadata->values[index].name);
        cm_exec_free_string(&metadata->values[index].generic_name);
        cm_free(metadata->values[index].parameter_types);
    }
    for (index = 0u; index < metadata->namespace_count; index += 1u)
        cm_exec_free_string(&metadata->namespace_entries[index].name);
    for (index = 0u; index < metadata->object_count; index += 1u)
        cm_exec_free_string(&metadata->objects[index].archive_member_name);
    for (index = 0u; index < metadata->symbol_count; index += 1u)
        cm_exec_free_string(&metadata->symbols[index].external_symbol);
    cm_free(metadata->cfgs);
    cm_free(metadata->modules);
    cm_free(metadata->traits);
    cm_free(metadata->types);
    cm_free(metadata->impls);
    cm_free(metadata->values);
    cm_free(metadata->predicates);
    cm_free(metadata->namespace_entries);
    cm_free(metadata->bodies);
    cm_free(metadata->objects);
    cm_free(metadata->symbols);
    memset(metadata, 0, sizeof(*metadata));
}

static int cm_exec_type_is_primitive(const CmHirExecutableMetadata *metadata,
    uint32_t local)
{
    return local != 0u && (size_t)local <= metadata->type_count
        && metadata->types[local - 1u].kind == CM_HIR_EXEC_TYPE_PRIMITIVE;
}

static int cm_exec_module_path_compare(
    const CmHirExecutableMetadata *metadata, size_t left, size_t right,
    int *valid)
{
    uint32_t left_chain[1024];
    uint32_t right_chain[1024];
    size_t left_count = 0u;
    size_t right_count = 0u;
    size_t index;
    while (left != 0u) {
        if (left_count == 1024u) {
            *valid = 0;
            return 0;
        }
        left_chain[left_count++] = (uint32_t)left;
        left = metadata->modules[left].parent_module - 1u;
    }
    while (right != 0u) {
        if (right_count == 1024u) {
            *valid = 0;
            return 0;
        }
        right_chain[right_count++] = (uint32_t)right;
        right = metadata->modules[right].parent_module - 1u;
    }
    for (index = 0u; index < left_count && index < right_count; index += 1u) {
        int order = cm_exec_string_compare(
            metadata->modules[left_chain[left_count - index - 1u]].name,
            metadata->modules[right_chain[right_count - index - 1u]].name);
        if (order != 0) return order;
    }
    return left_count < right_count ? -1 : left_count > right_count;
}

static int cm_exec_descriptor_identity_fields(
    const CmHirExecutableMetadata *metadata)
{
    size_t index;
    if (!cm_exec_string_valid(metadata->crate_name, 1u,
            CM_HIR_ARTIFACT_MAX_NAME_SIZE)
        || !cm_exec_string_valid(metadata->crate_disambiguator, 1u,
            CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE)
        || (metadata->edition != UINT32_C(2015)
            && metadata->edition != UINT32_C(2018)
            && metadata->edition != UINT32_C(2021)
            && metadata->edition != UINT32_C(2024))
        || !cm_exec_bytes_valid(metadata->target_descriptor, 1u,
            CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE)
        || !cm_exec_string_equal(metadata->panic_strategy,
            (CmHirExecutableString){ (unsigned char *)"abort", 5u })
        || metadata->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || (metadata->cfg_count != 0u && metadata->cfgs == NULL)) return 0;
    for (index = 0u; index < metadata->cfg_count; index += 1u) {
        if (!cm_exec_string_valid(metadata->cfgs[index], 1u,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE)
            || (index != 0u && cm_exec_string_compare(
                metadata->cfgs[index - 1u], metadata->cfgs[index]) >= 0)) {
            return 0;
        }
    }
    return 1;
}

static int cm_exec_source_digest_valid_for_encode(
    const CmHirExecutableMetadata *metadata)
{
    CmHirArtifactDigest digest;
    return metadata->source_entries != NULL
        && metadata->source_entry_count != 0u
        && cm_hir_artifact_source_closure_digest(metadata->source_entries,
            metadata->source_entry_count, &digest)
            == CM_HIR_ARTIFACT_IDENTITY_OK
        && cm_exec_digest_equal(&digest, &metadata->source_digest);
}

static int cm_exec_object_digests_valid_for_encode(
    const CmHirExecutableMetadata *metadata)
{
    CmHirArtifactDigest digest;
    size_t index;
    for (index = 0u; index < metadata->object_count; index += 1u) {
        const CmHirExecutableLinkObject *object = &metadata->objects[index];
        if (object->object_bytes == NULL || object->object_bytes_length == 0u
            || (uint64_t)object->object_bytes_length != object->byte_length)
            return 0;
        cm_exec_sha256(object->object_bytes, object->object_bytes_length,
            &digest);
        if (!cm_exec_digest_equal(&digest, &object->object_digest)) return 0;
    }
    return 1;
}

static int cm_exec_validate_namespace(const CmHirExecutableMetadata *metadata)
{
    size_t index;
    unsigned char *seen_modules;
    unsigned char *seen_traits;
    unsigned char *seen_values;
    int valid = 1;
    if (metadata->namespace_count
        != metadata->trait_count + metadata->value_count
            + (metadata->module_count - 1u)) return 0;
    seen_modules = cm_alloc_zeroed(metadata->module_count, 1u);
    seen_traits = cm_alloc_zeroed(metadata->trait_count, 1u);
    seen_values = cm_alloc_zeroed(metadata->value_count, 1u);
    for (index = 0u; index < metadata->namespace_count && valid; index += 1u) {
        const CmHirExecutableNamespaceEntry *entry
            = &metadata->namespace_entries[index];
        if (entry->owner_module == 0u
            || (size_t)entry->owner_module > metadata->module_count
            || !cm_exec_string_valid(entry->name, 1u, 255u)
            || (entry->namespace_kind != CM_HIR_EXEC_NAMESPACE_TYPE
                && entry->namespace_kind != CM_HIR_EXEC_NAMESPACE_VALUE)
            || (index != 0u && (metadata->namespace_entries[index - 1u].owner_module
                    > entry->owner_module
                || (metadata->namespace_entries[index - 1u].owner_module
                        == entry->owner_module
                    && (metadata->namespace_entries[index - 1u].namespace_kind
                            > entry->namespace_kind
                        || (metadata->namespace_entries[index - 1u].namespace_kind
                                == entry->namespace_kind
                            && cm_exec_string_compare(
                                metadata->namespace_entries[index - 1u].name,
                                entry->name) >= 0)))))) {
            valid = 0;
        } else if (entry->target_kind == CM_HIR_EXEC_NAMESPACE_MODULE) {
            uint32_t local = entry->target_local;
            valid = entry->namespace_kind == CM_HIR_EXEC_NAMESPACE_TYPE
                && local > 1u && (size_t)local <= metadata->module_count
                && metadata->modules[local - 1u].parent_module
                    == entry->owner_module
                && cm_exec_string_equal(metadata->modules[local - 1u].name,
                    entry->name) && !seen_modules[local - 1u];
            if (valid) seen_modules[local - 1u] = 1u;
        } else if (entry->target_kind == CM_HIR_EXEC_NAMESPACE_TRAIT) {
            uint32_t local = entry->target_local;
            valid = entry->namespace_kind == CM_HIR_EXEC_NAMESPACE_TYPE
                && local != 0u && (size_t)local <= metadata->trait_count
                && metadata->traits[local - 1u].owner_module
                    == entry->owner_module
                && cm_exec_string_equal(metadata->traits[local - 1u].name,
                    entry->name) && !seen_traits[local - 1u];
            if (valid) seen_traits[local - 1u] = 1u;
        } else if (entry->target_kind
                == CM_HIR_EXEC_NAMESPACE_VALUE_TARGET) {
            uint32_t local = entry->target_local;
            valid = entry->namespace_kind == CM_HIR_EXEC_NAMESPACE_VALUE
                && local != 0u && (size_t)local <= metadata->value_count
                && metadata->values[local - 1u].owner_module
                    == entry->owner_module
                && cm_exec_string_equal(metadata->values[local - 1u].name,
                    entry->name) && !seen_values[local - 1u];
            if (valid) seen_values[local - 1u] = 1u;
        } else valid = 0;
    }
    for (index = 1u; index < metadata->module_count; index += 1u)
        if (!seen_modules[index]) valid = 0;
    for (index = 0u; index < metadata->trait_count; index += 1u)
        if (!seen_traits[index]) valid = 0;
    for (index = 0u; index < metadata->value_count; index += 1u)
        if (!seen_values[index]) valid = 0;
    cm_free(seen_modules);
    cm_free(seen_traits);
    cm_free(seen_values);
    return valid;
}

static CmHirExecutableMetadataStatus cm_exec_validate(
    const CmHirExecutableMetadata *metadata)
{
    size_t index;
    size_t predicate_cursor = 0u;
    size_t body_cursor = 0u;
    size_t symbol_cursor = 0u;
    unsigned char *seen_generic;
    uint32_t last_generic_owner = UINT32_C(0);
    int in_generic_types = 0;
    if (metadata == NULL || !cm_exec_descriptor_identity_fields(metadata)
        || metadata->module_count == 0u
        || !cm_exec_size_count(metadata->module_count)
        || !cm_exec_size_count(metadata->trait_count)
        || !cm_exec_size_count(metadata->type_count)
        || !cm_exec_size_count(metadata->impl_count)
        || !cm_exec_size_count(metadata->value_count)
        || !cm_exec_size_count(metadata->predicate_count)
        || !cm_exec_size_count(metadata->namespace_count)
        || !cm_exec_size_count(metadata->body_count)
        || metadata->object_count == 0u || metadata->object_count > 15u
        || !cm_exec_size_count(metadata->symbol_count)
        || metadata->trait_count == 0u || metadata->impl_count == 0u
        || metadata->value_count == 0u || metadata->body_count == 0u
        || metadata->symbol_count == 0u
        || metadata->modules == NULL || metadata->traits == NULL
        || metadata->types == NULL || metadata->impls == NULL
        || metadata->values == NULL || metadata->predicates == NULL
        || metadata->namespace_entries == NULL || metadata->bodies == NULL
        || metadata->objects == NULL || metadata->symbols == NULL) {
        return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    if (metadata->modules[0].parent_module != 0u
        || !cm_exec_string_equal(metadata->modules[0].name,
            metadata->crate_name)) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    for (index = 0u; index < metadata->module_count; index += 1u) {
        const CmHirExecutableModule *module = &metadata->modules[index];
        int path_valid = 1;
        if (!cm_exec_string_valid(module->name, 1u, 255u)
            || (index != 0u && (module->parent_module == 0u
                || (size_t)module->parent_module > index))) {
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
        if (index != 0u
            && (cm_exec_module_path_compare(metadata, index - 1u, index,
                    &path_valid) >= 0 || !path_valid))
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    for (index = 0u; index < metadata->trait_count; index += 1u) {
        const CmHirExecutableTrait *trait = &metadata->traits[index];
        if (trait->owner_module == 0u
            || (size_t)trait->owner_module > metadata->module_count
            || !cm_exec_string_valid(trait->name, 1u, 255u)
            || (index != 0u
                && (metadata->traits[index - 1u].owner_module
                        > trait->owner_module
                    || (metadata->traits[index - 1u].owner_module
                            == trait->owner_module
                        && (cm_exec_string_compare(
                                metadata->traits[index - 1u].name,
                                trait->name) > 0
                            || (cm_exec_string_equal(
                                    metadata->traits[index - 1u].name,
                                    trait->name)
                                && metadata->traits[index - 1u].source_ordinal
                                    >= trait->source_ordinal))))))
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    seen_generic = cm_alloc_zeroed(metadata->value_count, 1u);
    for (index = 0u; index < metadata->type_count; index += 1u) {
        const CmHirExecutableType *type = &metadata->types[index];
        if (type->kind == CM_HIR_EXEC_TYPE_PRIMITIVE) {
            if (in_generic_types || !cm_exec_primitive(type->primitive)
                || type->owner_value != 0u
                || type->generic_index != 0u
                || (index != 0u && metadata->types[index - 1u].kind
                        == CM_HIR_EXEC_TYPE_PRIMITIVE
                    && metadata->types[index - 1u].primitive >= type->primitive)) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
            }
        } else if (type->kind == CM_HIR_EXEC_TYPE_VALUE_GENERIC) {
            in_generic_types = 1;
            if (type->primitive != 0u || type->owner_value == 0u
                || (size_t)type->owner_value > metadata->value_count
                || type->generic_index != 0u
                || type->owner_value <= last_generic_owner
                || seen_generic[type->owner_value - 1u]
                || metadata->values[type->owner_value - 1u].kind
                    != CM_HIR_EXEC_VALUE_RECIPE) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
            }
            seen_generic[type->owner_value - 1u] = 1u;
            last_generic_owner = type->owner_value;
        } else {
            cm_free(seen_generic);
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
    }
    for (index = 0u; index < metadata->value_count; index += 1u) {
        const CmHirExecutableValue *value = &metadata->values[index];
        size_t parameter;
        if (value->owner_module == 0u
            || (size_t)value->owner_module > metadata->module_count
            || !cm_exec_string_valid(value->name, 1u, 255u)
            || value->parameter_count == 0u
            || value->parameter_count > CM_HIR_EXEC_METADATA_MAX_PARAMETERS
            || value->parameter_types == NULL || value->return_type == 0u
            || (size_t)value->return_type > metadata->type_count
            || value->execution_local == 0u
            || (index != 0u
                && (metadata->values[index - 1u].owner_module
                        > value->owner_module
                    || (metadata->values[index - 1u].owner_module
                            == value->owner_module
                        && (cm_exec_string_compare(
                                metadata->values[index - 1u].name,
                                value->name) > 0
                            || (cm_exec_string_equal(
                                    metadata->values[index - 1u].name,
                                    value->name)
                                && metadata->values[index - 1u].source_ordinal
                                    >= value->source_ordinal)))))) {
            cm_free(seen_generic);
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
        for (parameter = 0u; parameter < value->parameter_count; parameter += 1u)
            if (value->parameter_types[parameter] == 0u
                || (size_t)value->parameter_types[parameter]
                    > metadata->type_count) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
        if (value->kind == CM_HIR_EXEC_VALUE_RECIPE) {
            const CmHirExecutableBody *body;
            uint32_t common;
            if (!seen_generic[index]
                || !cm_exec_string_valid(value->generic_name, 1u, 255u)
                || value->predicate_count == 0u
                || (size_t)value->predicate_start != predicate_cursor + 1u
                || (size_t)value->predicate_count
                    > metadata->predicate_count - predicate_cursor
                || value->execution_local != (uint32_t)(body_cursor + 1u)
                || (size_t)value->execution_local > metadata->body_count
                || metadata->bodies[body_cursor].owner_value != index + 1u
                || metadata->bodies[body_cursor].parameter_index
                    >= value->parameter_count) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
            }
            body = &metadata->bodies[body_cursor];
            common = value->parameter_types[body->parameter_index];
            if (metadata->types[common - 1u].kind
                    != CM_HIR_EXEC_TYPE_VALUE_GENERIC
                || metadata->types[common - 1u].owner_value != index + 1u
                || value->return_type != common
                || body->parameter_type != common
                || body->return_type != common) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
            }
            predicate_cursor += value->predicate_count;
            body_cursor += 1u;
        } else if (value->kind == CM_HIR_EXEC_VALUE_NATIVE_OBJECT) {
            if (value->generic_name.length != 0u || value->predicate_count != 0u
                || value->predicate_start != 0u
                || (size_t)value->execution_local > metadata->symbol_count
                || metadata->symbols[value->execution_local - 1u].owner_value
                    != index + 1u || !cm_exec_type_is_primitive(metadata,
                        value->return_type)) {
                cm_free(seen_generic);
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
            }
            for (parameter = 0u; parameter < value->parameter_count; parameter += 1u)
                if (!cm_exec_type_is_primitive(metadata,
                        value->parameter_types[parameter])) {
                    cm_free(seen_generic);
                    return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
                }
        } else {
            cm_free(seen_generic);
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
    }
    cm_free(seen_generic);
    if (predicate_cursor != metadata->predicate_count
        || body_cursor != metadata->body_count) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    for (index = 0u; index < metadata->predicate_count; index += 1u) {
        const CmHirExecutablePredicate *predicate = &metadata->predicates[index];
        if (predicate->owner_value == 0u
            || (size_t)predicate->owner_value > metadata->value_count
            || predicate->trait_local == 0u
            || (size_t)predicate->trait_local > metadata->trait_count
            || predicate->subject_type == 0u
            || (size_t)predicate->subject_type > metadata->type_count
            || metadata->values[predicate->owner_value - 1u].kind
                != CM_HIR_EXEC_VALUE_RECIPE
            || metadata->types[predicate->subject_type - 1u].kind
                != CM_HIR_EXEC_TYPE_VALUE_GENERIC
            || metadata->types[predicate->subject_type - 1u].owner_value
                != predicate->owner_value
            || (index != 0u && (metadata->predicates[index - 1u].owner_value
                    > predicate->owner_value
                || (metadata->predicates[index - 1u].owner_value
                        == predicate->owner_value
                    && metadata->predicates[index - 1u].ordinal
                        >= predicate->ordinal)))) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    for (index = 0u; index < metadata->impl_count; index += 1u) {
        const CmHirExecutableImpl *impl = &metadata->impls[index];
        if (impl->owner_module == 0u
            || (size_t)impl->owner_module > metadata->module_count
            || impl->trait_local == 0u
            || (size_t)impl->trait_local > metadata->trait_count
            || !cm_exec_type_is_primitive(metadata, impl->self_type)
            || (index != 0u && (metadata->impls[index - 1u].owner_module
                    > impl->owner_module
                || (metadata->impls[index - 1u].owner_module
                        == impl->owner_module
                    && metadata->impls[index - 1u].source_ordinal
                        >= impl->source_ordinal)))) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    for (index = 0u; index < metadata->object_count; index += 1u) {
        const CmHirExecutableLinkObject *object = &metadata->objects[index];
        size_t object_symbol;
        if (!cm_exec_member_name(object->archive_member_name)
            || object->byte_length == 0u
            || object->byte_length > UINT64_C(134217728)
            || object->symbol_count == 0u
            || (size_t)object->symbol_start != symbol_cursor + 1u
            || (size_t)object->symbol_count
                > metadata->symbol_count - symbol_cursor
            || (index != 0u && cm_exec_string_compare(
                metadata->objects[index - 1u].archive_member_name,
                object->archive_member_name) >= 0)) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        for (object_symbol = symbol_cursor;
            object_symbol < symbol_cursor + object->symbol_count;
            object_symbol += 1u) {
            if (metadata->symbols[object_symbol].object_local != index + 1u)
                return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        }
        symbol_cursor += object->symbol_count;
    }
    if (symbol_cursor != metadata->symbol_count) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    for (index = 0u; index < metadata->symbol_count; index += 1u) {
        const CmHirExecutableLinkSymbol *symbol = &metadata->symbols[index];
        if (symbol->owner_value == 0u
            || (size_t)symbol->owner_value > metadata->value_count
            || metadata->values[symbol->owner_value - 1u].kind
                != CM_HIR_EXEC_VALUE_NATIVE_OBJECT
            || metadata->values[symbol->owner_value - 1u].execution_local
                != index + 1u || symbol->object_local == 0u
            || (size_t)symbol->object_local > metadata->object_count
            || !cm_exec_portable_identifier(symbol->external_symbol)
            || (index != 0u && (metadata->symbols[index - 1u].object_local
                    > symbol->object_local
                || (metadata->symbols[index - 1u].object_local
                        == symbol->object_local
                    && (cm_exec_string_compare(
                            metadata->symbols[index - 1u].external_symbol,
                            symbol->external_symbol) > 0
                        || (cm_exec_string_equal(
                                metadata->symbols[index - 1u].external_symbol,
                                symbol->external_symbol)
                            && metadata->symbols[index - 1u].owner_value
                                >= symbol->owner_value)))))) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    if (metadata->symbol_count > 1u) {
        CmHirExecutableString *names = cm_alloc_zeroed(metadata->symbol_count,
            sizeof(*names));
        int unique = 1;
        for (index = 0u; index < metadata->symbol_count; index += 1u)
            names[index] = metadata->symbols[index].external_symbol;
        qsort(names, metadata->symbol_count, sizeof(*names),
            cm_exec_qsort_string_compare);
        for (index = 1u; index < metadata->symbol_count; index += 1u)
            if (cm_exec_string_equal(names[index - 1u], names[index]))
                unique = 0;
        cm_free(names);
        if (!unique) return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    if (!cm_exec_validate_namespace(metadata))
        return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    return CM_HIR_EXEC_METADATA_OK;
}

static CmHirExecutableMetadataStatus cm_exec_build_sections(
    const CmHirExecutableMetadata *metadata,
    CmByteBuf sections[CM_EXEC_SECTION_COUNT])
{
    CmHirMetadataWriter writer;
    size_t index;
    size_t parameter;
    uint32_t recipe_count = 0u;
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_byte_buf_init(&sections[index]);

    cm_exec_writer(&writer, &sections[0]);
    cm_exec_write_string(&writer, metadata->crate_name);
    cm_hir_metadata_write_u32(&writer, metadata->edition);
    cm_hir_metadata_write_u32(&writer, UINT32_C(1));
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[2]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->module_count);
    for (index = 0u; index < metadata->module_count; index += 1u) {
        cm_hir_metadata_write_u32(&writer,
            metadata->modules[index].parent_module);
        cm_exec_write_string(&writer, metadata->modules[index].name);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[3]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->trait_count);
    for (index = 0u; index < metadata->trait_count; index += 1u) {
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(2));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer,
            metadata->traits[index].owner_module);
        cm_exec_write_string(&writer, metadata->traits[index].name);
        cm_hir_metadata_write_u32(&writer,
            metadata->traits[index].source_ordinal);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[4]);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[5]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->body_count);
    for (index = 0u; index < metadata->value_count; index += 1u) {
        if (metadata->values[index].kind == CM_HIR_EXEC_VALUE_RECIPE) {
            cm_hir_metadata_write_u32(&writer, (uint32_t)(index + 1u));
            cm_hir_metadata_write_u32(&writer, UINT32_C(0));
            cm_exec_write_string(&writer, metadata->values[index].generic_name);
            recipe_count += 1u;
        }
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[6]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->type_count);
    for (index = 0u; index < metadata->type_count; index += 1u) {
        cm_hir_metadata_write_u8(&writer, metadata->types[index].kind);
        cm_hir_metadata_write_u8(&writer, metadata->types[index].primitive);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer,
            metadata->types[index].owner_value);
        cm_hir_metadata_write_u32(&writer,
            metadata->types[index].generic_index);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[7]);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[8]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->value_count);
    for (index = 0u; index < metadata->value_count; index += 1u) {
        const CmHirExecutableValue *value = &metadata->values[index];
        cm_hir_metadata_write_u32(&writer, value->owner_module);
        cm_exec_write_string(&writer, value->name);
        cm_hir_metadata_write_u32(&writer, value->source_ordinal);
        cm_hir_metadata_write_u8(&writer, value->kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_exec_write_string(&writer, value->generic_name);
        cm_hir_metadata_write_u32(&writer, value->parameter_count);
        for (parameter = 0u; parameter < value->parameter_count; parameter += 1u)
            cm_hir_metadata_write_u32(&writer,
                value->parameter_types[parameter]);
        cm_hir_metadata_write_u32(&writer, value->return_type);
        cm_hir_metadata_write_u32(&writer, value->predicate_start);
        cm_hir_metadata_write_u32(&writer, value->predicate_count);
        cm_hir_metadata_write_u8(&writer, value->kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, value->execution_local);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[9]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->predicate_count);
    for (index = 0u; index < metadata->predicate_count; index += 1u) {
        cm_hir_metadata_write_u32(&writer,
            metadata->predicates[index].owner_value);
        cm_hir_metadata_write_u32(&writer,
            metadata->predicates[index].ordinal);
        cm_hir_metadata_write_u32(&writer,
            metadata->predicates[index].subject_type);
        cm_hir_metadata_write_u32(&writer,
            metadata->predicates[index].trait_local);
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[10]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->impl_count);
    for (index = 0u; index < metadata->impl_count; index += 1u) {
        const CmHirExecutableImpl *impl = &metadata->impls[index];
        cm_hir_metadata_write_u32(&writer, impl->owner_module);
        cm_hir_metadata_write_u32(&writer, impl->source_ordinal);
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, impl->self_type);
        cm_hir_metadata_write_u32(&writer, impl->trait_local);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[11]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->namespace_count);
    for (index = 0u; index < metadata->namespace_count; index += 1u) {
        const CmHirExecutableNamespaceEntry *entry
            = &metadata->namespace_entries[index];
        cm_hir_metadata_write_u32(&writer, entry->owner_module);
        cm_hir_metadata_write_u8(&writer, entry->namespace_kind);
        cm_hir_metadata_write_u8(&writer, entry->target_kind);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_exec_write_string(&writer, entry->name);
        cm_hir_metadata_write_u32(&writer, entry->target_local);
        cm_hir_metadata_write_u32(&writer, entry->export_ordinal);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[12]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->body_count);
    for (index = 0u; index < metadata->body_count; index += 1u) {
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, metadata->bodies[index].owner_value);
        cm_hir_metadata_write_u32(&writer,
            metadata->bodies[index].parameter_index);
        cm_hir_metadata_write_u32(&writer,
            metadata->bodies[index].parameter_type);
        cm_hir_metadata_write_u32(&writer,
            metadata->bodies[index].return_type);
    }
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    cm_exec_writer(&writer, &sections[13]);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->object_count);
    for (index = 0u; index < metadata->object_count; index += 1u) {
        const CmHirExecutableLinkObject *object = &metadata->objects[index];
        cm_exec_write_string(&writer, object->archive_member_name);
        cm_hir_metadata_write_u64(&writer, object->byte_length);
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_bytes(&writer, object->object_digest.bytes,
            CM_HIR_ARTIFACT_IDENTITY_SIZE);
        cm_hir_metadata_write_u32(&writer, object->symbol_start);
        cm_hir_metadata_write_u32(&writer, object->symbol_count);
    }
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->symbol_count);
    for (index = 0u; index < metadata->symbol_count; index += 1u) {
        const CmHirExecutableLinkSymbol *symbol = &metadata->symbols[index];
        cm_hir_metadata_write_u32(&writer, symbol->owner_value);
        cm_hir_metadata_write_u32(&writer, symbol->object_local);
        cm_exec_write_string(&writer, symbol->external_symbol);
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
    }
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK)
        return cm_exec_writer_status(&writer);

    if (recipe_count != metadata->body_count)
        return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u) {
        if (index == 1u) continue;
        if (sections[index].len > (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE)
            return CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED;
    }
    return cm_exec_writer_status(&writer);
}

static CmHirExecutableMetadataStatus cm_exec_build_body_family(
    const CmHirExecutableMetadata *metadata, const CmByteBuf *body,
    CmByteBuf *family)
{
    CmHirMetadataWriter writer;
    size_t index;
    cm_exec_writer(&writer, family);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->value_count);
    for (index = 0u; index < metadata->value_count; index += 1u) {
        cm_hir_metadata_write_u32(&writer, (uint32_t)(index + 1u));
        cm_hir_metadata_write_u8(&writer, metadata->values[index].kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer,
            metadata->values[index].execution_local);
    }
    cm_hir_metadata_write_bytes(&writer, body->data, body->len);
    return cm_exec_writer_status(&writer);
}

static uint32_t cm_exec_empty_crc(void)
{
    static const unsigned char zero_count[4] = { 0u, 0u, 0u, 0u };
    return cm_hir_metadata_crc32(zero_count, sizeof(zero_count));
}

static CmHirExecutableMetadataStatus cm_exec_build_manifest(
    const CmHirExecutableMetadata *metadata,
    CmByteBuf sections[CM_EXEC_SECTION_COUNT],
    const CmHirArtifactDigest *link_digest,
    const CmHirArtifactDigest *artifact_identity)
{
    CmHirMetadataWriter writer;
    CmByteBuf module_family;
    CmByteBuf body_family;
    CmExecFamily families[CM_EXEC_FAMILY_COUNT];
    size_t index;
    static const uint8_t complete[CM_EXEC_FAMILY_COUNT] = {
        1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u, 1u
    };
    cm_exec_writer(&writer, &module_family);
    cm_hir_metadata_write_bytes(&writer, sections[2].data, sections[2].len);
    cm_hir_metadata_write_bytes(&writer, sections[11].data, sections[11].len);
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&module_family);
        return cm_exec_writer_status(&writer);
    }
    if (cm_exec_build_body_family(metadata, &sections[12], &body_family)
            != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&module_family);
        return CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED;
    }
    for (index = 0u; index < CM_EXEC_FAMILY_COUNT; index += 1u) {
        families[index].state = complete[index] ? UINT8_C(2) : UINT8_C(0);
        families[index].count = UINT32_C(0);
        families[index].crc = cm_exec_empty_crc();
    }
    families[0].count = (uint32_t)(metadata->module_count
        + metadata->namespace_count);
    families[0].crc = cm_hir_metadata_crc32(module_family.data,
        module_family.len);
    families[2].count = (uint32_t)metadata->value_count;
    families[2].crc = cm_hir_metadata_crc32(sections[8].data,
        sections[8].len);
    families[3].count = (uint32_t)metadata->trait_count;
    families[3].crc = cm_hir_metadata_crc32(sections[3].data,
        sections[3].len);
    families[8].count = (uint32_t)metadata->impl_count;
    families[8].crc = cm_hir_metadata_crc32(sections[10].data,
        sections[10].len);
    families[12].count = (uint32_t)(metadata->value_count
        + metadata->body_count);
    families[12].crc = cm_hir_metadata_crc32(body_family.data,
        body_family.len);
    families[13].count = (uint32_t)(metadata->object_count
        + metadata->symbol_count);
    families[13].crc = cm_hir_metadata_crc32(sections[13].data,
        sections[13].len);

    cm_exec_writer(&writer, &sections[1]);
    cm_hir_metadata_write_u8(&writer, UINT8_C(2));
    cm_hir_metadata_write_u8(&writer, UINT8_C(3));
    cm_hir_metadata_write_u8(&writer, UINT8_C(0));
    cm_hir_metadata_write_u8(&writer, UINT8_C(0));
    cm_exec_write_string(&writer, metadata->crate_name);
    cm_hir_metadata_write_u32(&writer,
        (uint32_t)metadata->crate_disambiguator.length);
    cm_hir_metadata_write_bytes(&writer, metadata->crate_disambiguator.data,
        metadata->crate_disambiguator.length);
    cm_hir_metadata_write_u32(&writer, metadata->edition);
    cm_exec_write_string(&writer, metadata->target_descriptor);
    cm_exec_write_string(&writer, metadata->panic_strategy);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->cfg_count);
    for (index = 0u; index < metadata->cfg_count; index += 1u)
        cm_exec_write_string(&writer, metadata->cfgs[index]);
    cm_hir_metadata_write_u8(&writer, UINT8_C(1));
    cm_hir_metadata_write_u8(&writer, UINT8_C(0));
    cm_hir_metadata_write_u16(&writer, UINT16_C(0));
    cm_hir_metadata_write_bytes(&writer, metadata->source_digest.bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    cm_hir_metadata_write_bytes(&writer, link_digest->bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    cm_hir_metadata_write_bytes(&writer, artifact_identity->bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)CM_EXEC_FAMILY_COUNT);
    for (index = 0u; index < CM_EXEC_FAMILY_COUNT; index += 1u) {
        cm_hir_metadata_write_u8(&writer, (uint8_t)(index + 1u));
        cm_hir_metadata_write_u8(&writer, families[index].state);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, families[index].count);
        cm_hir_metadata_write_u32(&writer, families[index].crc);
    }
    cm_byte_buf_destroy(&module_family);
    cm_byte_buf_destroy(&body_family);
    return cm_exec_writer_status(&writer);
}

CmHirExecutableMetadataStatus cm_hir_executable_metadata_compute_identity(
    const CmHirExecutableMetadata *metadata,
    CmHirArtifactDigest *out_link_manifest_digest,
    CmHirArtifactDigest *out_artifact_identity)
{
    CmByteBuf sections[CM_EXEC_SECTION_COUNT];
    CmHirArtifactIdentityInput input;
    CmHirArtifactDigest link_digest;
    CmHirArtifactDigest identity;
    CmHirArtifactBytes *cfgs;
    CmHirExecutableMetadataStatus status;
    size_t index;
    if (out_link_manifest_digest == NULL || out_artifact_identity == NULL)
        return CM_HIR_EXEC_METADATA_INVALID_ARGUMENT;
    status = cm_exec_validate(metadata);
    if (status != CM_HIR_EXEC_METADATA_OK) return status;
    if (!cm_exec_source_digest_valid_for_encode(metadata)
        || !cm_exec_object_digests_valid_for_encode(metadata))
        return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    status = cm_exec_build_sections(metadata, sections);
    if (status != CM_HIR_EXEC_METADATA_OK) goto done;
    cm_exec_sha256(sections[13].data, sections[13].len, &link_digest);
    cfgs = metadata->cfg_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->cfg_count, sizeof(*cfgs));
    for (index = 0u; index < metadata->cfg_count; index += 1u) {
        cfgs[index].data = metadata->cfgs[index].data;
        cfgs[index].length = metadata->cfgs[index].length;
    }
    memset(&input, 0, sizeof(input));
    input.schema_major = CM_HIR_EXEC_METADATA_MAJOR;
    input.schema_minor = CM_HIR_EXEC_METADATA_MINOR;
    input.profile = CM_HIR_EXEC_METADATA_PROFILE;
    input.crate_name.data = metadata->crate_name.data;
    input.crate_name.length = metadata->crate_name.length;
    input.crate_disambiguator.data = metadata->crate_disambiguator.data;
    input.crate_disambiguator.length = metadata->crate_disambiguator.length;
    input.edition = metadata->edition;
    input.target_descriptor.data = metadata->target_descriptor.data;
    input.target_descriptor.length = metadata->target_descriptor.length;
    input.panic_strategy.data = metadata->panic_strategy.data;
    input.panic_strategy.length = metadata->panic_strategy.length;
    input.cfgs = cfgs;
    input.cfg_count = metadata->cfg_count;
    input.source_closure = metadata->source_digest;
    input.link_manifest = link_digest;
    if (cm_hir_artifact_identity_compute(&input, &identity)
            != CM_HIR_ARTIFACT_IDENTITY_OK) {
        cm_free(cfgs);
        status = CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        goto done;
    }
    cm_free(cfgs);
    *out_link_manifest_digest = link_digest;
    *out_artifact_identity = identity;
    status = CM_HIR_EXEC_METADATA_OK;
done:
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_byte_buf_destroy(&sections[index]);
    return status;
}

CmHirExecutableMetadataStatus cm_hir_executable_metadata_encode(
    const CmHirExecutableMetadata *metadata, CmByteBuf *output)
{
    CmByteBuf sections[CM_EXEC_SECTION_COUNT];
    CmByteBuf payload;
    CmByteBuf encoded;
    CmHirMetadataWriter writer;
    CmHirArtifactDigest link_digest;
    CmHirArtifactDigest artifact_identity;
    CmHirArtifactIdentityInput identity_input;
    CmHirArtifactBytes *cfgs;
    CmHirExecutableMetadataStatus status;
    CmHirMetadataStatus wire_status;
    size_t index;
    if (output == NULL) return CM_HIR_EXEC_METADATA_INVALID_ARGUMENT;
    status = cm_exec_validate(metadata);
    if (status != CM_HIR_EXEC_METADATA_OK) return status;
    if (!cm_exec_source_digest_valid_for_encode(metadata)
        || !cm_exec_object_digests_valid_for_encode(metadata))
        return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
    status = cm_exec_build_sections(metadata, sections);
    if (status != CM_HIR_EXEC_METADATA_OK) goto destroy_sections;
    cm_exec_sha256(sections[13].data, sections[13].len, &link_digest);
    cfgs = metadata->cfg_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->cfg_count, sizeof(*cfgs));
    for (index = 0u; index < metadata->cfg_count; index += 1u) {
        cfgs[index].data = metadata->cfgs[index].data;
        cfgs[index].length = metadata->cfgs[index].length;
    }
    memset(&identity_input, 0, sizeof(identity_input));
    identity_input.schema_major = CM_HIR_EXEC_METADATA_MAJOR;
    identity_input.schema_minor = CM_HIR_EXEC_METADATA_MINOR;
    identity_input.profile = CM_HIR_EXEC_METADATA_PROFILE;
    identity_input.crate_name.data = metadata->crate_name.data;
    identity_input.crate_name.length = metadata->crate_name.length;
    identity_input.crate_disambiguator.data
        = metadata->crate_disambiguator.data;
    identity_input.crate_disambiguator.length
        = metadata->crate_disambiguator.length;
    identity_input.edition = metadata->edition;
    identity_input.target_descriptor.data = metadata->target_descriptor.data;
    identity_input.target_descriptor.length
        = metadata->target_descriptor.length;
    identity_input.panic_strategy.data = metadata->panic_strategy.data;
    identity_input.panic_strategy.length = metadata->panic_strategy.length;
    identity_input.cfgs = cfgs;
    identity_input.cfg_count = metadata->cfg_count;
    identity_input.source_closure = metadata->source_digest;
    identity_input.link_manifest = link_digest;
    if (cm_hir_artifact_identity_compute(&identity_input, &artifact_identity)
            != CM_HIR_ARTIFACT_IDENTITY_OK) {
        cm_free(cfgs);
        status = CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        goto destroy_sections;
    }
    cm_free(cfgs);
    status = cm_exec_build_manifest(metadata, sections, &link_digest,
        &artifact_identity);
    if (status != CM_HIR_EXEC_METADATA_OK) goto destroy_sections;
    cm_exec_writer(&writer, &payload);
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_hir_metadata_write_section(&writer, cm_exec_tags[index],
            sections[index].data, sections[index].len);
    status = cm_exec_writer_status(&writer);
    if (status != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&payload);
        goto destroy_sections;
    }
    cm_byte_buf_init(&encoded);
    wire_status = cm_hir_metadata_encode_envelope_version(&encoded,
        CM_HIR_EXEC_METADATA_MAJOR, CM_HIR_EXEC_METADATA_MINOR, UINT32_C(0),
        payload.data, payload.len);
    cm_byte_buf_destroy(&payload);
    if (wire_status != CM_HIR_METADATA_OK) {
        cm_byte_buf_destroy(&encoded);
        status = wire_status == CM_HIR_METADATA_LIMIT_EXCEEDED
            ? CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED
            : CM_HIR_EXEC_METADATA_INVALID_FORMAT;
        goto destroy_sections;
    }
    cm_byte_buf_destroy(output);
    *output = encoded;
    status = CM_HIR_EXEC_METADATA_OK;
destroy_sections:
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_byte_buf_destroy(&sections[index]);
    return status;
}

static CmHirExecutableMetadataStatus cm_exec_read_status(
    const CmHirMetadataReader *reader)
{
    CmHirMetadataStatus status = cm_hir_metadata_reader_status(reader);
    return status == CM_HIR_METADATA_LIMIT_EXCEEDED
        ? CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED
        : CM_HIR_EXEC_METADATA_INVALID_FORMAT;
}

static int cm_exec_read_string(CmHirMetadataReader *reader,
    CmHirExecutableString *output, size_t minimum, size_t maximum)
{
    uint32_t length;
    const unsigned char *data;
    CmHirExecutableString candidate;
    size_t index;
    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK)
        return 0;
    if ((size_t)length < minimum || (size_t)length > maximum) return 0;
    if (cm_hir_metadata_read_bytes(reader, (size_t)length, &data)
            != CM_HIR_METADATA_OK) return 0;
    for (index = 0u; index < (size_t)length; index += 1u)
        if (data[index] == 0u) return 0;
    candidate.length = (size_t)length;
    candidate.data = length == 0u ? NULL : cm_alloc((size_t)length);
    if (length != 0u) memcpy(candidate.data, data, (size_t)length);
    *output = candidate;
    return 1;
}


static int cm_exec_read_bytes(CmHirMetadataReader *reader,
    CmHirExecutableString *output, size_t minimum, size_t maximum)
{
    uint32_t length;
    const unsigned char *data;
    CmHirExecutableString candidate;

    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || (size_t)length < minimum || (size_t)length > maximum
        || cm_hir_metadata_read_bytes(reader, (size_t)length, &data)
            != CM_HIR_METADATA_OK) return 0;
    candidate.length = (size_t)length;
    candidate.data = length == 0u ? NULL : cm_alloc((size_t)length);
    if (length != 0u) memcpy(candidate.data, data, (size_t)length);
    *output = candidate;
    return 1;
}

static int cm_exec_read_count(CmHirMetadataReader *reader, size_t maximum,
    size_t *output)
{
    uint32_t value;
    if (cm_hir_metadata_read_u32(reader, &value) != CM_HIR_METADATA_OK
        || (size_t)value > maximum) return 0;
    *output = (size_t)value;
    return 1;
}

static int cm_exec_read_zero_u16(CmHirMetadataReader *reader)
{
    uint16_t value;
    return cm_hir_metadata_read_u16(reader, &value) == CM_HIR_METADATA_OK
        && value == UINT16_C(0);
}

static int cm_exec_read_zero_u8(CmHirMetadataReader *reader)
{
    uint8_t value;
    return cm_hir_metadata_read_u8(reader, &value) == CM_HIR_METADATA_OK
        && value == UINT8_C(0);
}

static int cm_exec_finish(CmHirMetadataReader *reader)
{
    return cm_hir_metadata_reader_finish(reader) == CM_HIR_METADATA_OK;
}

static int cm_exec_parse_manifest(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata, CmExecFamily families[CM_EXEC_FAMILY_COUNT])
{
    CmHirMetadataReader reader;
    const unsigned char *bytes;
    uint8_t byte;
    uint16_t reserved16;
    uint32_t length;
    uint32_t dependency_count;
    uint32_t family_count;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (cm_hir_metadata_read_u8(&reader, &byte) != CM_HIR_METADATA_OK
        || byte != UINT8_C(2)
        || cm_hir_metadata_read_u8(&reader, &byte) != CM_HIR_METADATA_OK
        || byte != UINT8_C(3)
        || cm_hir_metadata_read_u8(&reader, &byte) != CM_HIR_METADATA_OK
        || byte != UINT8_C(0) || !cm_exec_read_zero_u8(&reader)
        || !cm_exec_read_string(&reader, &metadata->crate_name, 1u,
            CM_HIR_ARTIFACT_MAX_NAME_SIZE)
        || cm_hir_metadata_read_u32(&reader, &length) != CM_HIR_METADATA_OK
        || (size_t)length > CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE
        || length == 0u
        || cm_hir_metadata_read_bytes(&reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK) return 0;
    metadata->crate_disambiguator.data = cm_alloc((size_t)length);
    metadata->crate_disambiguator.length = (size_t)length;
    memcpy(metadata->crate_disambiguator.data, bytes, (size_t)length);
    if (cm_hir_metadata_read_u32(&reader, &metadata->edition)
            != CM_HIR_METADATA_OK || metadata->edition == 0u
        || !cm_exec_read_bytes(&reader, &metadata->target_descriptor, 1u,
            CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE)
        || !cm_exec_read_string(&reader, &metadata->panic_strategy, 1u,
            CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE)
        || !cm_exec_read_count(&reader, CM_HIR_ARTIFACT_MAX_CFG_COUNT,
            &metadata->cfg_count)) return 0;
    metadata->cfgs = metadata->cfg_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->cfg_count, sizeof(*metadata->cfgs));
    for (index = 0u; index < metadata->cfg_count; index += 1u) {
        if (!cm_exec_read_string(&reader, &metadata->cfgs[index], 1u,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE)) return 0;
    }
    if (cm_hir_metadata_read_u8(&reader, &byte) != CM_HIR_METADATA_OK
        || byte != UINT8_C(1) || !cm_exec_read_zero_u8(&reader)
        || cm_hir_metadata_read_u16(&reader, &reserved16)
            != CM_HIR_METADATA_OK || reserved16 != UINT16_C(0)
        || cm_hir_metadata_read_bytes(&reader,
            CM_HIR_ARTIFACT_IDENTITY_SIZE, &bytes) != CM_HIR_METADATA_OK)
        return 0;
    memcpy(metadata->source_digest.bytes, bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    if (cm_hir_metadata_read_bytes(&reader,
            CM_HIR_ARTIFACT_IDENTITY_SIZE, &bytes) != CM_HIR_METADATA_OK)
        return 0;
    memcpy(metadata->link_manifest_digest.bytes, bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    if (cm_hir_metadata_read_u32(&reader, &dependency_count)
            != CM_HIR_METADATA_OK || dependency_count != UINT32_C(0)
        || cm_hir_metadata_read_bytes(&reader,
            CM_HIR_ARTIFACT_IDENTITY_SIZE, &bytes) != CM_HIR_METADATA_OK)
        return 0;
    memcpy(metadata->artifact_identity.bytes, bytes,
        CM_HIR_ARTIFACT_IDENTITY_SIZE);
    if (cm_hir_metadata_read_u32(&reader, &family_count)
            != CM_HIR_METADATA_OK
        || family_count != (uint32_t)CM_EXEC_FAMILY_COUNT) return 0;
    for (index = 0u; index < CM_EXEC_FAMILY_COUNT; index += 1u) {
        uint8_t tag;
        if (cm_hir_metadata_read_u8(&reader, &tag) != CM_HIR_METADATA_OK
            || tag != (uint8_t)(index + 1u)
            || cm_hir_metadata_read_u8(&reader, &families[index].state)
                != CM_HIR_METADATA_OK
            || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader, &families[index].count)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &families[index].crc)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_crat(const CmHirMetadataSection *section,
    const CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    CmHirExecutableString name;
    uint32_t edition;
    uint32_t root;
    int valid;
    memset(&name, 0, sizeof(name));
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    valid = cm_exec_read_string(&reader, &name, 1u,
            CM_HIR_ARTIFACT_MAX_NAME_SIZE)
        && cm_hir_metadata_read_u32(&reader, &edition) == CM_HIR_METADATA_OK
        && cm_hir_metadata_read_u32(&reader, &root) == CM_HIR_METADATA_OK
        && cm_exec_finish(&reader) && root == UINT32_C(1)
        && edition == metadata->edition
        && cm_exec_string_equal(name, metadata->crate_name);
    cm_exec_free_string(&name);
    return valid;
}

static int cm_exec_parse_modules(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->module_count) || metadata->module_count == 0u) return 0;
    metadata->modules = cm_alloc_zeroed(metadata->module_count,
        sizeof(*metadata->modules));
    for (index = 0u; index < metadata->module_count; index += 1u) {
        if (cm_hir_metadata_read_u32(&reader,
                &metadata->modules[index].parent_module)
                != CM_HIR_METADATA_OK
            || !cm_exec_read_string(&reader, &metadata->modules[index].name,
                1u, 255u)) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_traits(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    uint8_t kind;
    uint8_t state;
    uint32_t structural_zero;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->trait_count)) return 0;
    metadata->traits = metadata->trait_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->trait_count, sizeof(*metadata->traits));
    for (index = 0u; index < metadata->trait_count; index += 1u) {
        if (cm_hir_metadata_read_u8(&reader, &kind) != CM_HIR_METADATA_OK
            || kind != UINT8_C(1)
            || cm_hir_metadata_read_u8(&reader, &state) != CM_HIR_METADATA_OK
            || state != UINT8_C(2) || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader,
                &metadata->traits[index].owner_module) != CM_HIR_METADATA_OK
            || !cm_exec_read_string(&reader, &metadata->traits[index].name,
                1u, 255u)
            || cm_hir_metadata_read_u32(&reader,
                &metadata->traits[index].source_ordinal) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &structural_zero)
                != CM_HIR_METADATA_OK || structural_zero != UINT32_C(0))
            return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_zero_section(const CmHirMetadataSection *section)
{
    CmHirMetadataReader reader;
    uint32_t count;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    return cm_hir_metadata_read_u32(&reader, &count) == CM_HIR_METADATA_OK
        && count == UINT32_C(0) && cm_exec_finish(&reader);
}

static int cm_exec_parse_types(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->type_count)) return 0;
    metadata->types = metadata->type_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->type_count, sizeof(*metadata->types));
    for (index = 0u; index < metadata->type_count; index += 1u) {
        if (cm_hir_metadata_read_u8(&reader, &metadata->types[index].kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader,
                &metadata->types[index].primitive) != CM_HIR_METADATA_OK
            || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader,
                &metadata->types[index].owner_value) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &metadata->types[index].generic_index) != CM_HIR_METADATA_OK)
            return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_values(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t parameter;
    uint8_t execution_kind;
    uint8_t reserved8;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->value_count)) return 0;
    metadata->values = metadata->value_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->value_count, sizeof(*metadata->values));
    for (index = 0u; index < metadata->value_count; index += 1u) {
        CmHirExecutableValue *value = &metadata->values[index];
        if (cm_hir_metadata_read_u32(&reader, &value->owner_module)
                != CM_HIR_METADATA_OK
            || !cm_exec_read_string(&reader, &value->name, 1u, 255u)
            || cm_hir_metadata_read_u32(&reader, &value->source_ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &value->kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &reserved8)
                != CM_HIR_METADATA_OK || reserved8 != UINT8_C(0)
            || !cm_exec_read_zero_u16(&reader)
            || !cm_exec_read_string(&reader, &value->generic_name, 0u, 255u)
            || cm_hir_metadata_read_u32(&reader, &value->parameter_count)
                != CM_HIR_METADATA_OK
            || value->parameter_count == 0u
            || value->parameter_count > CM_HIR_EXEC_METADATA_MAX_PARAMETERS)
            return 0;
        value->parameter_types = cm_alloc_zeroed(value->parameter_count,
            sizeof(*value->parameter_types));
        for (parameter = 0u; parameter < value->parameter_count; parameter += 1u)
            if (cm_hir_metadata_read_u32(&reader,
                    &value->parameter_types[parameter]) != CM_HIR_METADATA_OK)
                return 0;
        if (cm_hir_metadata_read_u32(&reader, &value->return_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &value->predicate_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &value->predicate_count)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &execution_kind)
                != CM_HIR_METADATA_OK || execution_kind != value->kind
            || !cm_exec_read_zero_u8(&reader) || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader, &value->execution_local)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_generics(const CmHirMetadataSection *section,
    const CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    CmHirExecutableString name;
    size_t count;
    size_t index;
    size_t next_recipe = 0u;
    size_t recipe_count = 0u;
    uint32_t owner;
    uint32_t generic_index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS, &count))
        return 0;
    for (index = 0u; index < count; index += 1u) {
        while (next_recipe < metadata->value_count
            && metadata->values[next_recipe].kind
                != CM_HIR_EXEC_VALUE_RECIPE) next_recipe += 1u;
        memset(&name, 0, sizeof(name));
        if (next_recipe == metadata->value_count
            || cm_hir_metadata_read_u32(&reader, &owner)
                != CM_HIR_METADATA_OK || owner != next_recipe + 1u
            || cm_hir_metadata_read_u32(&reader, &generic_index)
                != CM_HIR_METADATA_OK || generic_index != UINT32_C(0)
            || !cm_exec_read_string(&reader, &name, 1u, 255u)
            || !cm_exec_string_equal(name,
                metadata->values[next_recipe].generic_name)) {
            cm_exec_free_string(&name);
            return 0;
        }
        cm_exec_free_string(&name);
        next_recipe += 1u;
        recipe_count += 1u;
    }
    while (next_recipe < metadata->value_count
        && metadata->values[next_recipe].kind
            != CM_HIR_EXEC_VALUE_RECIPE) next_recipe += 1u;
    return count == recipe_count
        && next_recipe == metadata->value_count && cm_exec_finish(&reader);
}

static int cm_exec_parse_predicates(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    uint8_t modifier;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->predicate_count)) return 0;
    metadata->predicates = metadata->predicate_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->predicate_count,
            sizeof(*metadata->predicates));
    for (index = 0u; index < metadata->predicate_count; index += 1u) {
        CmHirExecutablePredicate *predicate = &metadata->predicates[index];
        if (cm_hir_metadata_read_u32(&reader, &predicate->owner_value)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->subject_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->trait_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &modifier)
                != CM_HIR_METADATA_OK || modifier != UINT8_C(1)
            || !cm_exec_read_zero_u8(&reader) || !cm_exec_read_zero_u16(&reader))
            return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_impls(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    uint8_t kind;
    uint8_t safety;
    uint8_t polarity;
    uint8_t defaultness;
    uint8_t constness;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->impl_count)) return 0;
    metadata->impls = metadata->impl_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->impl_count, sizeof(*metadata->impls));
    for (index = 0u; index < metadata->impl_count; index += 1u) {
        CmHirExecutableImpl *impl = &metadata->impls[index];
        if (cm_hir_metadata_read_u32(&reader, &impl->owner_module)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &impl->source_ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &kind) != CM_HIR_METADATA_OK
            || kind != UINT8_C(1)
            || cm_hir_metadata_read_u8(&reader, &safety)
                != CM_HIR_METADATA_OK || safety != UINT8_C(0)
            || cm_hir_metadata_read_u8(&reader, &polarity)
                != CM_HIR_METADATA_OK || polarity != UINT8_C(1)
            || cm_hir_metadata_read_u8(&reader, &defaultness)
                != CM_HIR_METADATA_OK || defaultness != UINT8_C(0)
            || cm_hir_metadata_read_u8(&reader, &constness)
                != CM_HIR_METADATA_OK || constness != UINT8_C(0)
            || !cm_exec_read_zero_u8(&reader) || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader, &impl->self_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &impl->trait_local)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_namespace(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->namespace_count)) return 0;
    metadata->namespace_entries = metadata->namespace_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->namespace_count,
            sizeof(*metadata->namespace_entries));
    for (index = 0u; index < metadata->namespace_count; index += 1u) {
        CmHirExecutableNamespaceEntry *entry
            = &metadata->namespace_entries[index];
        if (cm_hir_metadata_read_u32(&reader, &entry->owner_module)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &entry->namespace_kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &entry->target_kind)
                != CM_HIR_METADATA_OK || !cm_exec_read_zero_u16(&reader)
            || !cm_exec_read_string(&reader, &entry->name, 1u, 255u)
            || cm_hir_metadata_read_u32(&reader, &entry->target_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &entry->export_ordinal)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_bodies(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    uint8_t tag;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->body_count)) return 0;
    metadata->bodies = metadata->body_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->body_count, sizeof(*metadata->bodies));
    for (index = 0u; index < metadata->body_count; index += 1u) {
        CmHirExecutableBody *body = &metadata->bodies[index];
        if (cm_hir_metadata_read_u8(&reader, &tag) != CM_HIR_METADATA_OK
            || tag != UINT8_C(1) || !cm_exec_read_zero_u8(&reader)
            || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_u32(&reader, &body->owner_value)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &body->parameter_index)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &body->parameter_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &body->return_type)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_exec_finish(&reader);
}

static int cm_exec_parse_link(const CmHirMetadataSection *section,
    CmHirExecutableMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    const unsigned char *digest;
    uint8_t algorithm;
    uint8_t linkage;
    uint32_t native_count;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_exec_read_count(&reader, 15u, &metadata->object_count)) return 0;
    metadata->objects = metadata->object_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->object_count, sizeof(*metadata->objects));
    for (index = 0u; index < metadata->object_count; index += 1u) {
        CmHirExecutableLinkObject *object = &metadata->objects[index];
        if (!cm_exec_read_string(&reader, &object->archive_member_name, 1u, 15u)
            || cm_hir_metadata_read_u64(&reader, &object->byte_length)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &algorithm)
                != CM_HIR_METADATA_OK || algorithm != UINT8_C(1)
            || !cm_exec_read_zero_u8(&reader) || !cm_exec_read_zero_u16(&reader)
            || cm_hir_metadata_read_bytes(&reader,
                CM_HIR_ARTIFACT_IDENTITY_SIZE, &digest) != CM_HIR_METADATA_OK)
            return 0;
        memcpy(object->object_digest.bytes, digest,
            CM_HIR_ARTIFACT_IDENTITY_SIZE);
        if (cm_hir_metadata_read_u32(&reader, &object->symbol_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &object->symbol_count)
                != CM_HIR_METADATA_OK) return 0;
    }
    if (!cm_exec_read_count(&reader, CM_HIR_EXEC_METADATA_MAX_RECORDS,
            &metadata->symbol_count)) return 0;
    metadata->symbols = metadata->symbol_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->symbol_count, sizeof(*metadata->symbols));
    for (index = 0u; index < metadata->symbol_count; index += 1u) {
        CmHirExecutableLinkSymbol *symbol = &metadata->symbols[index];
        if (cm_hir_metadata_read_u32(&reader, &symbol->owner_value)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &symbol->object_local)
                != CM_HIR_METADATA_OK
            || !cm_exec_read_string(&reader, &symbol->external_symbol, 1u, 255u)
            || cm_hir_metadata_read_u8(&reader, &linkage)
                != CM_HIR_METADATA_OK || linkage != UINT8_C(1)
            || !cm_exec_read_zero_u8(&reader) || !cm_exec_read_zero_u16(&reader))
            return 0;
    }
    return cm_hir_metadata_read_u32(&reader, &native_count)
            == CM_HIR_METADATA_OK && native_count == UINT32_C(0)
        && cm_exec_finish(&reader);
}

static int cm_exec_expectation_valid(
    const CmHirExecutableMetadataExpectation *expectation)
{
    size_t index;
    if (expectation == NULL
        || (expectation->edition != UINT32_C(2015)
            && expectation->edition != UINT32_C(2018)
            && expectation->edition != UINT32_C(2021)
            && expectation->edition != UINT32_C(2024))
        || !cm_exec_string_valid(expectation->crate_name, 1u,
            CM_HIR_ARTIFACT_MAX_NAME_SIZE)
        || !cm_exec_string_valid(expectation->crate_disambiguator, 1u,
            CM_HIR_ARTIFACT_MAX_DISAMBIGUATOR_SIZE)
        || !cm_exec_bytes_valid(expectation->target_descriptor, 1u,
            CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE)
        || !cm_exec_string_equal(expectation->panic_strategy,
            (CmHirExecutableString){ (unsigned char *)"abort", 5u })
        || expectation->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || (expectation->cfg_count != 0u && expectation->cfgs == NULL)) return 0;
    for (index = 0u; index < expectation->cfg_count; index += 1u) {
        if (!cm_exec_string_valid(expectation->cfgs[index], 1u,
                CM_HIR_ARTIFACT_MAX_CFG_SIZE)
            || (index != 0u && cm_exec_string_compare(
                expectation->cfgs[index - 1u], expectation->cfgs[index]) >= 0))
            return 0;
    }
    return 1;
}

static int cm_exec_identity_matches_expectation(
    const CmHirExecutableMetadata *metadata,
    const CmHirExecutableMetadataExpectation *expectation)
{
    size_t index;
    if (!cm_exec_string_equal(metadata->crate_name, expectation->crate_name)
        || !cm_exec_string_equal(metadata->crate_disambiguator,
            expectation->crate_disambiguator)
        || metadata->edition != expectation->edition
        || !cm_exec_string_equal(metadata->target_descriptor,
            expectation->target_descriptor)
        || !cm_exec_string_equal(metadata->panic_strategy,
            expectation->panic_strategy)
        || metadata->cfg_count != expectation->cfg_count
        || !cm_exec_digest_equal(&metadata->source_digest,
            &expectation->source_digest)
        || !cm_exec_digest_equal(&metadata->artifact_identity,
            &expectation->artifact_identity)) return 0;
    for (index = 0u; index < metadata->cfg_count; index += 1u)
        if (!cm_exec_string_equal(metadata->cfgs[index],
                expectation->cfgs[index])) return 0;
    return 1;
}

static int cm_exec_recompute_identity(const CmHirExecutableMetadata *metadata,
    const CmHirMetadataSection *link)
{
    CmHirArtifactIdentityInput input;
    CmHirArtifactDigest link_digest;
    CmHirArtifactDigest identity;
    CmHirArtifactBytes *cfgs;
    size_t index;
    cm_exec_sha256(link->data, link->length, &link_digest);
    if (!cm_exec_digest_equal(&link_digest,
            &metadata->link_manifest_digest)) return 0;
    cfgs = metadata->cfg_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->cfg_count, sizeof(*cfgs));
    for (index = 0u; index < metadata->cfg_count; index += 1u) {
        cfgs[index].data = metadata->cfgs[index].data;
        cfgs[index].length = metadata->cfgs[index].length;
    }
    memset(&input, 0, sizeof(input));
    input.schema_major = CM_HIR_EXEC_METADATA_MAJOR;
    input.schema_minor = CM_HIR_EXEC_METADATA_MINOR;
    input.profile = CM_HIR_EXEC_METADATA_PROFILE;
    input.crate_name.data = metadata->crate_name.data;
    input.crate_name.length = metadata->crate_name.length;
    input.crate_disambiguator.data = metadata->crate_disambiguator.data;
    input.crate_disambiguator.length = metadata->crate_disambiguator.length;
    input.edition = metadata->edition;
    input.target_descriptor.data = metadata->target_descriptor.data;
    input.target_descriptor.length = metadata->target_descriptor.length;
    input.panic_strategy.data = metadata->panic_strategy.data;
    input.panic_strategy.length = metadata->panic_strategy.length;
    input.cfgs = cfgs;
    input.cfg_count = metadata->cfg_count;
    input.source_closure = metadata->source_digest;
    input.link_manifest = link_digest;
    if (cm_hir_artifact_identity_compute(&input, &identity)
            != CM_HIR_ARTIFACT_IDENTITY_OK) {
        cm_free(cfgs);
        return 0;
    }
    cm_free(cfgs);
    return cm_exec_digest_equal(&identity, &metadata->artifact_identity);
}

CmHirExecutableMetadataStatus cm_hir_executable_metadata_validate(
    const CmHirExecutableMetadata *metadata)
{
    CmByteBuf sections[CM_EXEC_SECTION_COUNT];
    CmHirMetadataSection link;
    CmHirExecutableMetadataStatus status;
    size_t index;

    status = cm_exec_validate(metadata);
    if (status != CM_HIR_EXEC_METADATA_OK) return status;
    status = cm_exec_build_sections(metadata, sections);
    if (status == CM_HIR_EXEC_METADATA_OK) {
        link.data = sections[13].data;
        link.length = sections[13].len;
        if (!cm_exec_recompute_identity(metadata, &link))
            status = CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH;
    }
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_byte_buf_destroy(&sections[index]);
    return status;
}

static int cm_exec_validate_families(const CmHirExecutableMetadata *metadata,
    const CmHirMetadataSection sections[CM_EXEC_SECTION_COUNT],
    const CmExecFamily families[CM_EXEC_FAMILY_COUNT])
{
    static const uint8_t complete[CM_EXEC_FAMILY_COUNT] = {
        1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u, 1u
    };
    CmByteBuf module_family;
    CmByteBuf body_family;
    CmHirMetadataWriter writer;
    uint32_t expected_count[CM_EXEC_FAMILY_COUNT];
    uint32_t expected_crc[CM_EXEC_FAMILY_COUNT];
    CmByteBuf body_view;
    size_t index;
    memset(expected_count, 0, sizeof(expected_count));
    for (index = 0u; index < CM_EXEC_FAMILY_COUNT; index += 1u)
        expected_crc[index] = cm_exec_empty_crc();
    cm_exec_writer(&writer, &module_family);
    cm_hir_metadata_write_bytes(&writer, sections[2].data, sections[2].length);
    cm_hir_metadata_write_bytes(&writer, sections[11].data, sections[11].length);
    if (cm_exec_writer_status(&writer) != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&module_family);
        return 0;
    }
    cm_byte_buf_init(&body_view);
    body_view.data = (unsigned char *)sections[12].data;
    body_view.len = sections[12].length;
    body_view.cap = sections[12].length;
    if (cm_exec_build_body_family(metadata, &body_view, &body_family)
            != CM_HIR_EXEC_METADATA_OK) {
        cm_byte_buf_destroy(&module_family);
        return 0;
    }
    expected_count[0] = (uint32_t)(metadata->module_count
        + metadata->namespace_count);
    expected_crc[0] = cm_hir_metadata_crc32(module_family.data,
        module_family.len);
    expected_count[2] = (uint32_t)metadata->value_count;
    expected_crc[2] = cm_hir_metadata_crc32(sections[8].data,
        sections[8].length);
    expected_count[3] = (uint32_t)metadata->trait_count;
    expected_crc[3] = cm_hir_metadata_crc32(sections[3].data,
        sections[3].length);
    expected_count[8] = (uint32_t)metadata->impl_count;
    expected_crc[8] = cm_hir_metadata_crc32(sections[10].data,
        sections[10].length);
    expected_count[12] = (uint32_t)(metadata->value_count
        + metadata->body_count);
    expected_crc[12] = cm_hir_metadata_crc32(body_family.data,
        body_family.len);
    expected_count[13] = (uint32_t)(metadata->object_count
        + metadata->symbol_count);
    expected_crc[13] = cm_hir_metadata_crc32(sections[13].data,
        sections[13].length);
    cm_byte_buf_destroy(&module_family);
    cm_byte_buf_destroy(&body_family);
    for (index = 0u; index < CM_EXEC_FAMILY_COUNT; index += 1u) {
        uint8_t expected_state = complete[index] ? UINT8_C(2) : UINT8_C(0);
        if (families[index].state != expected_state
            || families[index].count != expected_count[index]
            || families[index].crc != expected_crc[index]) return 0;
    }
    return 1;
}

static int cm_exec_sections_canonical(const CmHirExecutableMetadata *metadata,
    const CmHirMetadataSection original[CM_EXEC_SECTION_COUNT])
{
    CmByteBuf rebuilt[CM_EXEC_SECTION_COUNT];
    CmHirExecutableMetadataStatus status;
    size_t index;
    int valid = 1;
    status = cm_exec_build_sections(metadata, rebuilt);
    if (status != CM_HIR_EXEC_METADATA_OK) valid = 0;
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u) {
        if (index == 1u) continue;
        if (rebuilt[index].len != original[index].length
            || (rebuilt[index].len != 0u
                && memcmp(rebuilt[index].data, original[index].data,
                    rebuilt[index].len) != 0)) valid = 0;
    }
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u)
        cm_byte_buf_destroy(&rebuilt[index]);
    return valid;
}

static CmHirExecutableMetadataStatus cm_exec_decode(
    const void *encoded, size_t encoded_length,
    const CmHirExecutableMetadataExpectation *expectation,
    CmHirExecutableMetadata *output)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader section_reader;
    CmHirMetadataSection sections[CM_EXEC_SECTION_COUNT];
    CmExecFamily families[CM_EXEC_FAMILY_COUNT];
    CmHirExecutableMetadata candidate;
    CmHirMetadataStatus wire_status;
    CmHirExecutableMetadataStatus status;
    size_t index;
    if (output == NULL || (encoded_length != 0u && encoded == NULL))
        return CM_HIR_EXEC_METADATA_INVALID_ARGUMENT;
    wire_status = cm_hir_metadata_decode_envelope_version(encoded,
        encoded_length, CM_HIR_EXEC_METADATA_MAJOR, CM_HIR_EXEC_METADATA_MINOR,
        &envelope);
    if (wire_status != CM_HIR_METADATA_OK) {
        if (wire_status == CM_HIR_METADATA_UNSUPPORTED_VERSION)
            return CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR;
        if (wire_status == CM_HIR_METADATA_LIMIT_EXCEEDED)
            return CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED;
        return CM_HIR_EXEC_METADATA_INVALID_FORMAT;
    }
    cm_hir_metadata_reader_init(&section_reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < CM_EXEC_SECTION_COUNT; index += 1u) {
        if (cm_hir_metadata_read_section(&section_reader, &sections[index])
                != CM_HIR_METADATA_OK
            || memcmp(sections[index].tag, cm_exec_tags[index], 4u) != 0)
            return CM_HIR_EXEC_METADATA_INVALID_FORMAT;
    }
    if (cm_hir_metadata_read_section(&section_reader, &sections[0])
            != CM_HIR_METADATA_DONE) return CM_HIR_EXEC_METADATA_INVALID_FORMAT;
    cm_hir_executable_metadata_init(&candidate);
    candidate.owns_storage = 1;
    memset(families, 0, sizeof(families));
    if (!cm_exec_parse_manifest(&sections[1], &candidate, families)
        || !cm_exec_parse_crat(&sections[0], &candidate)
        || !cm_exec_parse_modules(&sections[2], &candidate)
        || !cm_exec_parse_traits(&sections[3], &candidate)
        || !cm_exec_parse_zero_section(&sections[4])
        || !cm_exec_parse_types(&sections[6], &candidate)
        || !cm_exec_parse_zero_section(&sections[7])
        || !cm_exec_parse_values(&sections[8], &candidate)
        || !cm_exec_parse_generics(&sections[5], &candidate)
        || !cm_exec_parse_predicates(&sections[9], &candidate)
        || !cm_exec_parse_impls(&sections[10], &candidate)
        || !cm_exec_parse_namespace(&sections[11], &candidate)
        || !cm_exec_parse_bodies(&sections[12], &candidate)
        || !cm_exec_parse_link(&sections[13], &candidate)) {
        status = cm_exec_read_status(&section_reader);
        (void)status;
        cm_hir_executable_metadata_destroy(&candidate);
        return CM_HIR_EXEC_METADATA_INVALID_FORMAT;
    }
    status = cm_exec_validate(&candidate);
    if (status != CM_HIR_EXEC_METADATA_OK
        || !cm_exec_sections_canonical(&candidate, sections)
        || !cm_exec_validate_families(&candidate, sections, families)
        || !cm_exec_recompute_identity(&candidate, &sections[13])) {
        cm_hir_executable_metadata_destroy(&candidate);
        return status == CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED ? status
            : CM_HIR_EXEC_METADATA_INVALID_FORMAT;
    }
    if (expectation != NULL
        && !cm_exec_identity_matches_expectation(&candidate, expectation)) {
        cm_hir_executable_metadata_destroy(&candidate);
        return CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH;
    }
    cm_hir_executable_metadata_destroy(output);
    *output = candidate;
    return CM_HIR_EXEC_METADATA_OK;
}

CmHirExecutableMetadataStatus cm_hir_executable_metadata_decode(
    const void *encoded, size_t encoded_length,
    const CmHirExecutableMetadataExpectation *expectation,
    CmHirExecutableMetadata *output)
{
    if (!cm_exec_expectation_valid(expectation))
        return CM_HIR_EXEC_METADATA_INVALID_ARGUMENT;
    return cm_exec_decode(encoded, encoded_length, expectation, output);
}

static int cm_exec_configuration_matches(
    const CmHirExecutableMetadata *metadata,
    const CmHirArtifactConfig *configuration)
{
    CmHirExecutableString target;
    CmHirExecutableString panic;
    size_t index;

    if (configuration == NULL || metadata == NULL
        || configuration->cfg_count > CM_HIR_ARTIFACT_MAX_CFG_COUNT
        || (configuration->cfg_count != 0u
            && configuration->cfgs == NULL)) return 0;
    target.data = (unsigned char *)configuration->target_descriptor.data;
    target.length = configuration->target_descriptor.length;
    panic.data = (unsigned char *)configuration->panic_strategy.data;
    panic.length = configuration->panic_strategy.length;
    if (metadata->edition != configuration->edition
        || !cm_exec_bytes_valid(target, 1u,
            CM_HIR_ARTIFACT_MAX_DESCRIPTOR_SIZE)
        || !cm_exec_string_equal(panic,
            (CmHirExecutableString){ (unsigned char *)"abort", 5u })
        || !cm_exec_string_equal(metadata->target_descriptor, target)
        || !cm_exec_string_equal(metadata->panic_strategy, panic)
        || metadata->cfg_count != configuration->cfg_count) return 0;
    for (index = 0u; index < configuration->cfg_count; index += 1u) {
        CmHirExecutableString cfg;

        cfg.data = (unsigned char *)configuration->cfgs[index].data;
        cfg.length = configuration->cfgs[index].length;
        if (!cm_exec_string_valid(cfg, 1u, CM_HIR_ARTIFACT_MAX_CFG_SIZE)
            || !cm_exec_string_equal(metadata->cfgs[index], cfg)) return 0;
    }
    return 1;
}

CmHirExecutableMetadataStatus cm_hir_executable_metadata_decode_configured(
    const void *encoded, size_t encoded_length,
    const CmHirArtifactConfig *configuration,
    CmHirExecutableMetadata *output)
{
    CmHirExecutableMetadata candidate;
    CmHirExecutableMetadataStatus status;

    if (configuration == NULL || output == NULL)
        return CM_HIR_EXEC_METADATA_INVALID_ARGUMENT;
    cm_hir_executable_metadata_init(&candidate);
    status = cm_exec_decode(encoded, encoded_length, NULL, &candidate);
    if (status != CM_HIR_EXEC_METADATA_OK) {
        cm_hir_executable_metadata_destroy(&candidate);
        return status;
    }
    if (!cm_exec_configuration_matches(&candidate, configuration)) {
        cm_hir_executable_metadata_destroy(&candidate);
        return CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH;
    }
    cm_hir_executable_metadata_destroy(output);
    *output = candidate;
    return CM_HIR_EXEC_METADATA_OK;
}

const char *cm_hir_executable_metadata_status_name(
    CmHirExecutableMetadataStatus status)
{
    switch (status) {
    case CM_HIR_EXEC_METADATA_OK: return "ok";
    case CM_HIR_EXEC_METADATA_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_EXEC_METADATA_LIMIT_EXCEEDED: return "limit exceeded";
    case CM_HIR_EXEC_METADATA_UNSUPPORTED_DESCRIPTOR:
        return "unsupported descriptor";
    case CM_HIR_EXEC_METADATA_INVALID_FORMAT: return "invalid format";
    case CM_HIR_EXEC_METADATA_IDENTITY_MISMATCH: return "identity mismatch";
    }
    return "unknown executable metadata status";
}
