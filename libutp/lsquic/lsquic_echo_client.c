#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <time.h>

#define XXH_INLINE_ALL
#include "../3rd/xxhash.h"

#include "lsquic/lsquic.h"
#include "test_common.h"
#include "prog.h"

struct client_app_ctx;

struct client_conn_ctx
{
    lsquic_conn_t            *conn;
    struct client_app_ctx    *app_ctx;
};

struct client_stream_ctx
{
    lsquic_stream_t          *stream;
    struct client_app_ctx    *app_ctx;
    unsigned char            *chunk;
    size_t                    chunk_bytes;
    size_t                    total_bytes;
    size_t                    sent_bytes;
    size_t                    send_off;
    size_t                    header_off;
    char                     *upload_header;
    size_t                    upload_header_len;
    char                     *recv_buf;
    size_t                    recv_buf_len;
    size_t                    recv_buf_cap;
    size_t                    recv_parse_off;
    int                       send_done;
    int                       close_issued;
};

struct client_app_ctx
{
    struct prog              *prog;
    struct client_stream_ctx *stream_ctx;
    size_t                    total_bytes;
    size_t                    chunk_bytes;
    size_t                    sent_bytes;
    size_t                    done_bytes;
    char                      local_hash[33];
    char                      server_hash[33];
    unsigned                  ack_count;
    int                       quiet;
    int                       done_received;
    int                       connect_failed;
    int                       final_printed;
    int                       result;
    unsigned long long         start_ms;
    unsigned long long         connected_ms;
    unsigned long long         upload_done_ms;
    unsigned long long         done_ms;
    XXH3_state_t             *xxh_state;
    int                       hash_valid;
    int                       hash_finalized;
};

static unsigned long long
now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long long) ts.tv_sec * 1000ull
         + (unsigned long long) ts.tv_nsec / 1000000ull;
}

static int
finalize_local_hash(struct client_app_ctx *ctx)
{
    XXH128_hash_t digest;
    XXH128_canonical_t canonical;
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (ctx->hash_finalized)
        return 0;
    if (!ctx->hash_valid)
        return -1;

    digest = XXH3_128bits_digest(ctx->xxh_state);
    XXH128_canonicalFromHash(&canonical, digest);
    for (i = 0; i < sizeof(canonical.digest); ++i)
    {
        ctx->local_hash[i * 2 + 0] = hex[canonical.digest[i] >> 4];
        ctx->local_hash[i * 2 + 1] = hex[canonical.digest[i] & 0x0F];
    }
    ctx->local_hash[32] = '\0';
    ctx->hash_finalized = 1;
    return 0;
}

static void
print_final_result(struct client_app_ctx *ctx, const char *reason)
{
    int pass;

    if (ctx->final_printed)
        return;

    ctx->final_printed = 1;
    if (!ctx->hash_finalized)
        (void) finalize_local_hash(ctx);

    pass = ctx->done_received
        && !ctx->connect_failed
        && ctx->sent_bytes == ctx->total_bytes
        && ctx->done_bytes == ctx->total_bytes
        && 0 == strcmp(ctx->local_hash, ctx->server_hash);
    ctx->result = pass ? 0 : 1;

    fprintf(stderr,
        "[client] result=%s reason=%s sent_bytes=%zu done_bytes=%zu "
        "connect_ms=%llu upload_ms=%llu done_ms=%llu total_ms=%llu "
        "local_xxh128=%s server_xxh128=%s ack_count=%u done_received=%d\n",
        pass ? "PASS" : "FAIL",
        reason,
        ctx->sent_bytes,
        ctx->done_bytes,
        ctx->connected_ms ? ctx->connected_ms - ctx->start_ms : 0,
        ctx->upload_done_ms ? ctx->upload_done_ms - ctx->start_ms : 0,
        ctx->done_ms ? ctx->done_ms - ctx->start_ms : 0,
        now_ms() - ctx->start_ms,
        ctx->local_hash[0] ? ctx->local_hash : "<pending>",
        ctx->server_hash[0] ? ctx->server_hash : "<pending>",
        ctx->ack_count,
        ctx->done_received);
}

static void
client_finish(struct client_stream_ctx *st, const char *reason)
{
    if (!st->close_issued)
    {
        st->close_issued = 1;
        print_final_result(st->app_ctx, reason);
        lsquic_conn_close(lsquic_stream_conn(st->stream));
    }
}

static lsquic_conn_ctx_t *
on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn)
{
    struct client_app_ctx *ctx = stream_if_ctx;
    struct client_conn_ctx *conn_h = calloc(1, sizeof(*conn_h));

    if (!conn_h)
        return NULL;

    conn_h->conn = conn;
    conn_h->app_ctx = ctx;
    if (!ctx->quiet)
        fprintf(stderr, "[client] new connection\n");
    return (lsquic_conn_ctx_t *) conn_h;
}

static void
on_hsk_done(lsquic_conn_t *conn, enum lsquic_hsk_status status)
{
    struct client_conn_ctx *conn_h = (struct client_conn_ctx *) lsquic_conn_get_ctx(conn);

    if (!conn_h || !conn_h->app_ctx)
        return;

    if (status != LSQ_HSK_OK)
    {
        conn_h->app_ctx->connect_failed = 1;
        print_final_result(conn_h->app_ctx, "handshake-failed");
        lsquic_conn_close(conn);
        return;
    }

    lsquic_conn_make_stream(conn);
}

static void
on_conn_closed(lsquic_conn_t *conn)
{
    struct client_conn_ctx *conn_h = (struct client_conn_ctx *) lsquic_conn_get_ctx(conn);
    struct client_app_ctx *ctx = conn_h ? conn_h->app_ctx : NULL;

    if (ctx)
    {
        if (!ctx->final_printed)
            print_final_result(ctx, ctx->connect_failed ? "connect-error" : "loop-exit");
        prog_stop(ctx->prog);
    }

    lsquic_conn_set_ctx(conn, NULL);
    free(conn_h);
}

static lsquic_stream_ctx_t *
on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream)
{
    struct client_app_ctx *ctx = stream_if_ctx;
    struct client_stream_ctx *st;
    size_t i;
    char header[128];
    int n;

    st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;

    st->stream = stream;
    st->app_ctx = ctx;
    st->total_bytes = ctx->total_bytes;
    st->chunk_bytes = ctx->chunk_bytes;
    st->chunk = malloc(st->chunk_bytes);
    if (!st->chunk)
    {
        free(st);
        return NULL;
    }

    for (i = 0; i < st->chunk_bytes; ++i)
        st->chunk[i] = (unsigned char) (i & 0xFF);

    n = snprintf(header, sizeof(header), "UPLOAD %zu\n", st->total_bytes);
    if (n <= 0 || (size_t) n >= sizeof(header))
    {
        free(st->chunk);
        free(st);
        return NULL;
    }
    st->upload_header_len = (size_t) n;
    st->upload_header = malloc(st->upload_header_len);
    if (!st->upload_header)
    {
        free(st->chunk);
        free(st);
        return NULL;
    }
    memcpy(st->upload_header, header, st->upload_header_len);

    ctx->stream_ctx = st;
    ctx->connected_ms = now_ms();
    if (!ctx->quiet)
        fprintf(stderr, "[client] new stream\n");
    lsquic_stream_wantread(stream, 1);
    lsquic_stream_wantwrite(stream, 1);
    return (lsquic_stream_ctx_t *) st;
}

static void
handle_server_line(struct client_stream_ctx *st, const char *line)
{
    struct client_app_ctx *ctx = st->app_ctx;
    const char *hash_pos;
    char *endptr;

    if (0 == strncmp(line, "ACK total=", 10))
    {
        ++ctx->ack_count;
        return;
    }

    if (0 == strncmp(line, "DONE bytes=", 11))
    {
        hash_pos = strstr(line, " xxh128=");
        if (!hash_pos)
        {
            ctx->connect_failed = 1;
            client_finish(st, "bad-done");
            return;
        }

        ctx->done_bytes = strtoull(line + 11, &endptr, 10);
        if (endptr != hash_pos)
        {
            ctx->connect_failed = 1;
            client_finish(st, "bad-done");
            return;
        }

        snprintf(ctx->server_hash, sizeof(ctx->server_hash), "%s", hash_pos + 8);
        ctx->done_received = 1;
        ctx->done_ms = now_ms();
        if (0 != finalize_local_hash(ctx))
            ctx->connect_failed = 1;
        client_finish(st, (ctx->done_bytes == ctx->sent_bytes
                        && 0 == strcmp(ctx->local_hash, ctx->server_hash))
                        ? "done" : "mismatch");
        return;
    }

    if (0 == strncmp(line, "ERR ", 4))
    {
        ctx->connect_failed = 1;
        client_finish(st, "server-error");
        return;
    }
}

static int
append_recv_buf(struct client_stream_ctx *st, const unsigned char *data, size_t len)
{
    char *nbuf;
    size_t new_cap;

    if (len == 0)
        return 0;

    if (st->recv_buf_len + len <= st->recv_buf_cap)
    {
        memcpy(st->recv_buf + st->recv_buf_len, data, len);
        st->recv_buf_len += len;
        return 0;
    }

    new_cap = st->recv_buf_cap ? st->recv_buf_cap : 4096;
    while (new_cap < st->recv_buf_len + len)
        new_cap *= 2;

    nbuf = realloc(st->recv_buf, new_cap);
    if (!nbuf)
        return -1;

    st->recv_buf = nbuf;
    st->recv_buf_cap = new_cap;
    memcpy(st->recv_buf + st->recv_buf_len, data, len);
    st->recv_buf_len += len;
    return 0;
}

static void
compact_recv_buf(struct client_stream_ctx *st)
{
    if (st->recv_parse_off == 0)
        return;
    if (st->recv_parse_off >= st->recv_buf_len)
    {
        st->recv_buf_len = 0;
        st->recv_parse_off = 0;
        return;
    }

    memmove(st->recv_buf, st->recv_buf + st->recv_parse_off,
            st->recv_buf_len - st->recv_parse_off);
    st->recv_buf_len -= st->recv_parse_off;
    st->recv_parse_off = 0;
}

static void
on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    struct client_stream_ctx *st = (struct client_stream_ctx *) st_h;
    unsigned char buf[4096];
    ssize_t nr;
    char *nl;

    while ((nr = lsquic_stream_read(stream, buf, sizeof(buf))) > 0)
    {
        if (0 != append_recv_buf(st, buf, (size_t) nr))
        {
            st->app_ctx->connect_failed = 1;
            client_finish(st, "oom");
            return;
        }

        for (;;)
        {
            nl = memchr(st->recv_buf + st->recv_parse_off, '\n',
                        st->recv_buf_len - st->recv_parse_off);
            if (!nl)
                break;
            *nl = '\0';
            if (nl > st->recv_buf + st->recv_parse_off && nl[-1] == '\r')
                nl[-1] = '\0';
            handle_server_line(st, st->recv_buf + st->recv_parse_off);
            st->recv_parse_off = (size_t) (nl - st->recv_buf) + 1;
            if (st->close_issued)
                return;
        }
        compact_recv_buf(st);
    }

    if (nr == 0)
    {
        if (!st->app_ctx->done_received)
        {
            st->app_ctx->connect_failed = 1;
            client_finish(st, "server-closed-before-done");
        }
    }
    else if (nr < 0 && errno != EWOULDBLOCK)
    {
        st->app_ctx->connect_failed = 1;
        client_finish(st, "read-error");
    }
}

static void
on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    struct client_stream_ctx *st = (struct client_stream_ctx *) st_h;
    struct client_app_ctx *ctx = st->app_ctx;
    size_t remaining;
    size_t tosend;
    ssize_t nw;

    while (st->header_off < st->upload_header_len)
    {
        nw = lsquic_stream_write(stream, st->upload_header + st->header_off,
                                 st->upload_header_len - st->header_off);
        if (nw > 0)
            st->header_off += (size_t) nw;
        else if (nw == 0 || errno == EWOULDBLOCK)
            return;
        else
        {
            ctx->connect_failed = 1;
            client_finish(st, "write-header-error");
            return;
        }
    }

    while (st->sent_bytes < st->total_bytes)
    {
        remaining = st->total_bytes - st->sent_bytes;
        tosend = st->chunk_bytes - st->send_off;
        if (tosend > remaining)
            tosend = remaining;

        nw = lsquic_stream_write(stream, st->chunk + st->send_off, tosend);
        if (nw > 0)
        {
            if (XXH3_128bits_update(ctx->xxh_state, st->chunk + st->send_off, (size_t) nw) != XXH_OK)
            {
                ctx->connect_failed = 1;
                client_finish(st, "xxh128-update-failed");
                return;
            }
            st->sent_bytes += (size_t) nw;
            ctx->sent_bytes = st->sent_bytes;
            st->send_off += (size_t) nw;
            if (st->send_off == st->chunk_bytes)
                st->send_off = 0;
            if ((size_t) nw < tosend)
                break;
        }
        else if (nw == 0 || errno == EWOULDBLOCK)
            break;
        else
        {
            ctx->connect_failed = 1;
            client_finish(st, "write-payload-error");
            return;
        }
    }

    lsquic_stream_flush(stream);
    if (st->sent_bytes == st->total_bytes && !st->send_done)
    {
        st->send_done = 1;
        ctx->upload_done_ms = now_ms();
        if (0 != finalize_local_hash(ctx))
        {
            ctx->connect_failed = 1;
            client_finish(st, "xxh128-finalize-failed");
            return;
        }
        if (!ctx->quiet)
            fprintf(stderr, "[client] upload finished, waiting DONE, sent_bytes=%zu xxh128=%s\n",
                    ctx->sent_bytes, ctx->local_hash);
        lsquic_stream_shutdown(stream, 1);
        lsquic_stream_wantwrite(stream, 0);
        lsquic_stream_wantread(stream, 1);
    }
}

static void
on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *st_h)
{
    struct client_stream_ctx *st = (struct client_stream_ctx *) st_h;
    (void) stream;

    if (st)
    {
        if (st->app_ctx)
            st->app_ctx->stream_ctx = NULL;
        free(st->chunk);
        free(st->upload_header);
        free(st->recv_buf);
        free(st);
    }
}

static const struct lsquic_stream_if client_stream_if = {
    .on_new_conn    = on_new_conn,
    .on_conn_closed = on_conn_closed,
    .on_new_stream  = on_new_stream,
    .on_read        = on_read,
    .on_write       = on_write,
    .on_close       = on_close,
    .on_hsk_done    = on_hsk_done,
};

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--server-ip IP] [--server-port PORT] [--total-bytes BYTES] [--chunk-bytes BYTES] [--count N] [--length BYTES] [--silent]\n",
        prog);
}

int
main(int argc, char **argv)
{
    const char *server_ip = "127.0.0.1";
    unsigned short server_port = 4433;
    size_t total_bytes = 64 * 1024;
    size_t chunk_bytes = 1024;
    size_t count = 0;
    int quiet = 0;
    int i;
    char sport[128];
    struct sport_head sports;
    struct prog prog;
    struct client_app_ctx client_ctx;

    memset(&client_ctx, 0, sizeof(client_ctx));

    for (i = 1; i < argc; ++i)
    {
        if (0 == strcmp(argv[i], "--server-ip") && i + 1 < argc)
            server_ip = argv[++i];
        else if (0 == strcmp(argv[i], "--server-port") && i + 1 < argc)
            server_port = (unsigned short) atoi(argv[++i]);
        else if (0 == strcmp(argv[i], "--total-bytes") && i + 1 < argc)
            total_bytes = (size_t) strtoull(argv[++i], NULL, 10);
        else if (0 == strcmp(argv[i], "--chunk-bytes") && i + 1 < argc)
            chunk_bytes = (size_t) strtoull(argv[++i], NULL, 10);
        else if (0 == strcmp(argv[i], "--count") && i + 1 < argc)
            count = (size_t) strtoull(argv[++i], NULL, 10);
        else if (0 == strcmp(argv[i], "--length") && i + 1 < argc)
            chunk_bytes = (size_t) strtoull(argv[++i], NULL, 10);
        else if (0 == strcmp(argv[i], "--bind-ip") && i + 1 < argc)
            ++i;
        else if (0 == strcmp(argv[i], "--bind-port") && i + 1 < argc)
            ++i;
        else if (0 == strcmp(argv[i], "--silent") || 0 == strcmp(argv[i], "--quiet"))
            quiet = 1;
        else
        {
            usage(argv[0]);
            return 1;
        }
    }

    if (count)
        total_bytes = count * chunk_bytes;

    client_ctx.total_bytes = total_bytes;
    client_ctx.chunk_bytes = chunk_bytes;
    client_ctx.quiet = quiet;
    client_ctx.start_ms = now_ms();
    client_ctx.xxh_state = XXH3_createState();
    if (!client_ctx.xxh_state || XXH3_128bits_reset(client_ctx.xxh_state) != XXH_OK)
    {
        fprintf(stderr, "[client] failed to initialize xxh128\n");
        return 1;
    }
    client_ctx.hash_valid = 1;

    TAILQ_INIT(&sports);
    prog_init(&prog, 0, &sports, &client_stream_if, &client_ctx);
    client_ctx.prog = &prog;
    prog.prog_api.ea_alpn = "echo";
    prog.prog_settings.es_dplpmtud = 0;
    prog.prog_settings.es_base_plpmtu = 1400;
    prog.prog_settings.es_max_plpmtu = 1400;

    if (quiet)
        (void) prog_set_opt(&prog, 'L', "emerg");

    snprintf(sport, sizeof(sport), "%s:%hu", server_ip, server_port);
    if (0 != prog_set_opt(&prog, 's', sport))
        return 1;
    if (0 != prog_set_opt(&prog, 'H', "localhost"))
        return 1;

    if (0 != prog_prep(&prog))
        return 1;
    if (0 != prog_connect(&prog, NULL, 0))
        return 1;

    if (!quiet)
        fprintf(stderr,
            "[client] connecting to %s:%hu, total_bytes=%zu, chunk_bytes=%zu\n",
            server_ip, server_port, total_bytes, chunk_bytes);

    prog_run(&prog);
    prog_cleanup(&prog);
    if (client_ctx.xxh_state)
        XXH3_freeState(client_ctx.xxh_state);
    return client_ctx.result;
}
