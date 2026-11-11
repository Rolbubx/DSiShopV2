// SHA-256 minimal (FIPS 180-4), portable, sans dépendance
#ifndef SHA256_H__
#define SHA256_H__

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

#endif // SHA256_H__
