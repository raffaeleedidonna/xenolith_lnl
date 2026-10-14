#ifndef FORMAT_H
#define FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} format_sha256;

void format_put_u16le(uint8_t *p, uint16_t v);
void format_put_u32le(uint8_t *p, uint32_t v);
void format_put_u64le(uint8_t *p, uint64_t v);
uint16_t format_get_u16le(const uint8_t *p);
uint32_t format_get_u32le(const uint8_t *p);
uint64_t format_get_u64le(const uint8_t *p);

int format_add_u64(uint64_t a, uint64_t b, uint64_t *out);
int format_mul_u64(uint64_t a, uint64_t b, uint64_t *out);
int format_align_u64(uint64_t value, uint64_t alignment, uint64_t *out);

uint64_t format_crc64(uint64_t crc, const void *data, size_t len);

void format_sha256_init(format_sha256 *s);
void format_sha256_update(format_sha256 *s, const void *data, size_t len);
void format_sha256_final(format_sha256 *s, uint8_t out[32]);
void format_sha256_bytes(const void *data, size_t len, uint8_t out[32]);
typedef enum {
    FORMAT_SHA256_UNAVAILABLE,
    FORMAT_SHA256_OPENSSL,
    FORMAT_SHA256_FAILED,
    FORMAT_SHA256_DISABLED
} format_sha256_backend;
/* Full-content SHA-256 only when OpenSSL succeeds; otherwise leaves out intact.
 * Never falls back to an expensive generic full-file scan. Weight loaders then
 * warn and use metadata checks. XENOLITH_WEIGHTS_VERIFY=metadata forces that path. */
format_sha256_backend format_sha256_bytes_fast(const void *data, size_t len,
                                               uint8_t out[32]);
void format_weights_metadata_hash(const void *header, size_t header_len,
                                  size_t file_size, uint8_t out[32]);
struct stat;
void format_weights_file_identity(const struct stat *info, uint8_t out[32]);
void format_weights_verification_warning(format_sha256_backend reason,
                                          const char *artifact);

int format_read(FILE *f, void *data, size_t len);
int format_write(FILE *f, const void *data, size_t len);
int format_seek(FILE *f, uint64_t offset);
int format_file_size(FILE *f, uint64_t *size);

#endif
