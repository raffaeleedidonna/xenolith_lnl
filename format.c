#define _POSIX_C_SOURCE 200809L

#include "format.h"

#include <limits.h>
#include <pthread.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
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

static const uint32_t format_sha256_k[64] = {
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

static void format_sha256_block(format_sha256 *s, const uint8_t block[64]) {
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
        uint32_t t1 = h + s1 + ch + format_sha256_k[i] + w[i];
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

static void format_sha256_blocks_c(format_sha256 *s, const uint8_t *p,
                                  size_t blocks) {
    for (size_t i = 0; i < blocks; i++, p += 64)
        format_sha256_block(s, p);
}

#if defined(__x86_64__) || defined(__i386__)
/* Intel SHA state layout follows Jeffrey Walton's public-domain SHA-Intrinsics
 * implementation (based on Intel / Sean Gulley):
 * https://github.com/noloader/SHA-Intrinsics/blob/master/sha256-x86.c
 * Keep the state in registers across blocks. These instructions are isolated
 * here so generic host/test builds need no global -msha flag. */
__attribute__((target("sha,ssse3,sse4.1")))
static void format_sha256_blocks_sha(format_sha256 *s, const uint8_t *p,
                                    size_t blocks) {
    const __m128i swap = _mm_set_epi32(0x0c0d0e0f, 0x08090a0b,
                                     0x04050607, 0x00010203);
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)s->h), 0xb1);
    __m128i cdgh = _mm_shuffle_epi32(
        _mm_loadu_si128((const __m128i *)(s->h + 4)), 0x1b);
    __m128i abef = _mm_alignr_epi8(t, cdgh, 8);
    cdgh = _mm_blend_epi16(cdgh, t, 0xf0);

    for (size_t i = 0; i < blocks; i++, p += 64) {
        __m128i saved_abef = abef, saved_cdgh = cdgh;
        __m128i m0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)p), swap);
        __m128i m1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16)), swap);
        __m128i m2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 32)), swap);
        __m128i m3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 48)), swap);

        /* Four rounds per group; expand the 16-word rolling message schedule
         * with SHA instructions. Unrolling keeps all four vectors in registers. */
#define SHA_ROUNDS(n, m) do { \
    __m128i wk = _mm_add_epi32((m), _mm_loadu_si128( \
        (const __m128i *)(format_sha256_k + (n)))); \
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk); \
    abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(wk, 0x0e)); \
} while (0)
#define SHA_EXPAND(a, b, c, d) do { \
    (a) = _mm_sha256msg1_epu32((a), (b)); \
    (a) = _mm_add_epi32((a), _mm_alignr_epi8((d), (c), 4)); \
    (a) = _mm_sha256msg2_epu32((a), (d)); \
} while (0)
        SHA_ROUNDS(0, m0);
        SHA_ROUNDS(4, m1);
        SHA_ROUNDS(8, m2);
        SHA_ROUNDS(12, m3);
        SHA_EXPAND(m0, m1, m2, m3); SHA_ROUNDS(16, m0);
        SHA_EXPAND(m1, m2, m3, m0); SHA_ROUNDS(20, m1);
        SHA_EXPAND(m2, m3, m0, m1); SHA_ROUNDS(24, m2);
        SHA_EXPAND(m3, m0, m1, m2); SHA_ROUNDS(28, m3);
        SHA_EXPAND(m0, m1, m2, m3); SHA_ROUNDS(32, m0);
        SHA_EXPAND(m1, m2, m3, m0); SHA_ROUNDS(36, m1);
        SHA_EXPAND(m2, m3, m0, m1); SHA_ROUNDS(40, m2);
        SHA_EXPAND(m3, m0, m1, m2); SHA_ROUNDS(44, m3);
        SHA_EXPAND(m0, m1, m2, m3); SHA_ROUNDS(48, m0);
        SHA_EXPAND(m1, m2, m3, m0); SHA_ROUNDS(52, m1);
        SHA_EXPAND(m2, m3, m0, m1); SHA_ROUNDS(56, m2);
        SHA_EXPAND(m3, m0, m1, m2); SHA_ROUNDS(60, m3);
#undef SHA_EXPAND
#undef SHA_ROUNDS
        abef = _mm_add_epi32(abef, saved_abef);
        cdgh = _mm_add_epi32(cdgh, saved_cdgh);
    }
    t = _mm_shuffle_epi32(abef, 0x1b);
    cdgh = _mm_shuffle_epi32(cdgh, 0xb1);
    _mm_storeu_si128((__m128i *)s->h, _mm_blend_epi16(t, cdgh, 0xf0));
    _mm_storeu_si128((__m128i *)(s->h + 4), _mm_alignr_epi8(cdgh, t, 8));
}
#endif

static void (*format_sha256_blocks)(format_sha256 *, const uint8_t *, size_t)
    = format_sha256_blocks_c;
static pthread_once_t format_sha256_once = PTHREAD_ONCE_INIT;

static void format_sha256_select(void) {
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("sha") && __builtin_cpu_supports("ssse3") &&
        __builtin_cpu_supports("sse4.1"))
        format_sha256_blocks = format_sha256_blocks_sha;
#endif
}

void format_sha256_init(format_sha256 *s) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    pthread_once(&format_sha256_once, format_sha256_select);
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
            format_sha256_blocks(s, s->block, 1);
            s->used = 0;
        }
    }
    if (len >= 64) {
        size_t blocks = len / 64;
        format_sha256_blocks(s, p, blocks);
        p += blocks * 64;
        len %= 64;
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
        format_sha256_blocks(s, s->block, 1);
        s->used = 0;
    }
    memset(s->block + s->used, 0, 56 - s->used);
    for (int i = 0; i < 8; i++) s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    format_sha256_blocks(s, s->block, 1);
    for (int i = 0; i < 8; i++) format_put_u32be(out + 4 * i, s->h[i]);
    memset(s, 0, sizeof *s);
}

void format_sha256_bytes(const void *data, size_t len, uint8_t out[32]) {
    format_sha256 s;
    format_sha256_init(&s);
    format_sha256_update(&s, data, len);
    format_sha256_final(&s, out);
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
