#define XE_TEST_SESSION
#define XE_TEST_OUTPUT_COUNT
#include "../xenolith.c"
#include "../kvstore.h"
#include <dirent.h>

size_t xe_test_prefill_batches, xe_test_decode_tokens;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    xe_engine *e = xe_engine_open_with_context(argv[1], 4097);
    xe_session *s = xe_session_new(e);
    char path[4096]; const char *tmp = getenv("TMPDIR"); if (!tmp) tmp = "/tmp";
    CHECK(snprintf(path, sizeof path, "%s/xenolith-cache-model-XXXXXX", tmp) < (int)sizeof path);
    CHECK(mkdtemp(path));
    kvstore *cache; CHECK(kvstore_open(&cache, path, UINT64_C(1073741824)) == KVSTORE_OK);
    int32_t ids[1080]; for (int i = 0; i < 1080; i++) ids[i] = 2+i;
    xe_tokens prefix = {ids, 512, 1080}; xe_session_sync(s, &prefix);
    kvstore_save_options options = {.has_group = 1, .rebuild_cost = 512};
    options.group.bytes[0] = 1;
    kvstore_id older, latest;
    CHECK(kvstore_save(cache, s, &options, &older, NULL) == KVSTORE_OK);
    prefix.len = 1057; xe_session_sync(s, &prefix); options.rebuild_cost = 1057;
    CHECK(kvstore_save(cache, s, &options, &latest, NULL) == KVSTORE_OK);
    float *oracle = malloc(3 * XE_VOCAB * sizeof(float)); CHECK(oracle);
    memcpy(oracle, xe_session_logits(s), XE_VOCAB * sizeof(float));
    xe_sampler greedy = {0.0f, 1, 1.0f, 1}; ids[1057] = xe_session_next(s, &greedy);
    prefix.len = 1058; xe_session_sync(s, &prefix);
    memcpy(oracle + XE_VOCAB, xe_session_logits(s), XE_VOCAB * sizeof(float));
    prefix.len = 1065; xe_session_sync(s, &prefix);
    memcpy(oracle + 2*XE_VOCAB, xe_session_logits(s), XE_VOCAB * sizeof(float));
    CHECK(kvstore_remove(cache, &older) == KVSTORE_OK);
    kvstore_close(cache); CHECK(kvstore_open(&cache, path, UINT64_C(1073741824)) == KVSTORE_OK);
    CHECK(kvstore_enforce_budget(cache) == KVSTORE_OK);
    xe_session_reset(s); prefix.len = 1057;
    xe_test_prefill_batches = xe_test_decode_tokens = xe_test_output_calls = 0;
    CHECK(kvstore_load(cache, &latest, s, &prefix, NULL) == KVSTORE_OK);
    CHECK(xe_test_prefill_batches == 0 && xe_test_decode_tokens == 0 && xe_test_output_calls == 0);
    CHECK(memcmp(oracle, xe_session_logits(s), XE_VOCAB * sizeof(float)) == 0);
    CHECK(xe_session_next(s, &greedy) == ids[1057]);
    prefix.len = 1058; xe_session_sync(s, &prefix);
    CHECK(memcmp(oracle + XE_VOCAB, xe_session_logits(s), XE_VOCAB * sizeof(float)) == 0);
    prefix.len = 1065; xe_session_sync(s, &prefix);
    CHECK(memcmp(oracle + 2*XE_VOCAB, xe_session_logits(s), XE_VOCAB * sizeof(float)) == 0);
    CHECK(kvstore_remove(cache, &latest) == KVSTORE_OK);
    uint64_t total, pinned; CHECK(kvstore_usage(cache, &total, &pinned) == KVSTORE_OK && total == 0);
    kvstore_close(cache);
    char lock[4200]; snprintf(lock, sizeof lock, "%s/.lock", path); CHECK(unlink(lock) == 0 && rmdir(path) == 0);
    free(oracle); xe_session_free(s); xe_engine_close(e);
    puts("cache-model: shared 512/1057, ancestor removal, reopen, zero-prefill and exact CPU/GPU continuation PASS");
    return 0;
}
