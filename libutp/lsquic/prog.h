#ifndef UTP_LSQUIC_PROG_H
#define UTP_LSQUIC_PROG_H

#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>

#include "lsquic/lsquic.h"
#include "test_common.h"

struct ssl_ctx_st;

struct prog {
    struct lsquic_engine_api      prog_api;
    struct lsquic_engine_settings prog_settings;
    struct ssl_ctx_st*            prog_certs;

    int                            prog_flags;
    const struct lsquic_stream_if* prog_stream_if;
    void*                          prog_stream_if_ctx;

    char* bind_spec;
    char* peer_spec;
    char* hostname;
    int   quiet;

    int                     sockfd;
    struct sockaddr_storage local_sa;
    socklen_t               local_sa_len;
    struct sockaddr_storage peer_sa;
    socklen_t               peer_sa_len;
    int                     has_peer_sa;

    lsquic_engine_t* engine;
    int              running;
};

void prog_init(struct prog* prog, int flags, struct sport_head* sports, const struct lsquic_stream_if* stream_if,
               void* stream_if_ctx);
int  prog_set_opt(struct prog* prog, char opt, const char* value);
int  prog_prep(struct prog* prog);
int  prog_connect(struct prog* prog, const struct sockaddr* local_sa, unsigned short base_plpmtu);
void prog_run(struct prog* prog);
void prog_stop(struct prog* prog);
void prog_cleanup(struct prog* prog);

#endif
