#ifndef LIBUTP_LSQUIC_TEST_CERT_H
#define LIBUTP_LSQUIC_TEST_CERT_H

struct ssl_ctx_st;

int add_alpn(const char *alpn);
struct ssl_ctx_st *init_embedded_cert(const char *sni);
void delete_certs(struct ssl_ctx_st *ctx);

#endif
