#define _GNU_SOURCE
/* Include the implementation to exercise both backends without adding public
 * test controls. Backend overrides below are confined to this single thread,
 * after the concurrent first-use test has joined all workers. */
#include "../format.c"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static void require(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "sha256: FAIL %s\n", what);
        exit(1);
    }
}

static int hex_equal(const uint8_t bytes[32], const char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++)
        if (digits[bytes[i] >> 4] != hex[2 * i] ||
            digits[bytes[i] & 15] != hex[2 * i + 1]) return 0;
    return 1;
}

static void *first_use(void *unused) {
    (void)unused;
    uint8_t out[32];
    for (int i = 0; i < 100; i++) {
        format_sha256_bytes("abc", 3, out);
        require(hex_equal(out,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
            "concurrent first use");
    }
    return NULL;
}

typedef void (*blocks_fn)(format_sha256 *, const uint8_t *, size_t);

static void compare(const uint8_t *p, size_t len, blocks_fn backend) {
    uint8_t expected[32], got[32];
    format_sha256_blocks = format_sha256_blocks_c;
    format_sha256_bytes(p, len, expected);
    format_sha256_blocks = backend;
    format_sha256_bytes(p, len, got);
    require(!memcmp(expected, got, 32), "one-shot backend equivalence");

    /* Alternate tiny, partial-block and multi-block updates. */
    const size_t chunks[] = {1, 3, 55, 64, 65, 257};
    format_sha256 s;
    format_sha256_init(&s);
    for (size_t at = 0, step = 0; at < len; step++) {
        size_t n = chunks[step % (sizeof chunks / sizeof *chunks)];
        if (n > len - at) n = len - at;
        format_sha256_update(&s, p + at, 0);
        format_sha256_update(&s, p + at, n);
        at += n;
    }
    format_sha256_final(&s, got);
    require(!memcmp(expected, got, 32), "incremental backend equivalence");
    const format_sha256 zero = {0};
    require(!memcmp(&s, &zero, sizeof s), "final clears state");
}

static void known_vectors(blocks_fn backend) {
    static const struct { const char *text, *hex; } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
         "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"}
    };
    format_sha256_blocks = backend;
    uint8_t out[32];
    for (size_t i = 0; i < sizeof vectors / sizeof *vectors; i++) {
        format_sha256_bytes(vectors[i].text, strlen(vectors[i].text), out);
        require(hex_equal(out, vectors[i].hex), "known SHA-256 vector");
    }
    uint8_t *million = malloc(1000000);
    require(million != NULL, "allocate million-a vector");
    memset(million, 'a', 1000000);
    format_sha256_bytes(million, 1000000, out);
    require(hex_equal(out,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        "million-a vector");
    free(million);
}

static void check_backend(blocks_fn backend, const char *name) {
    known_vectors(backend);
    uint8_t data[8192 + 64];
    uint32_t rng = 0x132764ab;
    for (size_t i = 0; i < sizeof data; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        data[i] = (uint8_t)rng;
    }
    for (size_t offset = 0; offset < 64; offset++) {
        for (size_t len = 0; len <= 257; len++) compare(data + offset, len, backend);
        const size_t lengths[] = {511, 512, 513, 4095, 4096, 4097, 8192};
        for (size_t i = 0; i < sizeof lengths / sizeof *lengths; i++)
            compare(data + offset, lengths[i], backend);
    }
    /* Exercise every two-update split around padding/block boundaries. */
    for (size_t len = 0; len <= 130; len++) {
        uint8_t expected[32], got[32];
        format_sha256_blocks = format_sha256_blocks_c;
        format_sha256_bytes(data, len, expected);
        format_sha256_blocks = backend;
        for (size_t split = 0; split <= len; split++) {
            format_sha256 s;
            format_sha256_init(&s);
            format_sha256_update(&s, data, split);
            format_sha256_update(&s, data + split, len - split);
            format_sha256_final(&s, got);
            require(!memcmp(expected, got, 32), "every two-update split");
        }
    }
    /* Guard pages catch SIMD loads before/after the caller's accessible data. */
    long page_size = sysconf(_SC_PAGESIZE);
    require(page_size >= 512, "page size");
    size_t page = (size_t)page_size;
    uint8_t *map = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(map != MAP_FAILED, "guard mapping");
    require(mprotect(map + page, page, PROT_READ | PROT_WRITE) == 0, "guard data");
    memset(map + page, 0xa5, page);
    for (size_t len = 0; len <= 257; len++) {
        compare(map + page, len, backend);
        compare(map + 2 * page - len, len, backend);
    }
    compare(map + page, page, backend);
    require(munmap(map, 3 * page) == 0, "unmap guards");
    printf("sha256: %s vectors, alignments, streaming and bounds PASS\n", name);
}

int main(void) {
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++)
        require(!pthread_create(threads + i, NULL, first_use, NULL), "create thread");
    for (size_t i = 0; i < 8; i++)
        require(!pthread_join(threads[i], NULL), "join thread");
    blocks_fn selected = format_sha256_blocks;
    check_backend(format_sha256_blocks_c, "portable C");
#if defined(__x86_64__) || defined(__i386__)
    int has_sha = __builtin_cpu_supports("sha") && __builtin_cpu_supports("ssse3") &&
                  __builtin_cpu_supports("sse4.1");
    require(selected == (has_sha ? format_sha256_blocks_sha : format_sha256_blocks_c),
            "CPU dispatch");
    if (has_sha) check_backend(format_sha256_blocks_sha, "Intel SHA");
    else puts("sha256: Intel SHA SKIP (CPU instructions unavailable)");
#endif
    format_sha256_blocks = selected;
    return 0;
}
