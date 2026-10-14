#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "format.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__) && !defined(XE_NO_OPENSSL)
#include <dlfcn.h>
#endif

void format_put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void format_put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void format_put_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint16_t format_get_u16le(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

uint32_t format_get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint64_t format_get_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int format_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) return 0;
    *out = a + b;
    return 1;
}

int format_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

int format_align_u64(uint64_t value, uint64_t alignment, uint64_t *out) {
    if (!alignment || (alignment & (alignment - 1))) return 0;
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) return 0;
    *out = (value + mask) & ~mask;
    return 1;
}

static uint64_t format_crc64_table[256];
static pthread_once_t format_crc64_once = PTHREAD_ONCE_INIT;

static void format_crc64_table_init(void) {
    const uint64_t polynomial = UINT64_C(0x42f0e1eba9ea3693);
    for (uint64_t i = 0; i < 256; i++) {
        uint64_t crc = i << 56;
        for (int bit = 0; bit < 8; bit++)
            crc = crc & UINT64_C(0x8000000000000000)
                  ? (crc << 1) ^ polynomial : crc << 1;
        format_crc64_table[i] = crc;
    }
}

uint64_t format_crc64(uint64_t crc, const void *data, size_t len) {
    pthread_once(&format_crc64_once, format_crc64_table_init);
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++)
        crc = format_crc64_table[(uint8_t)(crc >> 56) ^ p[i]] ^ (crc << 8);
    return crc;
}

static uint32_t format_rotr32(uint32_t x, unsigned n) {
    return x >> n | x << (32 - n);
}

static uint32_t format_get_u32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void format_put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void format_sha256_block(format_sha256 *s, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = format_get_u32be(block + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t a = format_rotr32(w[i - 15], 7) ^
                     format_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t b = format_rotr32(w[i - 2], 17) ^
                     format_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + a + w[i - 7] + b;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = format_rotr32(e, 6) ^ format_rotr32(e, 11) ^
                      format_rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = format_rotr32(a, 2) ^ format_rotr32(a, 13) ^
                      format_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void format_sha256_init(format_sha256 *s) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(s->h, initial, sizeof initial);
    s->bytes = 0;
    s->used = 0;
}

void format_sha256_update(format_sha256 *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->bytes += len;
    if (s->used) {
        size_t take = 64 - s->used;
        if (take > len) take = len;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64) {
            format_sha256_block(s, s->block);
            s->used = 0;
        }
    }
    while (len >= 64) {
        format_sha256_block(s, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(s->block, p, len);
        s->used = len;
    }
}

void format_sha256_final(format_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->bytes << 3;
    s->block[s->used++] = 0x80;
    if (s->used > 56) {
        memset(s->block + s->used, 0, 64 - s->used);
        format_sha256_block(s, s->block);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56 - s->used);
    for (int i = 0; i < 8; i++) s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    format_sha256_block(s, s->block);
    for (int i = 0; i < 8; i++) format_put_u32be(out + 4 * i, s->h[i]);
    memset(s, 0, sizeof *s);
}

void format_sha256_bytes(const void *data, size_t len, uint8_t out[32]) {
    format_sha256 s;
    format_sha256_init(&s);
    format_sha256_update(&s, data, len);
    format_sha256_final(&s, out);
}

#if defined(__linux__) && !defined(XE_NO_OPENSSL)
/* Opaque OpenSSL 3 API: loading it is optional and needs no development headers.
 * Keep our successful dlopen reference alive while the function is callable. */
typedef struct ossl_lib_ctx_st format_ossl_lib_ctx;
typedef int (*format_ossl_digest_fn)(format_ossl_lib_ctx *, const char *,
    const char *, const void *, size_t, unsigned char *, size_t *);
static pthread_once_t format_openssl_once = PTHREAD_ONCE_INIT;
static void *format_openssl_library;
static format_ossl_digest_fn format_openssl_digest;

static void format_openssl_init(void) {
    void *library = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!library) return;
    void *symbol = dlsym(library, "EVP_Q_digest");
    if (!symbol || sizeof symbol != sizeof format_openssl_digest) {
        dlclose(library);
        return;
    }
    memcpy(&format_openssl_digest, &symbol, sizeof format_openssl_digest);
    format_openssl_library = library;
}
#endif

format_sha256_backend format_sha256_bytes_fast(const void *data, size_t len,
                                               uint8_t out[32]) {
    const char *mode = getenv("XENOLITH_WEIGHTS_VERIFY");
    if (mode && !strcmp(mode, "metadata")) return FORMAT_SHA256_DISABLED;
#if defined(__linux__) && !defined(XE_NO_OPENSSL)
    pthread_once(&format_openssl_once, format_openssl_init);
    if (format_openssl_library && format_openssl_digest) {
        /* EVP_Q_digest accepts an EVP_MAX_MD_SIZE (64-byte) buffer. */
        uint8_t digest[64];
        size_t size = 0;
        if (format_openssl_digest(NULL, "SHA256", NULL, data, len,
                                  digest, &size) == 1 && size == 32) {
            memcpy(out, digest, 32);
            return FORMAT_SHA256_OPENSSL;
        }
        return FORMAT_SHA256_FAILED;
    }
#endif
    (void)data; (void)len; (void)out;
    return FORMAT_SHA256_UNAVAILABLE;
}

void format_weights_metadata_hash(const void *header, size_t header_len,
                                  size_t file_size, uint8_t out[32]) {
    format_sha256 hash; format_sha256_init(&hash);
    const char domain[] = "xenolith-model-v2";
    uint8_t sizes[16];
    format_put_u64le(sizes, file_size);
    format_put_u64le(sizes + 8, header_len);
    format_sha256_update(&hash, domain, sizeof domain - 1);
    format_sha256_update(&hash, sizes, sizeof sizes);
    format_sha256_update(&hash, header, header_len);
    format_sha256_final(&hash, out);
}

void format_weights_file_identity(const struct stat *info, uint8_t out[32]) {
    uint8_t data[7 * 8];
    format_put_u64le(data, (uint64_t)info->st_dev);
    format_put_u64le(data + 8, (uint64_t)info->st_ino);
    format_put_u64le(data + 16, (uint64_t)info->st_size);
#if defined(__APPLE__)
    format_put_u64le(data + 24, (uint64_t)info->st_mtimespec.tv_sec);
    format_put_u64le(data + 32, (uint64_t)info->st_mtimespec.tv_nsec);
    format_put_u64le(data + 40, (uint64_t)info->st_ctimespec.tv_sec);
    format_put_u64le(data + 48, (uint64_t)info->st_ctimespec.tv_nsec);
#else
    format_put_u64le(data + 24, (uint64_t)info->st_mtim.tv_sec);
    format_put_u64le(data + 32, (uint64_t)info->st_mtim.tv_nsec);
    format_put_u64le(data + 40, (uint64_t)info->st_ctim.tv_sec);
    format_put_u64le(data + 48, (uint64_t)info->st_ctim.tv_nsec);
#endif
    format_sha256_bytes(data, sizeof data, out);
}

void format_weights_verification_warning(format_sha256_backend reason,
                                          const char *artifact) {
    const char *why = reason == FORMAT_SHA256_FAILED ? "OpenSSL SHA-256 failed" :
        reason == FORMAT_SHA256_DISABLED ? "OpenSSL SHA-256 disabled by XENOLITH_WEIGHTS_VERIFY" :
        "OpenSSL 3 could not be loaded";
    fprintf(stderr, "xenolith: warning: %s for %s; using metadata-only weight checks, "
                    "without full content verification.\n", why, artifact);
}

int format_read(FILE *f, void *data, size_t len) {
    uint8_t *p = data;
    while (len) {
        size_t n = fread(p, 1, len, f);
        if (!n) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

int format_write(FILE *f, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        size_t n = fwrite(p, 1, len, f);
        if (!n) return 0;
        p += n;
        len -= n;
    }
    return 1;
}

int format_seek(FILE *f, uint64_t offset) {
    if (offset > (uint64_t)INT64_MAX) return 0;
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
}

int format_file_size(FILE *f, uint64_t *size) {
    off_t current = ftello(f);
    if (current < 0 || fseeko(f, 0, SEEK_END) != 0) return 0;
    off_t end = ftello(f);
    int ok = end >= 0 && fseeko(f, current, SEEK_SET) == 0;
    if (ok) *size = (uint64_t)end;
    return ok;
}
