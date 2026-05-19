#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "echo_cert.h"
#include "test_cert.h"

static char s_alpn[0x100];
static SSL_CTX *s_server_ctx;

int
add_alpn(const char *alpn)
{
    size_t alpn_len, all_len;

    alpn_len = strlen(alpn);
    if (alpn_len > 255)
        return -1;

    all_len = strlen(s_alpn);
    if (all_len + 1 + alpn_len + 1 > sizeof(s_alpn))
        return -1;

    s_alpn[all_len] = (char) alpn_len;
    memcpy(&s_alpn[all_len + 1], alpn, alpn_len);
    s_alpn[all_len + 1 + alpn_len] = '\0';
    return 0;
}

static int
select_alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen,
            const unsigned char *in, unsigned int inlen, void *arg)
{
    int rv;
    (void) ssl;
    (void) arg;

    rv = SSL_select_next_proto((unsigned char **) out, outlen,
                               in, inlen,
                               (unsigned char *) s_alpn, strlen(s_alpn));
    return rv == OPENSSL_NPN_NEGOTIATED
         ? SSL_TLSEXT_ERR_OK
         : SSL_TLSEXT_ERR_ALERT_FATAL;
}

static SSL_CTX *
create_server_ctx(void)
{
    BIO *bio = NULL;
    X509 *cert = NULL;
    EVP_PKEY *pkey = NULL;
    SSL_CTX *ctx = NULL;

    ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        goto err;

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_alpn_select_cb(ctx, select_alpn, NULL);
    SSL_CTX_set_early_data_enabled(ctx, 1);

    bio = BIO_new_mem_buf(kLsquicEchoCertPem, -1);
    if (!bio)
        goto err;
    cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    bio = NULL;
    if (!cert)
        goto err;

    bio = BIO_new_mem_buf(kLsquicEchoKeyPem, -1);
    if (!bio)
        goto err;
    pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    bio = NULL;
    if (!pkey)
        goto err;

    if (1 != SSL_CTX_use_certificate(ctx, cert))
        goto err;
    if (1 != SSL_CTX_use_PrivateKey(ctx, pkey))
        goto err;
    if (1 != SSL_CTX_check_private_key(ctx))
        goto err;

    (void) SSL_CTX_set_session_cache_mode(ctx, 1);

    X509_free(cert);
    EVP_PKEY_free(pkey);
    return ctx;

err:
    if (bio)
        BIO_free(bio);
    if (cert)
        X509_free(cert);
    if (pkey)
        EVP_PKEY_free(pkey);
    if (ctx)
        SSL_CTX_free(ctx);
    return NULL;
}

struct ssl_ctx_st *
init_embedded_cert(const char *sni)
{
    (void) sni;
    if (!s_server_ctx)
        s_server_ctx = create_server_ctx();
    return s_server_ctx;
}

void
delete_certs(struct ssl_ctx_st *ctx)
{
    if (ctx)
        SSL_CTX_free(ctx);
    if (ctx == s_server_ctx)
        s_server_ctx = NULL;
}
