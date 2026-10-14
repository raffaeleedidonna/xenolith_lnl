#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "kvstore.h"
#include "format.h"

#include <dirent.h>
#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    KVSTORE_META_SIZE = 128,
    KVSTORE_ANCHOR_SIZE = 64,
    KVSTORE_META_KEY = 1
};

typedef struct {
    kvstore_id id;
    uint8_t key[32];
    uint64_t payload_size;
    uint64_t hits;
    int64_t last_hit;
    uint64_t rebuild_cost;
    int64_t created;
    uint32_t flags;
} kvstore_meta;

typedef struct {
    kvstore_meta meta;
    char snapshot_name[37];
    char meta_name[38];
    int pinned;
} kvstore_candidate;

struct kvstore {
    int directory_fd;
    uint64_t budget;
};

__attribute__((weak)) int kvstore_fault(const char *point) {
    (void)point;
    return 0;
}

static int kvstore_all_zero(const uint8_t *p, size_t length) {
    for (size_t i = 0; i < length; i++) if (p[i]) return 0;
    return 1;
}

static int64_t kvstore_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static int kvstore_random(void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        ssize_t n = getrandom(p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        length -= (size_t)n;
    }
    return 1;
}

void kvstore_id_format(const kvstore_id *id, char out[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[id->bytes[i] >> 4];
        out[2 * i + 1] = hex[id->bytes[i] & 15];
    }
    out[32] = '\0';
}

static int kvstore_hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kvstore_id_parse(const char *text, kvstore_id *id) {
    if (!text || !id || strlen(text) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        int high = kvstore_hex_value(text[2 * i]);
        int low = kvstore_hex_value(text[2 * i + 1]);
        if (high < 0 || low < 0) return 0;
        id->bytes[i] = (uint8_t)(high << 4 | low);
    }
    return 1;
}

static void kvstore_names(const kvstore_id *id, char snapshot[37],
                          char meta[38]) {
    kvstore_id_format(id, snapshot);
    memcpy(snapshot + 32, ".xkv", 5);
    memcpy(meta, snapshot, 32);
    memcpy(meta + 32, ".meta", 6);
}

static int kvstore_snapshot_name(const char *name, kvstore_id *id) {
    if (strlen(name) != 36 || memcmp(name + 32, ".xkv", 5) != 0) return 0;
    char hex[33];
    memcpy(hex, name, 32);
    hex[32] = '\0';
    return kvstore_id_parse(hex, id);
}

static int kvstore_anchor_name(const char *name, char out[64]) {
    if (!name || !*name) return 0;
    size_t length = strlen(name);
    if (length > 48) return 0;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) return 0;
    }
    memcpy(out, "anchor-", 7);
    memcpy(out + 7, name, length + 1);
    return 1;
}

static int kvstore_mkdirs(const char *path) {
    if (!path || !*path) return 0;
    char *copy = strdup(path);
    if (!copy) return 0;
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (char *p = copy + (copy[0] == '/'); *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (*copy && mkdir(copy, 0700) != 0 && errno != EEXIST) {
            free(copy);
            return 0;
        }
        *p = '/';
    }
    int ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

static int kvstore_write_all(int fd, const void *data, size_t length) {
    const uint8_t *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        length -= (size_t)n;
    }
    return 1;
}

static int kvstore_read_all(int fd, void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n;
        length -= (size_t)n;
    }
    return 1;
}

static int kvstore_temp_name(char out[64]) {
    kvstore_id id;
    if (!kvstore_random(id.bytes, sizeof id.bytes)) return 0;
    memcpy(out, ".tmp-", 5);
    kvstore_id_format(&id, out + 5);
    return 1;
}

static int kvstore_atomic_bytes(kvstore *store, const char *final_name,
                                const void *data, size_t length,
                                int durable) {
    char temporary[64];
    if (!kvstore_temp_name(temporary)) return 0;
    int fd = openat(store->directory_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return 0;
    int ok = kvstore_write_all(fd, data, length);
    if (ok && durable) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = 0;
    if (ok) ok = renameat(store->directory_fd, temporary,
                          store->directory_fd, final_name) == 0;
    if (ok && durable) ok = fsync(store->directory_fd) == 0;
    if (!ok) unlinkat(store->directory_fd, temporary, 0);
    return ok;
}

static void kvstore_meta_encode(const kvstore_meta *meta,
                                uint8_t out[KVSTORE_META_SIZE]) {
    memset(out, 0, KVSTORE_META_SIZE);
    memcpy(out, "XEKVMETA", 8);
    format_put_u32le(out + 8, 1);
    format_put_u32le(out + 12, meta->flags);
    memcpy(out + 16, meta->id.bytes, 16);
    memcpy(out + 32, meta->key, 32);
    format_put_u64le(out + 64, meta->payload_size);
    format_put_u64le(out + 72, meta->hits);
    format_put_u64le(out + 80, (uint64_t)meta->last_hit);
    format_put_u64le(out + 88, meta->rebuild_cost);
    format_put_u64le(out + 96, (uint64_t)meta->created);
    format_put_u64le(out + 120, format_crc64(0, out, 120));
}

static int kvstore_meta_decode(const uint8_t in[KVSTORE_META_SIZE],
                               const kvstore_id *id, kvstore_meta *meta) {
    if (memcmp(in, "XEKVMETA", 8) != 0 || format_get_u32le(in + 8) != 1 ||
        format_get_u32le(in + 12) & ~KVSTORE_META_KEY ||
        memcmp(in + 16, id->bytes, 16) != 0 ||
        !kvstore_all_zero(in + 104, 16) ||
        format_get_u64le(in + 120) != format_crc64(0, in, 120)) return 0;
    memset(meta, 0, sizeof *meta);
    meta->id = *id;
    meta->flags = format_get_u32le(in + 12);
    memcpy(meta->key, in + 32, 32);
    meta->payload_size = format_get_u64le(in + 64);
    meta->hits = format_get_u64le(in + 72);
    meta->last_hit = (int64_t)format_get_u64le(in + 80);
    meta->rebuild_cost = format_get_u64le(in + 88);
    meta->created = (int64_t)format_get_u64le(in + 96);
    return 1;
}

static int kvstore_meta_read(kvstore *store, const kvstore_id *id,
                             kvstore_meta *meta) {
    char snapshot[37], name[38];
    kvstore_names(id, snapshot, name);
    int fd = openat(store->directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    uint8_t bytes[KVSTORE_META_SIZE];
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             st.st_size == KVSTORE_META_SIZE &&
             kvstore_read_all(fd, bytes, sizeof bytes);
    if (close(fd) != 0) ok = 0;
    return ok && kvstore_meta_decode(bytes, id, meta);
}

static int kvstore_meta_write(kvstore *store, const kvstore_meta *meta,
                              int durable) {
    char snapshot[37], name[38];
    kvstore_names(&meta->id, snapshot, name);
    uint8_t bytes[KVSTORE_META_SIZE];
    kvstore_meta_encode(meta, bytes);
    return kvstore_atomic_bytes(store, name, bytes, sizeof bytes, durable);
}

static void kvstore_anchor_encode(const kvstore_id *id,
                                  uint8_t out[KVSTORE_ANCHOR_SIZE]) {
    memset(out, 0, KVSTORE_ANCHOR_SIZE);
    memcpy(out, "XEKVANCH", 8);
    format_put_u32le(out + 8, 1);
    memcpy(out + 16, id->bytes, 16);
    format_put_u64le(out + 56, format_crc64(0, out, 56));
}

static int kvstore_anchor_decode(const uint8_t in[KVSTORE_ANCHOR_SIZE],
                                 kvstore_id *id) {
    if (memcmp(in, "XEKVANCH", 8) != 0 || format_get_u32le(in + 8) != 1 ||
        format_get_u32le(in + 12) != 0 || !kvstore_all_zero(in + 32, 24) ||
        format_get_u64le(in + 56) != format_crc64(0, in, 56)) return 0;
    memcpy(id->bytes, in + 16, 16);
    return 1;
}

static int kvstore_anchor_read_name(kvstore *store, const char *filename,
                                    kvstore_id *id) {
    int fd = openat(store->directory_fd, filename,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    uint8_t bytes[KVSTORE_ANCHOR_SIZE];
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             st.st_size == KVSTORE_ANCHOR_SIZE &&
             kvstore_read_all(fd, bytes, sizeof bytes);
    if (close(fd) != 0) ok = 0;
    return ok && kvstore_anchor_decode(bytes, id) ? 1 : -1;
}

static int kvstore_id_equal(const kvstore_id *a, const kvstore_id *b) {
    return memcmp(a->bytes, b->bytes, sizeof a->bytes) == 0;
}

static int kvstore_collect_anchors(kvstore *store, kvstore_id **ids,
                                   size_t *count) {
    *ids = NULL;
    *count = 0;
    int duplicate = openat(store->directory_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (duplicate < 0) return 0;
    DIR *directory = fdopendir(duplicate);
    if (!directory) {
        close(duplicate);
        return 0;
    }
    size_t capacity = 0;
    struct dirent *entry;
    int ok = 1;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "anchor-", 7) != 0) continue;
        kvstore_id id;
        if (kvstore_anchor_read_name(store, entry->d_name, &id) != 1) continue;
        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            kvstore_id *grown = realloc(*ids, next * sizeof(**ids));
            if (!grown) {
                ok = 0;
                break;
            }
            *ids = grown;
            capacity = next;
        }
        (*ids)[(*count)++] = id;
    }
    closedir(directory);
    if (!ok) {
        free(*ids);
        *ids = NULL;
        *count = 0;
    }
    return ok;
}

static int kvstore_id_anchored(const kvstore_id *id, const kvstore_id *anchors,
                               size_t count) {
    for (size_t i = 0; i < count; i++)
        if (kvstore_id_equal(id, &anchors[i])) return 1;
    return 0;
}

static int kvstore_collect_candidates(kvstore *store,
                                      kvstore_candidate **candidates,
                                      size_t *count, uint64_t *total,
                                      uint64_t *pinned) {
    *candidates = NULL;
    *count = 0;
    *total = 0;
    *pinned = 0;
    kvstore_id *anchors = NULL;
    size_t anchor_count = 0;
    if (!kvstore_collect_anchors(store, &anchors, &anchor_count)) return 0;
    int duplicate = openat(store->directory_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (duplicate < 0) {
        free(anchors);
        return 0;
    }
    DIR *directory = fdopendir(duplicate);
    if (!directory) {
        close(duplicate);
        free(anchors);
        return 0;
    }
    size_t capacity = 0;
    int ok = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        kvstore_id id;
        if (!kvstore_snapshot_name(entry->d_name, &id)) continue;
        struct stat st;
        if (fstatat(store->directory_fd, entry->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
            st.st_size < 0) continue;
        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            kvstore_candidate *grown = realloc(
                *candidates, next * sizeof(**candidates));
            if (!grown) {
                ok = 0;
                break;
            }
            *candidates = grown;
            capacity = next;
        }
        kvstore_candidate *candidate = &(*candidates)[(*count)++];
        memset(candidate, 0, sizeof *candidate);
        candidate->meta.id = id;
        candidate->meta.payload_size = (uint64_t)st.st_size;
        kvstore_names(&id, candidate->snapshot_name, candidate->meta_name);
        kvstore_meta loaded;
        if (kvstore_meta_read(store, &id, &loaded)) {
            candidate->meta = loaded;
            candidate->meta.payload_size = (uint64_t)st.st_size;
        }
        candidate->pinned = kvstore_id_anchored(&id, anchors, anchor_count);
        if (*total > UINT64_MAX - (uint64_t)st.st_size) {
            ok = 0;
            break;
        }
        *total += (uint64_t)st.st_size;
        if (candidate->pinned) *pinned += (uint64_t)st.st_size;
    }
    closedir(directory);
    free(anchors);
    if (!ok) {
        free(*candidates);
        *candidates = NULL;
        *count = 0;
    }
    return ok;
}

static long double kvstore_candidate_score(const kvstore_candidate *candidate,
                                           int64_t now) {
    uint64_t hits = candidate->meta.hits;
    const int64_t half_life = INT64_C(30) * 24 * 60 * 60 * 1000000000;
    if (hits && candidate->meta.last_hit > 0 && now > candidate->meta.last_hit) {
        uint64_t periods = (uint64_t)((now - candidate->meta.last_hit) /
                                      half_life);
        hits = periods >= 64 ? 0 : hits >> periods;
    }
    uint64_t cost = candidate->meta.rebuild_cost;
    if (!cost) cost = candidate->meta.payload_size;
    if (!candidate->meta.payload_size) return LDBL_MAX;
    return (1.0L + (long double)hits) * (long double)cost /
           (long double)candidate->meta.payload_size;
}

static int kvstore_candidate_worse(const kvstore_candidate *a,
                                   const kvstore_candidate *b, int64_t now) {
    long double as = kvstore_candidate_score(a, now);
    long double bs = kvstore_candidate_score(b, now);
    if (as != bs) return as < bs;
    return memcmp(a->meta.id.bytes, b->meta.id.bytes, 16) < 0;
}

static kvstore_status kvstore_make_room(kvstore *store, uint64_t reserve,
                                        int reserve_pinned) {
    kvstore_candidate *candidates;
    size_t count;
    uint64_t total, pinned;
    if (!kvstore_collect_candidates(store, &candidates, &count, &total,
                                    &pinned)) return KVSTORE_IO;
    if (!reserve_pinned && (reserve > store->budget ||
                            pinned > store->budget - reserve)) {
        free(candidates);
        return KVSTORE_BUDGET;
    }
    int64_t now = kvstore_now_ns();
    int changed = 0;
    while (total > store->budget || reserve > store->budget -
           (total < store->budget ? total : store->budget)) {
        size_t victim = count;
        for (size_t i = 0; i < count; i++) {
            if (candidates[i].pinned || !candidates[i].meta.payload_size)
                continue;
            if (victim == count || kvstore_candidate_worse(
                    &candidates[i], &candidates[victim], now)) victim = i;
        }
        if (victim == count) break;
        kvstore_candidate *candidate = &candidates[victim];
        if (unlinkat(store->directory_fd, candidate->snapshot_name, 0) != 0 &&
            errno != ENOENT) {
            free(candidates);
            return KVSTORE_IO;
        }
        unlinkat(store->directory_fd, candidate->meta_name, 0);
        total -= candidate->meta.payload_size;
        candidate->meta.payload_size = 0;
        changed = 1;
    }
    if (changed && fsync(store->directory_fd) != 0) {
        free(candidates);
        return KVSTORE_IO;
    }
    int enough = reserve_pinned ||
                 (total <= store->budget && reserve <= store->budget - total);
    free(candidates);
    return enough ? KVSTORE_OK : KVSTORE_BUDGET;
}

static void kvstore_cleanup_temps(kvstore *store) {
    int duplicate = openat(store->directory_fd, ".",
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (duplicate < 0) return;
    DIR *directory = fdopendir(duplicate);
    if (!directory) {
        close(duplicate);
        return;
    }
    int changed = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, ".tmp-", 5) != 0) continue;
        if (unlinkat(store->directory_fd, entry->d_name, 0) == 0) changed = 1;
    }
    closedir(directory);
    if (changed) fsync(store->directory_fd);
}

kvstore_status kvstore_open(kvstore **out, const char *directory,
                            uint64_t budget) {
    if (!out || !directory || !*directory) return KVSTORE_INVALID_ARGUMENT;
    *out = NULL;
    if (!kvstore_mkdirs(directory)) return KVSTORE_IO;
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return KVSTORE_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        fchmod(fd, 0700) != 0) {
        close(fd);
        return KVSTORE_IO;
    }
    kvstore *store = calloc(1, sizeof *store);
    if (!store) {
        close(fd);
        return KVSTORE_IO;
    }
    store->directory_fd = fd;
    store->budget = budget;
    kvstore_cleanup_temps(store);
    kvstore_status status = kvstore_make_room(store, 0, 0);
    if (status != KVSTORE_OK && status != KVSTORE_BUDGET) {
        kvstore_close(store);
        return status;
    }
    *out = store;
    return KVSTORE_OK;
}

void kvstore_close(kvstore *store) {
    if (!store) return;
    close(store->directory_fd);
    free(store);
}

kvstore_status kvstore_save(kvstore *store, xe_session *session,
                            const kvstore_save_options *options,
                            kvstore_id *id,
                            xe_snapshot_status *snapshot_status) {
    if (snapshot_status) *snapshot_status = XE_SNAPSHOT_OK;
    if (!store || !session || !id) return KVSTORE_INVALID_ARGUMENT;
    if (!store->budget) return KVSTORE_DISABLED;
    kvstore_save_options defaults;
    memset(&defaults, 0, sizeof defaults);
    if (!options) options = &defaults;
    if ((options->has_key != 0 && options->has_key != 1) ||
        (options->pinned != 0 && options->pinned != 1))
        return KVSTORE_INVALID_ARGUMENT;
    uint64_t size;
    xe_snapshot_status core = xe_session_snapshot_size(session, &size);
    if (core != XE_SNAPSHOT_OK) {
        if (snapshot_status) *snapshot_status = core;
        return KVSTORE_REJECTED;
    }
    kvstore_status status = kvstore_make_room(store, size, options->pinned);
    if (status != KVSTORE_OK) return status;

    char snapshot_name[37], meta_name[38], temporary[64];
    int fd = -1;
    for (int attempt = 0; attempt < 32; attempt++) {
        if (!kvstore_random(id->bytes, sizeof id->bytes)) return KVSTORE_IO;
        kvstore_names(id, snapshot_name, meta_name);
        if (faccessat(store->directory_fd, snapshot_name, F_OK, 0) == 0)
            continue;
        if (errno != ENOENT) return KVSTORE_IO;
        if (!kvstore_temp_name(temporary)) return KVSTORE_IO;
        fd = openat(store->directory_fd, temporary,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) return KVSTORE_IO;
    }
    if (fd < 0) return KVSTORE_IO;
    FILE *file = fdopen(fd, "w+b");
    if (!file) {
        close(fd);
        unlinkat(store->directory_fd, temporary, 0);
        return KVSTORE_IO;
    }
    core = xe_session_snapshot_save(session, file);
    int ok = core == XE_SNAPSHOT_OK && fflush(file) == 0;
    if (ok && kvstore_fault("snapshot-written")) ok = 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (ok && kvstore_fault("snapshot-synced")) ok = 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        unlinkat(store->directory_fd, temporary, 0);
        if (snapshot_status) *snapshot_status = core;
        return core == XE_SNAPSHOT_OK ? KVSTORE_IO : KVSTORE_REJECTED;
    }
    if (renameat2(store->directory_fd, temporary, store->directory_fd,
                  snapshot_name, RENAME_NOREPLACE) != 0) {
        unlinkat(store->directory_fd, temporary, 0);
        return KVSTORE_IO;
    }
    if (kvstore_fault("snapshot-renamed") ||
        fsync(store->directory_fd) != 0 ||
        kvstore_fault("snapshot-directory-synced")) {
        unlinkat(store->directory_fd, snapshot_name, 0);
        fsync(store->directory_fd);
        return KVSTORE_IO;
    }

    kvstore_meta meta;
    memset(&meta, 0, sizeof meta);
    meta.id = *id;
    meta.payload_size = size;
    meta.rebuild_cost = options->rebuild_cost;
    meta.created = kvstore_now_ns();
    if (options->has_key) {
        meta.flags |= KVSTORE_META_KEY;
        memcpy(meta.key, options->key, sizeof meta.key);
    }
    if (!kvstore_meta_write(store, &meta, 1)) {
        unlinkat(store->directory_fd, snapshot_name, 0);
        fsync(store->directory_fd);
        return KVSTORE_IO;
    }
    return KVSTORE_OK;
}

static kvstore_status kvstore_load_impl(kvstore *store, const kvstore_id *id,
                            xe_session *session, const xe_tokens *expected, const xe_input *mixed,
                            xe_snapshot_status *snapshot_status) {
    if (snapshot_status) *snapshot_status = XE_SNAPSHOT_OK;
    if (!store || !id || !session || !expected)
        return KVSTORE_INVALID_ARGUMENT;
    if (!store->budget) return KVSTORE_DISABLED;
    char snapshot_name[37], meta_name[38];
    kvstore_names(id, snapshot_name, meta_name);
    int fd = openat(store->directory_fd, snapshot_name,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? KVSTORE_MISS : KVSTORE_IO;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return KVSTORE_IO;
    }
    FILE *file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        return KVSTORE_IO;
    }
    xe_snapshot_status core = mixed ? xe_session_snapshot_load_input(session,file,mixed) :
                              xe_session_snapshot_load(session,file,expected);
    int close_ok = fclose(file) == 0;
    if (snapshot_status) *snapshot_status = core;
    if (!close_ok) return KVSTORE_IO;
    if (core != XE_SNAPSHOT_OK) return KVSTORE_REJECTED;

    kvstore_meta meta;
    if (!kvstore_meta_read(store, id, &meta)) {
        memset(&meta, 0, sizeof meta);
        meta.id = *id;
        meta.payload_size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
        meta.created = kvstore_now_ns();
    }
    if (meta.hits != UINT64_MAX) meta.hits++;
    meta.last_hit = kvstore_now_ns();
    kvstore_meta_write(store, &meta, 0);
    return KVSTORE_OK;
}

kvstore_status kvstore_load(kvstore *store,const kvstore_id *id,xe_session *session,
                            const xe_tokens *expected,xe_snapshot_status *status) {
    return kvstore_load_impl(store,id,session,expected,NULL,status);
}

kvstore_status kvstore_load_input(kvstore *store,const kvstore_id *id,xe_session *session,
                                  const xe_input *input,xe_snapshot_status *status) {
    if(!input){if(status)*status=XE_SNAPSHOT_INVALID_ARGUMENT;return KVSTORE_INVALID_ARGUMENT;}
    xe_tokens expected={(int32_t *)input->tokens,input->count,input->count};
    return kvstore_load_impl(store,id,session,&expected,input,status);
}

kvstore_status kvstore_find(kvstore *store, const uint8_t key[32],
                            kvstore_id *id) {
    if (!store || !key || !id) return KVSTORE_INVALID_ARGUMENT;
    if (!store->budget) return KVSTORE_DISABLED;
    kvstore_candidate *candidates;
    size_t count;
    uint64_t total, pinned;
    if (!kvstore_collect_candidates(store, &candidates, &count, &total,
                                    &pinned)) return KVSTORE_IO;
    int found = 0;
    kvstore_meta best;
    for (size_t i = 0; i < count; i++) {
        kvstore_meta *meta = &candidates[i].meta;
        if (!(meta->flags & KVSTORE_META_KEY) ||
            memcmp(meta->key, key, 32) != 0) continue;
        if (!found || meta->created > best.created ||
            (meta->created == best.created &&
             memcmp(meta->id.bytes, best.id.bytes, 16) < 0)) {
            best = *meta;
            found = 1;
        }
    }
    if (found) *id = best.id;
    free(candidates);
    return found ? KVSTORE_OK : KVSTORE_MISS;
}

kvstore_status kvstore_anchor_get(kvstore *store, const char *name,
                                  kvstore_id *id) {
    if (!store || !id) return KVSTORE_INVALID_ARGUMENT;
    char filename[64];
    if (!kvstore_anchor_name(name, filename)) return KVSTORE_INVALID_ARGUMENT;
    int result = kvstore_anchor_read_name(store, filename, id);
    return result == 1 ? KVSTORE_OK : result == 0 ? KVSTORE_MISS : KVSTORE_IO;
}

kvstore_status kvstore_anchor_set(kvstore *store, const char *name,
                                  const kvstore_id *id) {
    if (!store || !id) return KVSTORE_INVALID_ARGUMENT;
    char filename[64], snapshot[37], meta_name[38];
    if (!kvstore_anchor_name(name, filename)) return KVSTORE_INVALID_ARGUMENT;
    kvstore_names(id, snapshot, meta_name);
    struct stat st;
    if (fstatat(store->directory_fd, snapshot, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? KVSTORE_MISS : KVSTORE_IO;
    if (!S_ISREG(st.st_mode)) return KVSTORE_IO;
    uint8_t bytes[KVSTORE_ANCHOR_SIZE];
    kvstore_anchor_encode(id, bytes);
    if (!kvstore_atomic_bytes(store, filename, bytes, sizeof bytes, 1))
        return KVSTORE_IO;
    return kvstore_make_room(store, 0, 0);
}

kvstore_status kvstore_anchor_clear(kvstore *store, const char *name) {
    if (!store) return KVSTORE_INVALID_ARGUMENT;
    char filename[64];
    if (!kvstore_anchor_name(name, filename)) return KVSTORE_INVALID_ARGUMENT;
    if (unlinkat(store->directory_fd, filename, 0) != 0)
        return errno == ENOENT ? KVSTORE_MISS : KVSTORE_IO;
    if (fsync(store->directory_fd) != 0) return KVSTORE_IO;
    return kvstore_make_room(store, 0, 0);
}

kvstore_status kvstore_remove(kvstore *store, const kvstore_id *id) {
    if (!store || !id) return KVSTORE_INVALID_ARGUMENT;
    kvstore_id *anchors = NULL;
    size_t anchor_count = 0;
    if (!kvstore_collect_anchors(store, &anchors, &anchor_count))
        return KVSTORE_IO;
    int anchored = kvstore_id_anchored(id, anchors, anchor_count);
    free(anchors);
    if (anchored) return KVSTORE_PINNED;
    char snapshot[37], meta_name[38];
    kvstore_names(id, snapshot, meta_name);
    if (unlinkat(store->directory_fd, snapshot, 0) != 0)
        return errno == ENOENT ? KVSTORE_MISS : KVSTORE_IO;
    unlinkat(store->directory_fd, meta_name, 0);
    return fsync(store->directory_fd) == 0 ? KVSTORE_OK : KVSTORE_IO;
}

kvstore_status kvstore_enforce_budget(kvstore *store) {
    if (!store) return KVSTORE_INVALID_ARGUMENT;
    return kvstore_make_room(store, 0, 0);
}

kvstore_status kvstore_usage(kvstore *store, uint64_t *total,
                             uint64_t *pinned) {
    if (!store || !total || !pinned) return KVSTORE_INVALID_ARGUMENT;
    kvstore_candidate *candidates;
    size_t count;
    if (!kvstore_collect_candidates(store, &candidates, &count, total, pinned))
        return KVSTORE_IO;
    free(candidates);
    return KVSTORE_OK;
}

const char *kvstore_status_name(kvstore_status status) {
    static const char *names[] = {
        "ok", "miss", "disabled", "invalid-argument", "io", "budget",
        "rejected", "pinned"
    };
    return (unsigned)status < sizeof names / sizeof names[0]
           ? names[status] : "unknown";
}
