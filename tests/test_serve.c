#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "../serve.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                #condition); \
    } \
} while (0)

typedef struct {
    int fd;
    char buf[262144];
    size_t length;
} client;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void test_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int client_connect(client *c, const char *path) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    for (int attempt = 0; attempt < 200; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path, strlen(path));
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            c->fd = fd;
            return 1;
        }
        close(fd);
        test_sleep_ms(50);
    }
    return 0;
}

static void client_close(client *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int client_send(client *c, const char *line) {
    size_t length = strlen(line);
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(c->fd, line + sent, length - sent);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int client_recv(client *c, char *out, size_t cap, int timeout_ms) {
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->length);
        if (nl) {
            size_t length = (size_t)(nl - c->buf);
            if (length >= cap) length = cap - 1;
            memcpy(out, c->buf, length);
            out[length] = '\0';
            size_t used = (size_t)(nl - c->buf) + 1;
            memmove(c->buf, c->buf + used, c->length - used);
            c->length -= used;
            return 1;
        }
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) return 0;
        if (c->length >= sizeof c->buf) return 0;
        ssize_t n = read(c->fd, c->buf + c->length,
                         sizeof c->buf - c->length);
        if (n <= 0) return 0;
        c->length += (size_t)n;
    }
}

static int client_eof(client *c, int timeout_ms) {
    for (int i = 0; i < 64; i++) {
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) return 0;
        char drain[65536];
        ssize_t n = read(c->fd, drain, sizeof drain);
        if (n == 0) return 1;
        if (n < 0) return errno == ECONNRESET;
    }
    return 0;
}


/* Large-frame helpers (3.7 finding 3): a growable reader for responses
 * beyond the fixed client buffer. */
static int client_recv_big(client *c, char **out, size_t *out_length,
                           int timeout_ms) {
    size_t cap = 1 << 20, length = 0;
    char *buf = malloc(cap);
    if (!buf) return 0;
    if (c->length) {
        memcpy(buf, c->buf, c->length);
        length = c->length;
        c->length = 0;
    }
    for (;;) {
        char *nl = memchr(buf, '\n', length);
        if (nl) {
            size_t used = (size_t)(nl - buf) + 1;
            *nl = '\0';
            if (length > used) {
                memcpy(c->buf, buf + used, length - used);
                c->length = length - used;
            }
            *out = buf;
            *out_length = used - 1;
            return 1;
        }
        if (length == cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); return 0; }
            buf = grown;
        }
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) { free(buf); return 0; }
        ssize_t n = read(c->fd, buf + length, cap - length);
        if (n <= 0) { free(buf); return 0; }
        length += (size_t)n;
    }
}

static const char *italian_paragraph =
    "Il problema \xc3\xa8 che la compaction, per costruzione, \xc3\xa8 a zero "
    "prefill: il riassunto viene generato accodando alla sessione viva, quindi "
    "la cache \xc3\xa8 gi\xc3\xa0 calcolata. Per\xc3\xb2 prima della compaction "
    "la sessione pu\xc3\xb2 avvicinarsi alla finestra, ed \xc3\xa8 l\xc3\xac che "
    "il limite per riga pu\xc3\xb2 mordere: una ricostruzione strutturale o una "
    "riconciliazione dopo il riavvio devono trasportare l'intero trascritto in "
    "una sola riga. Perci\xc3\xb2 misuriamo, non stimiamo: quanti byte costa "
    "davvero un token di prosa italiana, e dove cade il confine.";

/* Builds a rebuild request of `pairs` user/assistant turns, each side
 * `reps` paragraphs long (127 tokens per paragraph on this tokenizer). */
static char *build_rebuild(int pairs, int reps, size_t *length) {
    size_t para = strlen(italian_paragraph);
    size_t cap = 256 + (size_t)pairs * 2 * ((size_t)reps * (para + 1) + 64);
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n,
                          "{\"op\":\"rebuild\",\"system\":\"Sei un assistente.\","
                          "\"messages\":[");
    for (int i = 0; i < pairs; i++) {
        for (int side = 0; side < 2; side++) {
            n += (size_t)snprintf(out + n, cap - n, "%s{\"role\":\"%s\",\"text\":\"",
                                  (i || side) ? "," : "",
                                  side ? "assistant" : "user");
            for (int r = 0; r < reps; r++) {
                memcpy(out + n, italian_paragraph, para);
                n += para;
                out[n++] = ' ';
            }
            n += (size_t)snprintf(out + n, cap - n, "\"}");
        }
    }
    n += (size_t)snprintf(out + n, cap - n, "]}\n");
    *length = n;
    return out;
}

static int client_call(client *c, const char *request, char *out,
                       size_t cap) {
    if (!client_send(c, request)) return 0;
    return client_recv(c, out, cap, 15000);
}

static int session_of(const char *line, char *out) {
    const char *p = strstr(line, "\"session\":\"");
    if (!p) return 0;
    p += strlen("\"session\":\"");
    if (strlen(p) < 32) return 0;
    memcpy(out, p, 32);
    out[32] = '\0';
    return 1;
}

static int lock_child(int start_fd, int report_fd, int hold_fd) {
    char byte;
    if (read(start_fd, &byte, 1) != 1) return 2;
    char holder[256];
    int fd = serve_lock(NULL, holder, sizeof holder);
    char status = fd < 0 ? 'l' : 'w';
    if (write(report_fd, &status, 1) != 1) return 2;
    if (fd < 0) return 1;
    if (read(hold_fd, &byte, 1) != 1) return 2;
    close(fd);
    return 0;
}

static int foreign_state_child(const char *state_home) {
    if (setenv("XDG_STATE_HOME", state_home, 1) != 0) return 2;
    char holder[256];
    int fd = serve_lock(NULL, holder, sizeof holder);
    if (fd < 0) return 1;
    close(fd);
    return 0;
}

static int foreign_runtime_child(const char *runtime_dir,
                                 const char *socket_path) {
    if (setenv("XDG_RUNTIME_DIR", runtime_dir, 1) != 0) return 2;
    char holder[256];
    int fd = serve_lock(socket_path, holder, sizeof holder);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int stdio_child(xe_engine *engine, const char *state_dir,
                       const char *cache_dir, int in_fd, int out_fd) {
    if (dup2(in_fd, 0) < 0 || dup2(out_fd, 1) < 0) return 2;
    close(in_fd);
    close(out_fd);
    return serve_stdio(engine, state_dir, cache_dir);
}

int main(int argc, char **argv) {
    if (argc < 2 || !argv[1][0]) {
        fprintf(stderr, "usage: %s <model.gguf>\n", argv[0]);
        return 2;
    }
    const char *model = argv[1];
    char state_dir[] = "/tmp/xenolith-test-serve-state-XXXXXX";
    char cache_dir[] = "/tmp/xenolith-test-serve-cache-XXXXXX";
    char runtime_dir[] = "/tmp/xenolith-test-serve-run-XXXXXX";
    char state_home[] = "/tmp/xenolith-test-serve-home-XXXXXX";
    char other_state[] = "/tmp/xenolith-test-serve-other-XXXXXX";
    char other_runtime[] = "/tmp/xenolith-test-serve-run2-XXXXXX";
    CHECK(mkdtemp(state_dir) != NULL);
    CHECK(mkdtemp(cache_dir) != NULL);
    CHECK(mkdtemp(runtime_dir) != NULL);
    CHECK(mkdtemp(state_home) != NULL);
    CHECK(mkdtemp(other_state) != NULL);
    CHECK(mkdtemp(other_runtime) != NULL);
    CHECK(setenv("XDG_RUNTIME_DIR", runtime_dir, 1) == 0);
    CHECK(setenv("XDG_STATE_HOME", state_home, 1) == 0);
    signal(SIGPIPE, SIG_IGN);

    char socket_path[256];
    snprintf(socket_path, sizeof socket_path, "%s/wire.sock", state_dir);
    char other_socket[256];
    snprintf(other_socket, sizeof other_socket, "%s/wire.sock", other_runtime);

    char lock_path[4096];
    CHECK(serve_lock_path(lock_path, sizeof lock_path));
    CHECK(strncmp(lock_path, state_home, strlen(state_home)) == 0);
    CHECK(strstr(lock_path, "/xenolith/engine.lock") != NULL);

    char holder[256];
    int lock_fd = serve_lock(socket_path, holder, sizeof holder);
    CHECK(lock_fd >= 0);
    char pid_text[64];
    snprintf(pid_text, sizeof pid_text, "pid %ld", (long)getpid());
    holder[0] = '\0';
    CHECK(serve_lock(NULL, holder, sizeof holder) < 0);
    CHECK(strstr(holder, pid_text) != NULL);
    CHECK(strstr(holder, socket_path) != NULL);

    fflush(NULL);
    pid_t foreign = fork();
    CHECK(foreign >= 0);
    if (foreign == 0) _exit(foreign_state_child(other_state));
    int foreign_status = 0;
    waitpid(foreign, &foreign_status, 0);
    CHECK(WIFEXITED(foreign_status) && WEXITSTATUS(foreign_status) == 0);

    fflush(NULL);
    pid_t neighbour = fork();
    CHECK(neighbour >= 0);
    if (neighbour == 0)
        _exit(foreign_runtime_child(other_runtime, other_socket));
    int neighbour_status = 0;
    waitpid(neighbour, &neighbour_status, 0);
    CHECK(WIFEXITED(neighbour_status) &&
          WEXITSTATUS(neighbour_status) == 0);

    if (lock_fd >= 0) close(lock_fd);
    lock_fd = serve_lock(NULL, holder, sizeof holder);
    CHECK(lock_fd >= 0);
    if (lock_fd >= 0) close(lock_fd);

    int start[2], report[2], hold[2];
    CHECK(pipe(start) == 0);
    CHECK(pipe(report) == 0);
    CHECK(pipe(hold) == 0);
    fflush(NULL);
    pid_t racers[2];
    for (int i = 0; i < 2; i++) {
        racers[i] = fork();
        if (racers[i] == 0) {
            close(start[1]);
            close(report[0]);
            close(hold[1]);
            _exit(lock_child(start[0], report[1], hold[0]));
        }
    }
    close(start[0]);
    close(report[1]);
    close(hold[0]);
    CHECK(write(start[1], "gogo", 2) == 2);
    close(start[1]);
    char reports[2] = { 0, 0 };
    for (int i = 0; i < 2; i++)
        CHECK(read(report[0], &reports[i], 1) == 1);
    close(report[0]);
    CHECK((reports[0] == 'w') != (reports[1] == 'w'));
    CHECK((reports[0] == 'l') != (reports[1] == 'l'));
    CHECK(write(hold[1], "g", 1) == 1);
    close(hold[1]);
    int winners = 0, losers = 0;
    for (int i = 0; i < 2; i++) {
        int status = 0;
        waitpid(racers[i], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) winners++;
        else losers++;
    }
    CHECK(winners == 1);
    CHECK(losers == 1);

    FILE *stale = fopen(socket_path, "w");
    CHECK(stale != NULL);
    if (stale) {
        fputs("stale\n", stale);
        fclose(stale);
    }

    xe_engine *e = xe_engine_open_vocab(model);
    fflush(NULL);
    pid_t server = fork();
    CHECK(server >= 0);
    if (server == 0) {
        _exit(serve_run(e, state_dir, cache_dir, socket_path, 0.0));
    }

    char line[8192];
    client a, b, c;
    CHECK(client_connect(&a, socket_path));
    if (a.fd < 0) {
        kill(server, SIGKILL);
        fprintf(stderr, "test_serve: server never accepted\n");
        return 1;
    }
    CHECK(client_connect(&b, socket_path));

    CHECK(client_call(&a, "{\"op\":\"describe\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(strstr(line, "\"protocol\":1") != NULL);
    CHECK(strstr(line, "\"kvstore\":true") != NULL);
    CHECK(strstr(line, "\"reasoning\":{\"efforts\":[\"low\","
                       "\"medium\",\"high\",\"max\"]") != NULL);
    CHECK(strstr(line, "minimal") == NULL);
    CHECK(strstr(line, "xhigh") == NULL);

    CHECK(client_call(&b, "{\"op\":\"append\",\"role\":\"user\","
                          "\"text\":\"no session\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":false") != NULL);

    char session_a[64], session_b[64];
    char request[512];
    CHECK(client_call(&a, "{\"op\":\"create\",\"system\":\"A\"}\n", line,
                      sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(session_of(line, session_a));
    CHECK(client_call(&a, "{\"op\":\"append\",\"role\":\"user\","
                          "\"text\":\"hello\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    /* 3.7 finding 2: the checkpoint op reports its outcome. A vocab-only
     * engine never computes KV, so the honest answer is "empty". */
    CHECK(client_call(&a, "{\"op\":\"checkpoint\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(strstr(line, "\"saved\":false") != NULL);
    CHECK(strstr(line, "\"reason\":\"empty\"") != NULL);
    snprintf(request, sizeof request,
             "{\"op\":\"open\",\"session\":\"%s\"}\n", session_a);
    CHECK(client_call(&a, request, line, sizeof line));
    CHECK(strstr(line, "\"zero_prefill\":false") != NULL);
    CHECK(strstr(line, "\"resume\":\"none\"") != NULL);

    CHECK(client_call(&b, "{\"op\":\"create\",\"system\":\"B\"}\n", line,
                      sizeof line));
    CHECK(session_of(line, session_b));

    snprintf(request, sizeof request,
             "{\"op\":\"delete\",\"session\":\"%s\"}\n", session_a);
    CHECK(client_call(&b, request, line, sizeof line));
    CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(strstr(line, "\"code\":\"busy\"") != NULL);

    CHECK(client_call(&a, request, line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(client_call(&a, "{\"op\":\"list\"}\n", line, sizeof line));
    CHECK(strstr(line, session_a) == NULL);
    CHECK(strstr(line, session_b) != NULL);

    CHECK(client_call(&a, "{\"op\":\"append\",\"role\":\"user\","
                          "\"text\":\"unbound\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":false") != NULL);

    CHECK(client_call(&a, "{\"op\":\"create\",\"system\":\"A2\"}\n", line,
                      sizeof line));
    CHECK(session_of(line, session_a));
    CHECK(client_call(&a, "{\"op\":\"generate\",\"reasoning\":true}\n",
                      line, sizeof line));
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_call(&a, "{\"op\":\"generate\",\"reasoning\":"
                          "{\"effort\":\"minimal\"}}\n",
                      line, sizeof line));
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_call(&a, "{\"op\":\"generate\",\"reasoning\":"
                          "{\"effort\":\"xhigh\"}}\n",
                      line, sizeof line));
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_call(&a, "{\"op\":\"generate\",\"reasoning\":"
                          "{\"effort\":\"low\","
                          "\"budget_tokens\":3.5}}\n",
                      line, sizeof line));
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_call(&a, "{\"op\":\"rebuild\",\"system\":\"A2\","
                          "\"messages\":[{\"role\":\"user\","
                          "\"text\":\"q\"},{\"role\":\"assistant\","
                          "\"text\":\"a\",\"reasoning\":\"r\"}]}\n",
                      line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(client_call(&a, "{\"op\":\"history\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"reasoning\":\"r\"") != NULL);

    for (int i = 0; i < 8; i++) {
        snprintf(request, sizeof request,
                 "{\"op\":\"append\",\"role\":\"user\",\"text\":\"m%d\"}\n",
                 i);
        CHECK(client_send(&a, request));
    }
    CHECK(client_send(&b, "{\"op\":\"list\"}\n"));
    CHECK(client_send(&b, "{\"op\":\"stat\",\"session\":\"x\"}\n"));
    for (int i = 0; i < 8; i++) {
        CHECK(client_recv(&a, line, sizeof line, 15000));
        CHECK(strstr(line, "\"ok\":true") != NULL);
        CHECK(strstr(line, "\"marker\":") != NULL);
    }
    CHECK(client_recv(&b, line, sizeof line, 15000));
    CHECK(strstr(line, "\"sessions\":[") != NULL);
    CHECK(client_recv(&b, line, sizeof line, 15000));
    CHECK(strstr(line, "\"ok\":false") != NULL);

    CHECK(client_call(&a, "{not json\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(strstr(line, "invalid request") != NULL);
    CHECK(client_call(&a, "{\"op\":\"describe\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);

    CHECK(client_connect(&c, socket_path));
    size_t big = 4500000;   /* over the 4 MiB frame bound (262144 x 16) */
    char *chunk = malloc(65536);
    CHECK(chunk != NULL);
    if (chunk) {
        memset(chunk, 'x', 65536);
        size_t sent = 0;
        while (sent < big) {
            ssize_t n = write(c.fd, chunk, 65536);
            if (n <= 0) break;
            sent += (size_t)n;
        }
        free(chunk);
    }
    int alive = client_recv(&c, line, sizeof line, 5000);
    if (alive) CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(client_eof(&c, 5000) == 1);
    client_close(&c);

    CHECK(client_call(&a, "{\"op\":\"describe\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);

    client u;
    CHECK(client_connect(&u, socket_path));
    CHECK(client_send(&u, "{\"op\":\"describe\"}"));
    CHECK(shutdown(u.fd, SHUT_WR) == 0);
    CHECK(client_recv(&u, line, sizeof line, 5000));
    CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_eof(&u, 5000) == 1);
    client_close(&u);

    client q;
    CHECK(client_connect(&q, socket_path));
    CHECK(shutdown(q.fd, SHUT_WR) == 0);
    CHECK(client_recv(&q, line, sizeof line, 2000) == 0);
    client_close(&q);

    client h;
    CHECK(client_connect(&h, socket_path));
    CHECK(client_send(&h, "{\"op\":\"describe\"}\n"));
    CHECK(shutdown(h.fd, SHUT_WR) == 0);
    CHECK(client_recv(&h, line, sizeof line, 5000));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(strstr(line, "\"protocol\":1") != NULL);
    CHECK(client_eof(&h, 5000) == 1);
    client_close(&h);

    client p;
    CHECK(client_connect(&p, socket_path));
    CHECK(client_send(&p, "{\"op\":\"describe\"}\n{\"op\":\"list\"}\n"
                          "{\"op\":\"desc"));
    CHECK(shutdown(p.fd, SHUT_WR) == 0);
    CHECK(client_recv(&p, line, sizeof line, 5000));
    CHECK(strstr(line, "\"protocol\":1") != NULL);
    CHECK(client_recv(&p, line, sizeof line, 5000));
    CHECK(strstr(line, "\"sessions\":[") != NULL);
    CHECK(client_recv(&p, line, sizeof line, 5000));
    CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    CHECK(client_eof(&p, 5000) == 1);
    client_close(&p);

    client w;
    CHECK(client_connect(&w, socket_path));
    CHECK(client_call(&w, "{\"op\":\"create\",\"system\":\"W\"}\n", line,
                      sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(client_send(&w, "{\"op\":\"append\",\"role\":\"user\","
                          "\"text\":\"last words\"}\n"));
    CHECK(shutdown(w.fd, SHUT_WR) == 0);
    CHECK(client_recv(&w, line, sizeof line, 15000));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(strstr(line, "\"marker\":") != NULL);
    CHECK(client_eof(&w, 5000) == 1);
    client_close(&w);

    /* 3.7 finding 3: frames are bounded by the context window, not by a
     * fixed MiB. describe advertises the bound; a transcript near the
     * window round-trips through rebuild and history in one line each. */
    {
        client big;
        CHECK(client_connect(&big, socket_path));
        CHECK(client_call(&big, "{\"op\":\"describe\"}\n", line, sizeof line));
        CHECK(strstr(line, "\"max_frame\":4194304") != NULL);
        CHECK(client_call(&big, "{\"op\":\"create\",\"system\":\"L\"}\n",
                          line, sizeof line));
        CHECK(strstr(line, "\"ok\":true") != NULL);
        char session_l[64];
        CHECK(session_of(line, session_l));

        /* 120 pairs x 2 sides x 8 paragraphs x 127 tokens = ~244k tokens,
         * ~1.0 MB of JSON: beyond the old 900 KB client guard, inside the
         * 262144-token window. */
        size_t request_length = 0;
        char *request_big = build_rebuild(120, 8, &request_length);
        CHECK(request_big != NULL);
        CHECK(request_length > 1000000);
        long long t0 = now_ms();
        CHECK(client_send(&big, request_big));
        free(request_big);
        CHECK(client_recv(&big, line, sizeof line, 120000));
        CHECK(strstr(line, "\"ok\":true") != NULL);
        long long rebuild_ms = now_ms() - t0;
        snprintf(request, sizeof request,
                 "{\"op\":\"stat\",\"session\":\"%s\"}\n", session_l);
        CHECK(client_call(&big, request, line, sizeof line));
        const char *tok = strstr(line, "\"tokens\":");
        long long tokens = tok ? atoll(tok + 9) : 0;
        CHECK(tokens > 230000 && tokens <= 262144);

        t0 = now_ms();
        CHECK(client_send(&big, "{\"op\":\"history\"}\n"));
        char *history = NULL;
        size_t history_length = 0;
        CHECK(client_recv_big(&big, &history, &history_length, 120000));
        long long history_ms = now_ms() - t0;
        if (history) {
            CHECK(history_length > 1000000);
            CHECK(strncmp(history, "{\"ok\":true", 10) == 0);
            CHECK(strstr(history, "\"entries\":[") != NULL);
            free(history);
        }
        printf("test_serve: long session %lld tokens, rebuild %zu bytes in %lld ms, "
               "history %zu bytes in %lld ms\n",
               tokens, request_length, rebuild_ms, history_length, history_ms);

        /* Transport bound alone: a 1.5 MiB line within the token budget
         * (byte-heavy, token-cheap filler) is accepted, not dropped. */
        size_t filler = 1536 * 1024;
        size_t append_capacity = filler + 128;
        char *append = malloc(append_capacity);
        CHECK(append != NULL);
        if (append) {
            size_t n = (size_t)snprintf(append, 128,
                "{\"op\":\"append\",\"role\":\"user\",\"text\":\"");
            for (size_t i = 0; i < filler; i++)
                append[n + i] = (i % 32 == 0) ? 'x' : ' ';
            n += filler;
            n += (size_t)snprintf(append + n, append_capacity - n, "\"}\n");
            (void)n;
            CHECK(client_call(&big, "{\"op\":\"create\",\"system\":\"F\"}\n",
                              line, sizeof line));
            CHECK(client_send(&big, append));
            free(append);
            CHECK(client_recv(&big, line, sizeof line, 120000));
            CHECK(strstr(line, "\"ok\":true") != NULL);
        }
        client_close(&big);
    }

    client_close(&a);
    client_close(&b);

    kill(server, SIGTERM);
    int status = 0;
    waitpid(server, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    struct stat st;
    CHECK(stat(socket_path, &st) != 0);

    lock_fd = serve_lock(NULL, holder, sizeof holder);
    CHECK(lock_fd >= 0);
    if (lock_fd >= 0) close(lock_fd);

    char idle_socket[256], fragment_socket[256], late_socket[256];
    snprintf(idle_socket, sizeof idle_socket, "%s/idle.sock", state_dir);
    snprintf(fragment_socket, sizeof fragment_socket, "%s/fragment.sock",
             state_dir);
    snprintf(late_socket, sizeof late_socket, "%s/late.sock", state_dir);

    fflush(NULL);
    pid_t idler = fork();
    CHECK(idler >= 0);
    if (idler == 0)
        _exit(serve_run(e, state_dir, cache_dir, idle_socket, 0.02));
    client k;
    CHECK(client_connect(&k, idle_socket));
    long long began = now_ms();
    int idle_code = -2;
    for (int i = 0; i < 150; i++) {
        int idle_status = 0;
        if (waitpid(idler, &idle_status, WNOHANG) == idler) {
            idle_code = WIFEXITED(idle_status) ? WEXITSTATUS(idle_status)
                                               : -1;
            break;
        }
        client_send(&k, "\n");
        test_sleep_ms(200);
    }
    long long idle_elapsed = now_ms() - began;
    CHECK(idle_code == 0);
    CHECK(idle_elapsed < 10000);
    client_close(&k);

    fflush(NULL);
    pid_t fragment_server = fork();
    CHECK(fragment_server >= 0);
    if (fragment_server == 0)
        _exit(serve_run(e, state_dir, cache_dir, fragment_socket, 0.03));
    client fragment;
    CHECK(client_connect(&fragment, fragment_socket));
    test_sleep_ms(1200);
    CHECK(client_send(&fragment, "{\"op\":\"desc"));
    test_sleep_ms(1200);
    CHECK(client_send(&fragment, "ribe\"}\n"));
    CHECK(client_recv(&fragment, line, sizeof line, 15000));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    CHECK(client_send(&fragment, "{"));
    int fragment_code = -2;
    for (int i = 0; i < 50; i++) {
        int fragment_status = 0;
        if (waitpid(fragment_server, &fragment_status, WNOHANG) ==
            fragment_server) {
            fragment_code = WIFEXITED(fragment_status)
                            ? WEXITSTATUS(fragment_status) : -1;
            break;
        }
        test_sleep_ms(200);
    }
    CHECK(fragment_code == 0);
    client_close(&fragment);

    fflush(NULL);
    pid_t late = fork();
    CHECK(late >= 0);
    if (late == 0)
        _exit(serve_run(e, state_dir, cache_dir, late_socket, 0.05));
    client g;
    CHECK(client_connect(&g, late_socket));
    long long touched = now_ms();
    client_close(&g);
    long long remaining = 2700 - (now_ms() - touched);
    if (remaining > 0) test_sleep_ms((int)remaining);
    CHECK(client_connect(&g, late_socket));
    CHECK(client_call(&g, "{\"op\":\"describe\"}\n", line, sizeof line));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    client_close(&g);
    kill(late, SIGTERM);
    int late_status = 0;
    waitpid(late, &late_status, 0);
    CHECK(WIFEXITED(late_status) && WEXITSTATUS(late_status) == 0);

    int to_child[2], from_child[2];
    CHECK(pipe(to_child) == 0);
    CHECK(pipe(from_child) == 0);
    fflush(NULL);
    pid_t stdio_pid = fork();
    CHECK(stdio_pid >= 0);
    if (stdio_pid == 0) {
        close(to_child[1]);
        close(from_child[0]);
        _exit(stdio_child(e, state_dir, cache_dir, to_child[0],
                          from_child[1]));
    }
    close(to_child[0]);
    close(from_child[1]);
    client sio;
    memset(&sio, 0, sizeof sio);
    sio.fd = from_child[0];
    const char *create = "{\"op\":\"create\",\"system\":\"S\"}\n";
    CHECK(write(to_child[1], create, strlen(create)) ==
          (ssize_t)strlen(create));
    CHECK(client_recv(&sio, line, sizeof line, 15000));
    CHECK(strstr(line, "\"ok\":true") != NULL);
    const char *partial = "{\"op\":\"describe\"";
    CHECK(write(to_child[1], partial, strlen(partial)) ==
          (ssize_t)strlen(partial));
    close(to_child[1]);
    CHECK(client_recv(&sio, line, sizeof line, 15000));
    CHECK(strstr(line, "\"ok\":false") != NULL);
    CHECK(strstr(line, "\"code\":\"invalid_request\"") != NULL);
    client_close(&sio);
    status = 0;
    waitpid(stdio_pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    xe_engine_close(e);

    if (failures) {
        fprintf(stderr, "test_serve: %d failures\n", failures);
        return 1;
    }
    printf("test_serve: all checks passed\n");
    return 0;
}
