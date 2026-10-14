#ifndef SERVE_H
#define SERVE_H

#include "xenolith.h"

#include <stddef.h>

int serve_lock_path(char *out, size_t cap);
int serve_lock(const char *socket_path, char *holder, size_t holder_cap);
int serve_socket_path(const char *override, const char *state_dir, char *out,
                      size_t cap);

int serve_stdio_with_budget(xe_engine *engine, const char *state_dir,
                             const char *cache_dir, uint64_t cache_budget);
int serve_run_with_budget(xe_engine *engine, const char *state_dir, const char *cache_dir,
                           const char *socket_path, double idle_minutes, uint64_t cache_budget);
int serve_stdio(xe_engine *engine, const char *state_dir,
                const char *cache_dir);
int serve_run(xe_engine *engine, const char *state_dir, const char *cache_dir,
              const char *socket_path, double idle_minutes);

#endif
