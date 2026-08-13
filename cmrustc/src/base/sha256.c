#include "cm/sha256.h"

#include <string.h>

static uint32_t cm_sha256_rotr(uint32_t value, unsigned int shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static uint32_t cm_sha256_load_be(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24u)
        | ((uint32_t)bytes[1] << 16u)
        | ((uint32_t)bytes[2] << 8u)
        | (uint32_t)bytes[3];
}

static void cm_sha256_store_be(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value >> 24u);
    bytes[1] = (unsigned char)(value >> 16u);
    bytes[2] = (unsigned char)(value >> 8u);
    bytes[3] = (unsigned char)value;
}

static void cm_sha256_compress(CmSha256 *context,
    const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
        UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
        UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
        UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
        UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
        UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
        UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
        UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
        UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
        UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
        UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
        UINT32_C(0xc67178f2)
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int index;

    for (index = 0u; index < 16u; ++index) {
        words[index] = cm_sha256_load_be(block + index * 4u);
    }
    for (index = 16u; index < 64u; ++index) {
        uint32_t left;
        uint32_t right;

        left = words[index - 15u];
        right = words[index - 2u];
        words[index] = words[index - 16u]
            + (cm_sha256_rotr(left, 7u) ^ cm_sha256_rotr(left, 18u)
                ^ (left >> 3u))
            + words[index - 7u]
            + (cm_sha256_rotr(right, 17u) ^ cm_sha256_rotr(right, 19u)
                ^ (right >> 10u));
    }
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0u; index < 64u; ++index) {
        uint32_t choose;
        uint32_t majority;
        uint32_t sum0;
        uint32_t sum1;
        uint32_t temporary1;
        uint32_t temporary2;

        choose = (e & f) ^ ((~e) & g);
        majority = (a & b) ^ (a & c) ^ (b & c);
        sum0 = cm_sha256_rotr(a, 2u) ^ cm_sha256_rotr(a, 13u)
            ^ cm_sha256_rotr(a, 22u);
        sum1 = cm_sha256_rotr(e, 6u) ^ cm_sha256_rotr(e, 11u)
            ^ cm_sha256_rotr(e, 25u);
        temporary1 = h + sum1 + choose + constants[index] + words[index];
        temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void cm_sha256_init(CmSha256 *context)
{
    if (context == NULL) return;
    memset(context, 0, sizeof(*context));
    context->state[0] = UINT32_C(0x6a09e667);
    context->state[1] = UINT32_C(0xbb67ae85);
    context->state[2] = UINT32_C(0x3c6ef372);
    context->state[3] = UINT32_C(0xa54ff53a);
    context->state[4] = UINT32_C(0x510e527f);
    context->state[5] = UINT32_C(0x9b05688c);
    context->state[6] = UINT32_C(0x1f83d9ab);
    context->state[7] = UINT32_C(0x5be0cd19);
}

void cm_sha256_update(CmSha256 *context, const void *bytes, size_t size)
{
    const unsigned char *input;

    if (context == NULL || (size != 0u && bytes == NULL)) return;
    input = (const unsigned char *)bytes;
    context->total_size += (uint64_t)size;
    while (size != 0u) {
        size_t available;
        size_t take;

        available = sizeof(context->block) - context->block_size;
        take = size < available ? size : available;
        memcpy(context->block + context->block_size, input, take);
        context->block_size += take;
        input += take;
        size -= take;
        if (context->block_size == sizeof(context->block)) {
            cm_sha256_compress(context, context->block);
            context->block_size = 0u;
        }
    }
}

void cm_sha256_final(CmSha256 *context,
    unsigned char digest[CM_SHA256_DIGEST_SIZE])
{
    uint64_t bit_size;
    unsigned int index;

    if (context == NULL || digest == NULL) return;
    bit_size = context->total_size * UINT64_C(8);
    context->block[context->block_size++] = 0x80u;
    if (context->block_size > 56u) {
        memset(context->block + context->block_size, 0,
            sizeof(context->block) - context->block_size);
        cm_sha256_compress(context, context->block);
        context->block_size = 0u;
    }
    memset(context->block + context->block_size, 0,
        56u - context->block_size);
    for (index = 0u; index < 8u; ++index) {
        context->block[63u - index] = (unsigned char)(bit_size >> (index * 8u));
    }
    cm_sha256_compress(context, context->block);
    for (index = 0u; index < 8u; ++index) {
        cm_sha256_store_be(digest + index * 4u, context->state[index]);
    }
    memset(context, 0, sizeof(*context));
}
