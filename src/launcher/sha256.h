/* sha256.h -- SHA-256 (FIPS 180-4) for the launcher's download check. Written
 * for this toolkit from the standard's description. */
#ifndef XR_SHA256_H
#define XR_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;          /* bytes so far */
    uint8_t  buf[64];
    size_t   n;            /* bytes in buf */
} XrSha256;

void xr_sha256_init(XrSha256 *s);
void xr_sha256_update(XrSha256 *s, const void *data, size_t len);
void xr_sha256_final(XrSha256 *s, uint8_t out[32]);
/* the 64 lowercase hex digits of out into hex (65 bytes) */
void xr_sha256_hex(const uint8_t out[32], char hex[65]);

#endif
