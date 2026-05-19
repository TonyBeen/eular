#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/queue.h>

#define XXH_INLINE_ALL
#include "../3rd/xxhash.h"
#include "lsquic/lsquic.h"
#include "prog.h"
#include "test_cert.h"
#include "test_common.h"

struct server_app_ctx;

struct server_conn_ctx {
    lsquic_conn_t*         conn;
    struct server_app_ctx* app_ctx;
};

struct server_stream_ctx {
    enum phase {
        PHASE_READ_HEADER = 0,
        PHASE_READ_PAYLOAD,
        PHASE_CLOSED,
    } phase;
    lsquic_stream_t*       stream;
    struct server_app_ctx* app_ctx;
    char*                  header_buf;
    size_t                 header_len;
    size_t                 header_cap;
    uint64_t               expected_bytes;
    uint64_t               received_bytes;
    XXH3_state_t*          xxh_state;
    int                    hash_valid;
    int                    hash_finalized;
    char                   out_hash[33];
    char*                  outbox;
    size_t                 outbox_len;
    size_t                 outbox_cap;
    size_t                 outbox_off;
    int                    close_after_flush;
    int                    failed;
    int                    fin_seen;
};

struct server_app_ctx {
    struct prog* prog;
    int          quiet;
};

static int append_bytes(char** buf, size_t* len, size_t* cap, const void* data, size_t data_len)
{
    char*  nbuf;
    size_t new_cap;

    if (*len + data_len <= *cap) {
        memcpy(*buf + *len, data, data_len);
        *len += data_len;
        return 0;
    }

    new_cap = *cap ? *cap : 256;
    while (new_cap < *len + data_len) new_cap *= 2;

    nbuf = realloc(*buf, new_cap);
    if (!nbuf) return -1;

    *buf = nbuf;
    *cap = new_cap;
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
}

static int queue_line(struct server_stream_ctx* st, const char* line)
{
    return append_bytes(&st->outbox, &st->outbox_len, &st->outbox_cap, line, strlen(line));
}

static int parse_upload_header(const char* line, uint64_t* expected_bytes)
{
    const char* prefix = "UPLOAD ";
    char*       endptr;

    if (0 != strncmp(line, prefix, strlen(prefix))) return 0;

    *expected_bytes = strtoull(line + strlen(prefix), &endptr, 10);
    return endptr && *endptr == '\0' && *expected_bytes > 0;
}

static int finalize_hash(struct server_stream_ctx* st)
{
    XXH128_hash_t      digest;
    XXH128_canonical_t canonical;
    static const char  hex[] = "0123456789abcdef";
    size_t             i;

    if (st->hash_finalized) return 0;
    if (!st->hash_valid) return -1;

    digest = XXH3_128bits_digest(st->xxh_state);
    XXH128_canonicalFromHash(&canonical, digest);
    for (i = 0; i < sizeof(canonical.digest); ++i) {
        st->out_hash[i * 2 + 0] = hex[canonical.digest[i] >> 4];
        st->out_hash[i * 2 + 1] = hex[canonical.digest[i] & 0x0F];
    }
    st->out_hash[32] = '\0';
    st->hash_finalized = 1;
    return 0;
}

static void mark_failed(struct server_stream_ctx* st, const char* reason)
{
    char line[256];

    if (st->failed) return;
    st->failed = 1;
    if (!st->app_ctx->quiet) fprintf(stderr, "[server] stream failed: %s\n", reason);
    snprintf(line, sizeof(line), "ERR %s\n", reason);
    (void)queue_line(st, line);
    st->close_after_flush = 1;
}

static void maybe_finalize_done(struct server_stream_ctx* st)
{
    char line[256];

    if (st->failed) return;

    if (st->phase != PHASE_READ_PAYLOAD) {
        mark_failed(st, "missing_upload_header");
        return;
    }

    if (st->received_bytes != st->expected_bytes) {
        mark_failed(st, "size_mismatch");
        return;
    }

    if (0 != finalize_hash(st)) {
        mark_failed(st, "xxh128_finalize_failed");
        return;
    }

    snprintf(line, sizeof(line), "DONE bytes=%llu xxh128=%s\n", (unsigned long long)st->received_bytes, st->out_hash);
    if (0 != queue_line(st, line)) {
        mark_failed(st, "oom");
        return;
    }
    st->close_after_flush = 1;
}

static lsquic_conn_ctx_t* on_new_conn(void* stream_if_ctx, lsquic_conn_t* conn)
{
    struct server_app_ctx*  ctx = stream_if_ctx;
    struct server_conn_ctx* conn_h = calloc(1, sizeof(*conn_h));

    if (!conn_h) return NULL;

    conn_h->conn = conn;
    conn_h->app_ctx = ctx;
    if (!ctx->quiet) fprintf(stderr, "[server] new connection\n");
    return (lsquic_conn_ctx_t*)conn_h;
}

static void on_conn_closed(lsquic_conn_t* conn)
{
    struct server_conn_ctx* conn_h = (struct server_conn_ctx*)lsquic_conn_get_ctx(conn);
    lsquic_conn_set_ctx(conn, NULL);
    free(conn_h);
}

static lsquic_stream_ctx_t* on_new_stream(void* stream_if_ctx, lsquic_stream_t* stream)
{
    struct server_app_ctx*    ctx = stream_if_ctx;
    struct server_stream_ctx* st = calloc(1, sizeof(*st));

    if (!st) return NULL;

    st->stream = stream;
    st->app_ctx = ctx;
    st->phase = PHASE_READ_HEADER;
    st->xxh_state = XXH3_createState();
    if (!st->xxh_state || XXH3_128bits_reset(st->xxh_state) != XXH_OK) {
        if (st->xxh_state) XXH3_freeState(st->xxh_state);
        free(st);
        return NULL;
    }
    st->hash_valid = 1;
    lsquic_stream_wantread(stream, 1);
    return (lsquic_stream_ctx_t*)st;
}

static void process_payload_chunk(struct server_stream_ctx* st, const unsigned char* data, size_t len)
{
    char     line[128];
    uint64_t remain;

    if (st->phase != PHASE_READ_PAYLOAD) {
        mark_failed(st, "bad_state");
        return;
    }

    if (st->received_bytes >= st->expected_bytes) {
        mark_failed(st, "payload_overflow");
        return;
    }

    remain = st->expected_bytes - st->received_bytes;
    if (len > remain) {
        mark_failed(st, "payload_overflow");
        return;
    }

    if (XXH3_128bits_update(st->xxh_state, data, len) != XXH_OK) {
        mark_failed(st, "xxh128_update_failed");
        return;
    }

    st->received_bytes += len;
    snprintf(line, sizeof(line), "ACK total=%llu\n", (unsigned long long)st->received_bytes);
    if (0 != queue_line(st, line)) mark_failed(st, "oom");
}

static int consume_read_buf(struct server_stream_ctx* st, const unsigned char* data, size_t len)
{
    const unsigned char* chunk = data;
    size_t               left = len;
    const unsigned char* lf;
    uint64_t             expected;
    size_t               head_len;
    char*                line;

    while (left > 0 && !st->failed) {
        if (st->phase == PHASE_READ_HEADER) {
            lf = memchr(chunk, '\n', left);
            if (!lf) {
                if (0 != append_bytes(&st->header_buf, &st->header_len, &st->header_cap, chunk, left)) return -1;
                return 0;
            }

            head_len = (size_t)(lf - chunk);
            if (0 != append_bytes(&st->header_buf, &st->header_len, &st->header_cap, chunk, head_len)) return -1;

            if (st->header_len > 0 && st->header_buf[st->header_len - 1] == '\r') --st->header_len;
            if (0 != append_bytes(&st->header_buf, &st->header_len, &st->header_cap, "", 1)) return -1;
            line = st->header_buf;
            line[st->header_len - 1] = '\0';

            if (!parse_upload_header(line, &expected)) {
                mark_failed(st, "bad_upload_header");
                return 0;
            }

            st->expected_bytes = expected;
            st->phase = PHASE_READ_PAYLOAD;
            st->header_len = 0;

            chunk = lf + 1;
            left -= head_len + 1;
            continue;
        }

        process_payload_chunk(st, chunk, left);
        left = 0;
    }

    return st->failed ? -1 : 0;
}

static void on_read(lsquic_stream_t* stream, lsquic_stream_ctx_t* st_h)
{
    struct server_stream_ctx* st = (struct server_stream_ctx*)st_h;
    unsigned char             buf[64 * 1024];
    ssize_t                   nr;

    while ((nr = lsquic_stream_read(stream, buf, sizeof(buf))) > 0) {
        if (0 != consume_read_buf(st, buf, (size_t)nr)) {
            if (!st->failed) mark_failed(st, "oom");
            break;
        }
    }

    if (nr == 0) {
        st->fin_seen = 1;
        maybe_finalize_done(st);
    } else if (nr < 0 && errno != EWOULDBLOCK)
        mark_failed(st, "read-error");

    if (st->outbox_len > st->outbox_off) {
        lsquic_stream_wantwrite(stream, 1);
        lsquic_stream_wantread(stream, 0);
    }
}

static void on_write(lsquic_stream_t* stream, lsquic_stream_ctx_t* st_h)
{
    struct server_stream_ctx* st = (struct server_stream_ctx*)st_h;
    ssize_t                   nw;

    while (st->outbox_off < st->outbox_len) {
        nw = lsquic_stream_write(stream, st->outbox + st->outbox_off, st->outbox_len - st->outbox_off);
        if (nw > 0)
            st->outbox_off += (size_t)nw;
        else if (nw == 0 || errno == EWOULDBLOCK)
            break;
        else {
            lsquic_conn_close(lsquic_stream_conn(stream));
            return;
        }
    }

    lsquic_stream_flush(stream);
    if (st->outbox_off == st->outbox_len) {
        st->outbox_len = 0;
        st->outbox_off = 0;
        lsquic_stream_wantwrite(stream, 0);
        if (st->close_after_flush) {
            st->phase = PHASE_CLOSED;
            lsquic_stream_shutdown(stream, 1);
        } else if (!st->fin_seen)
            lsquic_stream_wantread(stream, 1);
    }
}

static void on_close(lsquic_stream_t* stream, lsquic_stream_ctx_t* st_h)
{
    struct server_stream_ctx* st = (struct server_stream_ctx*)st_h;
    (void)stream;

    if (st) {
        if (st->xxh_state) XXH3_freeState(st->xxh_state);
        free(st->header_buf);
        free(st->outbox);
        free(st);
    }
}

static const struct lsquic_stream_if server_stream_if = {
    .on_new_conn = on_new_conn,
    .on_conn_closed = on_conn_closed,
    .on_new_stream = on_new_stream,
    .on_read = on_read,
    .on_write = on_write,
    .on_close = on_close,
};

static void usage(const char* prog)
{
    fprintf(stderr, "Usage: %s [--bind-ip IP] [--bind-port PORT] [--silent]\n", prog);
}

int main(int argc, char** argv)
{
    const char*           bind_ip = "127.0.0.1";
    unsigned short        bind_port = 4433;
    int                   quiet = 0;
    int                   i;
    char                  sport[128];
    struct sport_head     sports;
    struct prog           prog;
    struct server_app_ctx server_ctx;

    memset(&server_ctx, 0, sizeof(server_ctx));

    for (i = 1; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--bind-ip") && i + 1 < argc)
            bind_ip = argv[++i];
        else if (0 == strcmp(argv[i], "--bind-port") && i + 1 < argc)
            bind_port = (unsigned short)atoi(argv[++i]);
        else if (0 == strcmp(argv[i], "--silent") || 0 == strcmp(argv[i], "--quiet"))
            quiet = 1;
        else {
            usage(argv[0]);
            return 1;
        }
    }

    TAILQ_INIT(&sports);
    prog_init(&prog, LSENG_SERVER, &sports, &server_stream_if, &server_ctx);
    server_ctx.prog = &prog;
    server_ctx.quiet = quiet;
    prog.prog_settings.es_dplpmtud = 0;
    prog.prog_settings.es_base_plpmtu = 1400;
    prog.prog_settings.es_max_plpmtu = 1400;

    if (quiet) (void)prog_set_opt(&prog, 'L', "emerg");

    if (add_alpn("echo") != 0) return 1;
    prog.prog_certs = init_embedded_cert("localhost");
    if (!prog.prog_certs) return 1;

    snprintf(sport, sizeof(sport), "%s:%hu", bind_ip, bind_port);
    if (0 != prog_set_opt(&prog, 's', sport)) return 1;

    if (0 != prog_prep(&prog)) return 1;

    if (!quiet) fprintf(stderr, "[server] listening on %s:%hu\n", bind_ip, bind_port);
    prog_run(&prog);
    prog_cleanup(&prog);
    return 0;
}
