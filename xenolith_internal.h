#ifndef XENOLITH_INTERNAL_H
#define XENOLITH_INTERNAL_H

#include "xenolith.h"

/* Seekable stream serializer; unlike snapshot_save, does not truncate an fd.
 * Used by the disk cache to write shared segments without a full temporary. */
xe_snapshot_status xe_session_snapshot_stream(xe_session *s, FILE *out);

/* Check snapshot identity and token prefix without touching inference state. */
xe_snapshot_status xe_session_snapshot_check_prefix(xe_session *s, FILE *in,
                                                     uint64_t position);

int xe_session_anchor_capture(xe_session *s);
int xe_session_anchor_restore(xe_session *s);
void xe_session_anchor_clear(xe_session *s);
int xe_session_anchor_valid(const xe_session *s);
int xe_session_token_near_top(xe_session *s, int32_t token,
                              int max_rank, float max_margin);
xe_session *xe_session_shadow_new(xe_session *source);
int xe_session_shadow_start(xe_session *s, const xe_tokens *prefix,
                            int max_rows);
int xe_session_shadow_poll(xe_session *s);
int xe_session_shadow_wait(xe_session *s);
int xe_session_shadow_sync(xe_session *s, const xe_tokens *prefix);
void xe_session_shadow_refresh_logits(xe_session *s);
int xe_session_shadow_promote(xe_session *source, xe_session *shadow);
uint64_t xe_session_shadow_kv_bytes(const xe_session *s);
int xe_session_shadow_split(const xe_session *s);

#endif
