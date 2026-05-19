#ifndef LIBUTP_LSQUIC_TEST_CERT_H
#define LIBUTP_LSQUIC_TEST_CERT_H

#include <sys/queue.h>

#include "lsquic_hash.h"

struct lsquic_hash;
struct ssl_ctx_st;
struct sockaddr;

struct server_cert
{
    char                        *ce_sni;
    struct ssl_ctx_st           *ce_ssl_ctx;
    struct lsquic_hash_elem      ce_hash_el;
};

int
add_alpn(const char *alpn);

int
load_cert(struct lsquic_hash *certs, const char *optarg);

int
init_embedded_cert(struct lsquic_hash **certs, const char *sni);

struct ssl_ctx_st *
lookup_cert(void *cert_lu_ctx, const struct sockaddr *unused, const char *sni);

void
delete_certs(struct lsquic_hash *certs);

#endif
