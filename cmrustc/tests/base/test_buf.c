#include "cm/buf.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    CmByteBuf bytes;
    CmStrBuf string;
    unsigned char source[128];
    size_t index;

    for (index = 0; index < sizeof(source); index += 1) {
        source[index] = (unsigned char)index;
    }

    cm_byte_buf_init(&bytes);
    cm_byte_buf_append(&bytes, source, sizeof(source));
    assert(bytes.len == sizeof(source));
    assert(memcmp(bytes.data, source, sizeof(source)) == 0);

    cm_byte_buf_append(&bytes, bytes.data + 16, 64);
    assert(bytes.len == sizeof(source) + 64);
    assert(memcmp(bytes.data + sizeof(source), source + 16, 64) == 0);
    cm_byte_buf_resize(&bytes, bytes.len + 19);
    for (index = bytes.len - 19; index < bytes.len; index += 1) {
        assert(bytes.data[index] == 0);
    }
    cm_byte_buf_push(&bytes, 0xa5);
    assert(bytes.data[bytes.len - 1] == 0xa5);
    cm_byte_buf_clear(&bytes);
    assert(bytes.len == 0);
    cm_byte_buf_destroy(&bytes);

    cm_str_buf_init(&string);
    assert(strcmp(cm_str_buf_c_str(&string), "") == 0);
    cm_str_buf_append(&string, "abcdefghijklmno");
    cm_str_buf_push(&string, 'p');
    assert(strcmp(cm_str_buf_c_str(&string), "abcdefghijklmnop") == 0);
    cm_str_buf_append_n(&string, string.data, string.len);
    assert(strcmp(
        cm_str_buf_c_str(&string),
        "abcdefghijklmnopabcdefghijklmnop"
    ) == 0);
    assert(string.data[string.len] == '\0');
    cm_str_buf_clear(&string);
    assert(string.len == 0);
    assert(strcmp(cm_str_buf_c_str(&string), "") == 0);
    cm_str_buf_destroy(&string);
    return 0;
}
