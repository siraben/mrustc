#include "cm/rlib.h"

#include <assert.h>
#include <string.h>

#define TEST_NAME_OFFSET ((size_t)8u)
#define TEST_TIMESTAMP_OFFSET ((size_t)24u)
#define TEST_UID_OFFSET ((size_t)36u)
#define TEST_GID_OFFSET ((size_t)42u)
#define TEST_MODE_OFFSET ((size_t)48u)
#define TEST_SIZE_OFFSET ((size_t)56u)
#define TEST_TRAILER_OFFSET ((size_t)66u)
#define TEST_PAYLOAD_OFFSET ((size_t)68u)

static void copy_bytes(CmByteBuf *destination, const CmByteBuf *source)
{
    cm_byte_buf_clear(destination);
    cm_byte_buf_append(destination, source->data, source->len);
}

static void expect_decode_failure(
    const CmByteBuf *archive,
    CmRlibStatus expected_status
)
{
    CmRlibMetadataView view;

    view.data = (const unsigned char *)"sentinel";
    view.length = 8;
    assert(cm_rlib_decode_metadata(
        archive->data, archive->len, &view) == expected_status);
    assert(view.data == NULL);
    assert(view.length == 0);
}

static void set_size_field(CmByteBuf *archive, const char field[10])
{
    memcpy(archive->data + TEST_SIZE_OFFSET, field, 10);
}

static void expect_archive_decode_failure(
    const CmByteBuf *archive,
    CmRlibStatus expected_status
)
{
    CmRlibArchiveView view;

    memset(&view, 0x5a, sizeof(view));
    assert(cm_rlib_decode_members(
        archive->data, archive->len, &view) == expected_status);
    assert(view.member_count == 0);
    assert(view.members[0].data == NULL);
    assert(view.members[0].length == 0);
    assert(view.members[0].name[0] == '\0');
}

static void test_multi_member_archive(void)
{
    static const unsigned char link_data[] = { 1u, 2u, 3u };
    static const unsigned char object_data[] = { 0x7fu, 'E', 'L', 'F' };
    static const unsigned char metadata_data[] = { 'C', 'M', 'H', '3', 0u };
    CmRlibMember members[3];
    CmRlibArchiveView view;
    CmRlibMemberView member;
    CmByteBuf first;
    CmByteBuf second;

    members[0].name = "cmrustc.link";
    members[0].data = link_data;
    members[0].length = sizeof(link_data);
    members[1].name = "cmrustc.object";
    members[1].data = object_data;
    members[1].length = sizeof(object_data);
    members[2].name = "cmrustc.rmeta";
    members[2].data = metadata_data;
    members[2].length = sizeof(metadata_data);

    cm_byte_buf_init(&first);
    cm_byte_buf_init(&second);
    assert(cm_rlib_encode_members(&first, members, 3u) == CM_RLIB_OK);
    assert(cm_rlib_encode_members(&second, members, 3u) == CM_RLIB_OK);
    assert(first.len == second.len);
    assert(memcmp(first.data, second.data, first.len) == 0);

    assert(cm_rlib_decode_members(first.data, first.len, &view)
        == CM_RLIB_OK);
    assert(view.member_count == 3u);
    assert(strcmp(view.members[0].name, "cmrustc.link") == 0);
    assert(view.members[0].length == sizeof(link_data));
    assert(memcmp(view.members[0].data, link_data, sizeof(link_data)) == 0);
    assert(strcmp(view.members[1].name, "cmrustc.object") == 0);
    assert(view.members[1].length == sizeof(object_data));
    assert(memcmp(view.members[1].data, object_data,
        sizeof(object_data)) == 0);
    assert(strcmp(view.members[2].name, "cmrustc.rmeta") == 0);
    assert(view.members[2].length == sizeof(metadata_data));
    assert(memcmp(view.members[2].data, metadata_data,
        sizeof(metadata_data)) == 0);

    assert(cm_rlib_find_member(&view, "cmrustc.object", &member)
        == CM_RLIB_OK);
    assert(member.data == view.members[1].data);
    assert(member.length == sizeof(object_data));
    member.data = (const unsigned char *)"sentinel";
    member.length = 8u;
    assert(cm_rlib_find_member(&view, "missing", &member)
        == CM_RLIB_WRONG_MEMBER);
    assert(member.data == NULL);
    assert(member.length == 0);
    assert(member.name[0] == '\0');

    cm_byte_buf_destroy(&second);
    cm_byte_buf_destroy(&first);
}

static void test_multi_member_encode_rejections(void)
{
    CmRlibMember members[CM_RLIB_MAX_MEMBER_COUNT + 1u];
    CmByteBuf output;
    unsigned char *saved_data;
    size_t saved_length;
    size_t index;

    cm_byte_buf_init(&output);
    cm_byte_buf_append(&output, "unchanged", 9u);
    saved_data = output.data;
    saved_length = output.len;

    members[0].name = "b";
    members[0].data = "b";
    members[0].length = 1u;
    members[1].name = "a";
    members[1].data = "a";
    members[1].length = 1u;
    assert(cm_rlib_encode_members(&output, members, 2u)
        == CM_RLIB_NONCANONICAL_ORDER);
    assert(output.data == saved_data && output.len == saved_length);

    members[0].name = "a";
    members[1].name = "a";
    assert(cm_rlib_encode_members(&output, members, 2u)
        == CM_RLIB_NONCANONICAL_ORDER);
    assert(output.data == saved_data && output.len == saved_length);

    members[0].name = "";
    assert(cm_rlib_encode_members(&output, members, 1u)
        == CM_RLIB_INVALID_MEMBER_NAME);
    members[0].name = "has/slash";
    assert(cm_rlib_encode_members(&output, members, 1u)
        == CM_RLIB_INVALID_MEMBER_NAME);
    members[0].name = "sixteen-chars-xxx";
    assert(cm_rlib_encode_members(&output, members, 1u)
        == CM_RLIB_INVALID_MEMBER_NAME);
    members[0].name = "a";
    members[0].data = NULL;
    members[0].length = 1u;
    assert(cm_rlib_encode_members(&output, members, 1u)
        == CM_RLIB_INVALID_ARGUMENT);
    members[0].data = "x";
    members[0].length = CM_RLIB_MAX_MEMBER_SIZE + 1u;
    assert(cm_rlib_encode_members(&output, members, 1u)
        == CM_RLIB_LIMIT_EXCEEDED);

    for (index = 0; index < CM_RLIB_MAX_MEMBER_COUNT + 1u; index += 1u) {
        members[index].name = "a";
        members[index].data = NULL;
        members[index].length = 0;
    }
    assert(cm_rlib_encode_members(&output, members,
        CM_RLIB_MAX_MEMBER_COUNT + 1u) == CM_RLIB_TOO_MANY_MEMBERS);
    assert(cm_rlib_encode_members(NULL, members, 1u)
        == CM_RLIB_INVALID_ARGUMENT);
    assert(cm_rlib_encode_members(&output, NULL, 1u)
        == CM_RLIB_INVALID_ARGUMENT);
    assert(output.data == saved_data && output.len == saved_length);
    assert(memcmp(output.data, "unchanged", 9u) == 0);

    cm_byte_buf_destroy(&output);
}

static void test_multi_member_decode_rejections(void)
{
    CmRlibMember members[2];
    CmRlibArchiveView view;
    CmByteBuf original;
    CmByteBuf changed;
    size_t second_header;

    members[0].name = "a";
    members[0].data = "x";
    members[0].length = 1u;
    members[1].name = "b";
    members[1].data = "yz";
    members[1].length = 2u;
    cm_byte_buf_init(&original);
    cm_byte_buf_init(&changed);
    assert(cm_rlib_encode_members(&original, members, 2u) == CM_RLIB_OK);
    second_header = 8u + 60u + 2u;

    copy_bytes(&changed, &original);
    memcpy(changed.data + second_header,
        changed.data + TEST_NAME_OFFSET, 16u);
    expect_archive_decode_failure(&changed, CM_RLIB_NONCANONICAL_ORDER);

    copy_bytes(&changed, &original);
    changed.data[TEST_NAME_OFFSET] = (unsigned char)'b';
    changed.data[second_header] = (unsigned char)'a';
    expect_archive_decode_failure(&changed, CM_RLIB_NONCANONICAL_ORDER);

    copy_bytes(&changed, &original);
    changed.data[TEST_NAME_OFFSET] = (unsigned char)'/';
    expect_archive_decode_failure(&changed, CM_RLIB_INVALID_MEMBER_NAME);

    copy_bytes(&changed, &original);
    changed.data[8u + 60u + 1u] = (unsigned char)' ';
    expect_archive_decode_failure(&changed, CM_RLIB_INVALID_PADDING);

    copy_bytes(&changed, &original);
    changed.len = second_header + 20u;
    expect_archive_decode_failure(&changed, CM_RLIB_TRAILING_BYTES);

    memset(&view, 0x5a, sizeof(view));
    assert(cm_rlib_decode_members(NULL, 0u, &view)
        == CM_RLIB_INVALID_ARGUMENT);
    assert(view.member_count == 0u);
    assert(cm_rlib_decode_members(original.data, original.len, NULL)
        == CM_RLIB_INVALID_ARGUMENT);

    cm_byte_buf_destroy(&changed);
    cm_byte_buf_destroy(&original);
}

static void test_exact_encoding_and_decode(void)
{
    static const unsigned char expected_even[] =
        "!<arch>\n"
        "cmrustc.rmeta/  "
        "0           "
        "0     "
        "0     "
        "100644  "
        "2         "
        "`\n"
        "AB";
    static const unsigned char expected_odd[] =
        "!<arch>\n"
        "cmrustc.rmeta/  "
        "0           "
        "0     "
        "0     "
        "100644  "
        "3         "
        "`\n"
        "XYZ\n";
    CmByteBuf even;
    CmByteBuf repeated;
    CmByteBuf odd;
    CmByteBuf empty;
    CmByteBuf alias;
    CmRlibMetadataView view;

    assert(sizeof(expected_even) - 1u == TEST_PAYLOAD_OFFSET + 2u);
    assert(sizeof(expected_odd) - 1u == TEST_PAYLOAD_OFFSET + 4u);

    cm_byte_buf_init(&even);
    assert(cm_rlib_encode_metadata(&even, "AB", 2) == CM_RLIB_OK);
    assert(even.len == sizeof(expected_even) - 1u);
    assert(memcmp(even.data, expected_even, even.len) == 0);
    assert(cm_rlib_decode_metadata(even.data, even.len, &view) == CM_RLIB_OK);
    assert(view.data == even.data + TEST_PAYLOAD_OFFSET);
    assert(view.length == 2);
    assert(memcmp(view.data, "AB", 2) == 0);

    cm_byte_buf_init(&repeated);
    cm_byte_buf_append(&repeated, "old allocation", 14);
    assert(cm_rlib_encode_metadata(&repeated, "AB", 2) == CM_RLIB_OK);
    assert(repeated.len == even.len);
    assert(memcmp(repeated.data, even.data, even.len) == 0);

    cm_byte_buf_init(&odd);
    assert(cm_rlib_encode_metadata(&odd, "XYZ", 3) == CM_RLIB_OK);
    assert(odd.len == sizeof(expected_odd) - 1u);
    assert(memcmp(odd.data, expected_odd, odd.len) == 0);
    assert(cm_rlib_decode_metadata(odd.data, odd.len, &view) == CM_RLIB_OK);
    assert(view.data == odd.data + TEST_PAYLOAD_OFFSET);
    assert(view.length == 3);
    assert(memcmp(view.data, "XYZ", 3) == 0);

    cm_byte_buf_init(&empty);
    assert(cm_rlib_encode_metadata(&empty, NULL, 0) == CM_RLIB_OK);
    assert(empty.len == TEST_PAYLOAD_OFFSET);
    assert(cm_rlib_decode_metadata(empty.data, empty.len, &view) == CM_RLIB_OK);
    assert(view.data == empty.data + TEST_PAYLOAD_OFFSET);
    assert(view.length == 0);

    cm_byte_buf_init(&alias);
    cm_byte_buf_append(&alias, "aliased", 7);
    assert(cm_rlib_encode_metadata(&alias, alias.data, alias.len) == CM_RLIB_OK);
    assert(cm_rlib_decode_metadata(alias.data, alias.len, &view) == CM_RLIB_OK);
    assert(view.length == 7);
    assert(memcmp(view.data, "aliased", 7) == 0);

    cm_byte_buf_destroy(&alias);
    cm_byte_buf_destroy(&empty);
    cm_byte_buf_destroy(&odd);
    cm_byte_buf_destroy(&repeated);
    cm_byte_buf_destroy(&even);
}

static void test_encode_failures_are_transactional(void)
{
    CmByteBuf output;
    unsigned char *saved_data;
    size_t saved_length;
    size_t saved_capacity;

    cm_byte_buf_init(&output);
    cm_byte_buf_append(&output, "unchanged", 9);
    saved_data = output.data;
    saved_length = output.len;
    saved_capacity = output.cap;

    assert(cm_rlib_encode_metadata(&output, NULL, 1)
        == CM_RLIB_INVALID_ARGUMENT);
    assert(output.data == saved_data);
    assert(output.len == saved_length);
    assert(output.cap == saved_capacity);
    assert(memcmp(output.data, "unchanged", 9) == 0);

    assert(cm_rlib_encode_metadata(
        &output, "x", CM_RLIB_MAX_METADATA_SIZE + 1u)
        == CM_RLIB_LIMIT_EXCEEDED);
    assert(output.data == saved_data);
    assert(output.len == saved_length);
    assert(output.cap == saved_capacity);
    assert(memcmp(output.data, "unchanged", 9) == 0);

    assert(cm_rlib_encode_metadata(NULL, "x", 1)
        == CM_RLIB_INVALID_ARGUMENT);
    cm_byte_buf_destroy(&output);
}

static void test_decode_arguments_and_truncation(void)
{
    CmByteBuf archive;
    CmRlibMetadataView view;
    size_t prefix_length;

    cm_byte_buf_init(&archive);
    assert(cm_rlib_encode_metadata(&archive, "XYZ", 3) == CM_RLIB_OK);

    view.data = (const unsigned char *)"sentinel";
    view.length = 8;
    assert(cm_rlib_decode_metadata(NULL, 0, &view)
        == CM_RLIB_INVALID_ARGUMENT);
    assert(view.data == NULL);
    assert(view.length == 0);
    assert(cm_rlib_decode_metadata(archive.data, archive.len, NULL)
        == CM_RLIB_INVALID_ARGUMENT);

    for (prefix_length = 0; prefix_length < archive.len; prefix_length += 1) {
        view.data = (const unsigned char *)"sentinel";
        view.length = 8;
        assert(cm_rlib_decode_metadata(
            archive.data, prefix_length, &view) == CM_RLIB_TRUNCATED);
        assert(view.data == NULL);
        assert(view.length == 0);
    }
    cm_byte_buf_destroy(&archive);
}

static void test_noncanonical_headers(void)
{
    CmByteBuf original;
    CmByteBuf changed;

    cm_byte_buf_init(&original);
    cm_byte_buf_init(&changed);
    assert(cm_rlib_encode_metadata(&original, "XYZ", 3) == CM_RLIB_OK);

    copy_bytes(&changed, &original);
    changed.data[0] = (unsigned char)'?';
    expect_decode_failure(&changed, CM_RLIB_WRONG_MAGIC);

    copy_bytes(&changed, &original);
    changed.data[TEST_NAME_OFFSET] = (unsigned char)'/';
    expect_decode_failure(&changed, CM_RLIB_WRONG_MEMBER);

    copy_bytes(&changed, &original);
    changed.data[TEST_TIMESTAMP_OFFSET] = (unsigned char)'1';
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    changed.data[TEST_UID_OFFSET] = (unsigned char)'1';
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    changed.data[TEST_GID_OFFSET] = (unsigned char)'1';
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    changed.data[TEST_MODE_OFFSET] = (unsigned char)'0';
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    changed.data[TEST_TRAILER_OFFSET] = (unsigned char)'!';
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "          ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "-3        ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "3-        ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "3x        ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "03        ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "3 1       ");
    expect_decode_failure(&changed, CM_RLIB_INVALID_HEADER);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "67108865  ");
    expect_decode_failure(&changed, CM_RLIB_LIMIT_EXCEEDED);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "67108864  ");
    expect_decode_failure(&changed, CM_RLIB_TRUNCATED);

    copy_bytes(&changed, &original);
    set_size_field(&changed, "9999999999");
    {
        CmRlibMetadataView view;
        CmRlibStatus status;

        status = cm_rlib_decode_metadata(changed.data, changed.len, &view);
        assert(status == CM_RLIB_LIMIT_EXCEEDED
            || status == CM_RLIB_INVALID_HEADER);
        assert(view.data == NULL);
        assert(view.length == 0);
    }

    cm_byte_buf_destroy(&changed);
    cm_byte_buf_destroy(&original);
}

static void test_size_padding_and_exact_eof(void)
{
    CmByteBuf even;
    CmByteBuf odd;
    CmByteBuf changed;

    cm_byte_buf_init(&even);
    cm_byte_buf_init(&odd);
    cm_byte_buf_init(&changed);
    assert(cm_rlib_encode_metadata(&even, "AB", 2) == CM_RLIB_OK);
    assert(cm_rlib_encode_metadata(&odd, "XYZ", 3) == CM_RLIB_OK);

    copy_bytes(&changed, &even);
    set_size_field(&changed, "3         ");
    expect_decode_failure(&changed, CM_RLIB_TRUNCATED);

    copy_bytes(&changed, &odd);
    set_size_field(&changed, "2         ");
    expect_decode_failure(&changed, CM_RLIB_TRAILING_BYTES);

    copy_bytes(&changed, &odd);
    changed.data[changed.len - 1u] = (unsigned char)' ';
    expect_decode_failure(&changed, CM_RLIB_INVALID_PADDING);

    copy_bytes(&changed, &even);
    cm_byte_buf_push(&changed, (unsigned char)'\n');
    expect_decode_failure(&changed, CM_RLIB_TRAILING_BYTES);

    copy_bytes(&changed, &even);
    cm_byte_buf_append(&changed, odd.data, odd.len);
    expect_decode_failure(&changed, CM_RLIB_TRAILING_BYTES);

    copy_bytes(&changed, &odd);
    cm_byte_buf_push(&changed, (unsigned char)'x');
    expect_decode_failure(&changed, CM_RLIB_TRAILING_BYTES);

    cm_byte_buf_destroy(&changed);
    cm_byte_buf_destroy(&odd);
    cm_byte_buf_destroy(&even);
}

static void test_status_names(void)
{
    assert(strcmp(cm_rlib_status_name(CM_RLIB_OK), "ok") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_INVALID_ARGUMENT),
        "invalid argument") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_LIMIT_EXCEEDED),
        "limit exceeded") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_WRONG_MAGIC),
        "wrong magic") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_TRUNCATED),
        "truncated") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_INVALID_HEADER),
        "invalid header") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_WRONG_MEMBER),
        "wrong member") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_INVALID_PADDING),
        "invalid padding") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_TRAILING_BYTES),
        "trailing bytes") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_INVALID_MEMBER_NAME),
        "invalid member name") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_TOO_MANY_MEMBERS),
        "too many members") == 0);
    assert(strcmp(cm_rlib_status_name(CM_RLIB_NONCANONICAL_ORDER),
        "noncanonical member order") == 0);
    assert(strcmp(cm_rlib_status_name((CmRlibStatus)999),
        "unknown rlib status") == 0);
}

int main(void)
{
    test_multi_member_archive();
    test_multi_member_encode_rejections();
    test_multi_member_decode_rejections();
    test_exact_encoding_and_decode();
    test_encode_failures_are_transactional();
    test_decode_arguments_and_truncation();
    test_noncanonical_headers();
    test_size_padding_and_exact_eof();
    test_status_names();
    return 0;
}
