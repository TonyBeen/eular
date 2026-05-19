#include "prog.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

static int s_global_inited;

static int
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static socklen_t
sockaddr_len(const struct sockaddr *sa)
{
    if (!sa)
        return 0;
    switch (sa->sa_family)
    {
        case AF_INET:
            return sizeof(struct sockaddr_in);
        case AF_INET6:
            return sizeof(struct sockaddr_in6);
        default:
            return sizeof(struct sockaddr_storage);
    }
}

static int
parse_endpoint(const char *spec, struct sockaddr_storage *sa, socklen_t *salen)
{
    char host[256];
    char port[32];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int rc;

    if (!colon)
        return -1;

    size_t host_len = (size_t) (colon - spec);
    if (host_len >= sizeof(host))
        return -1;
    memcpy(host, spec, host_len);
    host[host_len] = '\0';
    snprintf(port, sizeof(port), "%s", colon + 1);

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res)
        return -1;

    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *salen = (socklen_t) res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int
ensure_socket(struct prog *prog, const struct sockaddr_storage *bind_sa, socklen_t bind_len)
{
    int yes = 1;

    prog->sockfd = socket(bind_sa->ss_family, SOCK_DGRAM, 0);
    if (prog->sockfd < 0)
        return -1;
    if (setsockopt(prog->sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0)
        return -1;
    if (bind(prog->sockfd, (const struct sockaddr *) bind_sa, bind_len) != 0)
        return -1;
    if (set_nonblocking(prog->sockfd) != 0)
        return -1;
    return 0;
}

static int
packets_out(void *ctx, const struct lsquic_out_spec *out_spec, unsigned n_packets_out)
{
    struct prog *prog = ctx;
    unsigned i;

    for (i = 0; i < n_packets_out; ++i)
    {
        struct msghdr msg;
        ssize_t nw;

        memset(&msg, 0, sizeof(msg));
        msg.msg_name = (void *) out_spec[i].dest_sa;
        msg.msg_namelen = sockaddr_len(out_spec[i].dest_sa);
        msg.msg_iov = out_spec[i].iov;
        msg.msg_iovlen = out_spec[i].iovlen;

        nw = sendmsg(prog->sockfd, &msg, 0);
        if (nw < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                errno = EAGAIN;
            }
            return (int) i > 0 ? (int) i : -1;
        }
    }

    return (int) n_packets_out;
}

static struct ssl_ctx_st *
get_ssl_ctx(void *peer_ctx, const struct sockaddr *local)
{
    struct prog *prog = peer_ctx;
    (void) local;
    return prog ? prog->prog_certs : NULL;
}

void
prog_init(struct prog *prog, int flags, struct sport_head *sports,
          const struct lsquic_stream_if *stream_if, void *stream_if_ctx)
{
    (void) sports;
    memset(prog, 0, sizeof(*prog));
    prog->prog_flags = flags;
    prog->sockfd = -1;
    prog->prog_stream_if = stream_if;
    prog->prog_stream_if_ctx = stream_if_ctx;
    lsquic_engine_init_settings(&prog->prog_settings, (unsigned) flags);
    prog->prog_settings.es_versions = LSQUIC_SUPPORTED_VERSIONS;
    prog->prog_settings.es_dplpmtud = 0;
    prog->prog_settings.es_base_plpmtu = 1400;
    prog->prog_settings.es_max_plpmtu = 1400;
    prog->prog_settings.es_progress_check = 1;
    prog->prog_settings.es_rw_once = 0;
    prog->prog_api.ea_settings = &prog->prog_settings;
    prog->prog_api.ea_stream_if = stream_if;
    prog->prog_api.ea_stream_if_ctx = stream_if_ctx;
    prog->prog_api.ea_packets_out = packets_out;
    prog->prog_api.ea_packets_out_ctx = prog;
    prog->prog_api.ea_alpn = "echo";
    if (flags & LSENG_SERVER)
        prog->prog_api.ea_get_ssl_ctx = get_ssl_ctx;
    if (!s_global_inited)
    {
        if (lsquic_global_init((flags & LSENG_SERVER) ? LSQUIC_GLOBAL_SERVER : LSQUIC_GLOBAL_CLIENT) == 0)
            s_global_inited = 1;
    }
    prog->running = 1;
}

int
prog_set_opt(struct prog *prog, char opt, const char *value)
{
    char **dst = NULL;

    switch (opt)
    {
        case 's':
            dst = (prog->prog_flags & LSENG_SERVER) ? &prog->bind_spec : &prog->peer_spec;
            break;
        case 'H':
            dst = &prog->hostname;
            break;
        case 'L':
            return 0;
        default:
            return -1;
    }

    free(*dst);
    *dst = value ? strdup(value) : NULL;
    return *dst || !value ? 0 : -1;
}

static int
prep_bind_sa(struct prog *prog, struct sockaddr_storage *sa, socklen_t *salen)
{
    const char *spec = (prog->prog_flags & LSENG_SERVER) ? prog->bind_spec : NULL;
    char fallback[64];

    if (!spec)
    {
        snprintf(fallback, sizeof(fallback), "0.0.0.0:0");
        spec = fallback;
    }
    return parse_endpoint(spec, sa, salen);
}

int
prog_prep(struct prog *prog)
{
    struct sockaddr_storage bind_sa;
    socklen_t bind_len = 0;

    if (prep_bind_sa(prog, &bind_sa, &bind_len) != 0)
        return -1;

    if (ensure_socket(prog, &bind_sa, bind_len) != 0)
        return -1;

    prog->local_sa_len = sizeof(prog->local_sa);
    if (getsockname(prog->sockfd, (struct sockaddr *) &prog->local_sa, &prog->local_sa_len) != 0)
        return -1;

    prog->engine = lsquic_engine_new((unsigned) prog->prog_flags, &prog->prog_api);
    return prog->engine ? 0 : -1;
}

int
prog_connect(struct prog *prog, const struct sockaddr *local_sa, unsigned short base_plpmtu)
{
    const struct sockaddr *l_sa = local_sa ? local_sa : (const struct sockaddr *) &prog->local_sa;
    socklen_t l_len = local_sa ? sockaddr_len(local_sa) : prog->local_sa_len;

    if (!prog->peer_spec || !prog->engine)
        return -1;
    if (parse_endpoint(prog->peer_spec, &prog->peer_sa, &prog->peer_sa_len) != 0)
        return -1;
    prog->has_peer_sa = 1;

    if (!lsquic_engine_connect(prog->engine, N_LSQVER, l_sa, (const struct sockaddr *) &prog->peer_sa, prog,
                               NULL, prog->hostname ? prog->hostname : "localhost",
                               base_plpmtu ? base_plpmtu : prog->prog_settings.es_base_plpmtu,
                               NULL, 0, NULL, 0))
        return -1;
    (void) l_len;
    return 0;
}

static void
pump_engine(struct prog *prog)
{
    if (!prog->engine)
        return;
    lsquic_engine_process_conns(prog->engine);
    while (lsquic_engine_has_unsent_packets(prog->engine))
        lsquic_engine_send_unsent_packets(prog->engine);
}

void
prog_run(struct prog *prog)
{
    pump_engine(prog);
    while (prog->running)
    {
        fd_set readfds;
        struct timeval tv;
        int rv;

        FD_ZERO(&readfds);
        FD_SET(prog->sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        rv = select(prog->sockfd + 1, &readfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(prog->sockfd, &readfds))
        {
            for (;;)
            {
                unsigned char buf[2048];
                struct sockaddr_storage peer_sa;
                socklen_t peer_len = sizeof(peer_sa);
                ssize_t nr;

                nr = recvfrom(prog->sockfd, buf, sizeof(buf), 0, (struct sockaddr *) &peer_sa, &peer_len);
                if (nr < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    break;
                }

                lsquic_engine_packet_in(prog->engine, buf, (size_t) nr,
                                        (const struct sockaddr *) &prog->local_sa,
                                        (const struct sockaddr *) &peer_sa, prog, 0);
            }
        }

        pump_engine(prog);
    }
}

void
prog_stop(struct prog *prog)
{
    prog->running = 0;
}

void
prog_cleanup(struct prog *prog)
{
    if (prog->engine)
    {
        lsquic_engine_destroy(prog->engine);
        prog->engine = NULL;
    }
    if (prog->prog_certs)
    {
        SSL_CTX_free(prog->prog_certs);
        prog->prog_certs = NULL;
    }
    if (prog->sockfd >= 0)
    {
        close(prog->sockfd);
        prog->sockfd = -1;
    }
    free(prog->bind_spec);
    free(prog->peer_spec);
    free(prog->hostname);
    prog->bind_spec = prog->peer_spec = prog->hostname = NULL;
    if (s_global_inited)
    {
        lsquic_global_cleanup();
        s_global_inited = 0;
    }
}
