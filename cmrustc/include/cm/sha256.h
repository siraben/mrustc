#ifndef CMRUSTC_CM_SHA256_H
#define CMRUSTC_CM_SHA256_H

#include "cm/config.h"

#include <stdint.h>

#define CM_SHA256_DIGEST_SIZE ((size_t)32u)

typedef struct CmSha256 {
    uint32_t state[8];
    uint64_t total_size;
    unsigned char block[64];
    size_t block_size;
} CmSha256;

void cm_sha256_init(CmSha256 *context);
void cm_sha256_update(CmSha256 *context, const void *bytes, size_t size);
void cm_sha256_final(CmSha256 *context,
    unsigned char digest[CM_SHA256_DIGEST_SIZE]);

#endif
