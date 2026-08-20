#include "tls_stream.h"

#include <limits.h>
#include <stdlib.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#define UTP_NTRS_TLS_IO_BUFFER_SIZE 16384u

struct utp_ntrs_tls_stream {
  struct bufferevent *transport;      // TCP 字节流
  SSL *ssl;                           // BoringSSL 会话
  struct evbuffer *plaintext;         // 等待 SSL_write 的完整明文
  utp_ntrs_tls_callbacks_t callbacks; // 上层连接回调
  void *user_data;                    // 上层连接
  bool server;                        // 服务端握手角色
  bool tcp_connected;
  bool handshake_complete;
  bool closed;
};

static bool utp_ntrs_tls_stream_flush_encrypted(utp_ntrs_tls_stream_t *stream) {
  uint8_t buffer[UTP_NTRS_TLS_IO_BUFFER_SIZE];
  BIO *write_bio = SSL_get_wbio(stream->ssl);

  while (BIO_pending(write_bio) > 0) {
    const int32_t read_length =
        BIO_read(write_bio, buffer, (int32_t)sizeof(buffer));

    if (read_length <= 0 || bufferevent_write(stream->transport, buffer,
                                              (size_t)read_length) != 0) {
      return false;
    }
  }
  return true;
}

static void utp_ntrs_tls_stream_notify_closed(utp_ntrs_tls_stream_t *stream) {
  if (!stream->closed) {
    stream->closed = true;
    if (stream->callbacks.closed != NULL) {
      stream->callbacks.closed(stream->user_data);
    }
  }
}

static bool utp_ntrs_tls_stream_write_pending(utp_ntrs_tls_stream_t *stream) {
  while (stream->handshake_complete &&
         evbuffer_get_length(stream->plaintext) != 0u) {
    const size_t length = evbuffer_get_length(stream->plaintext);
    unsigned char *data = evbuffer_pullup(stream->plaintext, -1);
    const int32_t result = SSL_write(stream->ssl, data, (int32_t)length);

    if (result > 0) {
      if (evbuffer_drain(stream->plaintext, (size_t)result) != 0 ||
          !utp_ntrs_tls_stream_flush_encrypted(stream)) {
        return false;
      }
      continue;
    }
    {
      const int32_t error = SSL_get_error(stream->ssl, result);

      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        return utp_ntrs_tls_stream_flush_encrypted(stream);
      }
    }
    return false;
  }
  return true;
}

static bool utp_ntrs_tls_stream_drive(utp_ntrs_tls_stream_t *stream) {
  uint8_t buffer[UTP_NTRS_TLS_IO_BUFFER_SIZE];

  if (!stream->tcp_connected || stream->closed) {
    return !stream->closed;
  }
  if (!stream->handshake_complete) {
    const int32_t result = SSL_do_handshake(stream->ssl);

    if (result == 1) {
      stream->handshake_complete = true;
      if (stream->callbacks.ready != NULL) {
        stream->callbacks.ready(stream->user_data);
      }
    } else {
      const int32_t error = SSL_get_error(stream->ssl, result);

      if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
        return false;
      }
    }
    if (!utp_ntrs_tls_stream_flush_encrypted(stream) ||
        !stream->handshake_complete) {
      return !stream->closed;
    }
  }
  for (;;) {
    const int32_t result =
        SSL_read(stream->ssl, buffer, (int32_t)sizeof(buffer));

    if (result > 0) {
      if (stream->callbacks.plaintext != NULL) {
        stream->callbacks.plaintext(stream->user_data, buffer, (size_t)result);
      }
      if (stream->closed) {
        return false;
      }
      continue;
    }
    {
      const int32_t error = SSL_get_error(stream->ssl, result);

      if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
        return false;
      }
    }
    break;
  }
  return utp_ntrs_tls_stream_write_pending(stream);
}

static void utp_ntrs_tls_stream_on_read(struct bufferevent *transport,
                                        void *user_data) {
  utp_ntrs_tls_stream_t *const stream = user_data;
  struct evbuffer *const input = bufferevent_get_input(transport);

  while (evbuffer_get_length(input) != 0u) {
    uint8_t buffer[UTP_NTRS_TLS_IO_BUFFER_SIZE];
    const int32_t read_length = evbuffer_remove(input, buffer, sizeof(buffer));

    if (read_length <= 0 || BIO_write(SSL_get_rbio(stream->ssl), buffer,
                                      read_length) != read_length) {
      utp_ntrs_tls_stream_notify_closed(stream);
      return;
    }
    if (!utp_ntrs_tls_stream_drive(stream)) {
      utp_ntrs_tls_stream_notify_closed(stream);
      return;
    }
  }
}

static void utp_ntrs_tls_stream_on_write(struct bufferevent *transport,
                                         void *user_data) {
  utp_ntrs_tls_stream_t *const stream = user_data;

  (void)transport;
  if (!utp_ntrs_tls_stream_drive(stream)) {
    utp_ntrs_tls_stream_notify_closed(stream);
  }
}

static void utp_ntrs_tls_stream_on_event(struct bufferevent *transport,
                                         int16_t events, void *user_data) {
  utp_ntrs_tls_stream_t *const stream = user_data;

  (void)transport;
  if ((events & BEV_EVENT_CONNECTED) != 0) {
    stream->tcp_connected = true;
    if (!utp_ntrs_tls_stream_drive(stream)) {
      utp_ntrs_tls_stream_notify_closed(stream);
    }
    return;
  }
  if ((events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) != 0) {
    utp_ntrs_tls_stream_notify_closed(stream);
  }
}

static SSL_CTX *utp_ntrs_tls_context_new(void) {
  SSL_CTX *const context = SSL_CTX_new(TLS_method());

  if (context == NULL ||
      SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1) {
    SSL_CTX_free(context);
    return NULL;
  }
  SSL_CTX_set_verify(context, SSL_VERIFY_NONE, NULL);
  return context;
}

SSL_CTX *utp_ntrs_tls_server_context_new(const char *certificate_path,
                                         const char *private_key_path) {
  SSL_CTX *const context = utp_ntrs_tls_context_new();

  if (context == NULL ||
      SSL_CTX_use_certificate_chain_file(context, certificate_path) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, private_key_path,
                                  SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context) != 1) {
    SSL_CTX_free(context);
    return NULL;
  }
  return context;
}

SSL_CTX *utp_ntrs_tls_client_context_new(void) {
  return utp_ntrs_tls_context_new();
}

void utp_ntrs_tls_context_free(SSL_CTX *context) { SSL_CTX_free(context); }

utp_ntrs_tls_stream_t *
utp_ntrs_tls_stream_new(struct event_base *base, int32_t fd, SSL_CTX *context,
                        bool server, const utp_ntrs_tls_callbacks_t *callbacks,
                        void *user_data) {
  utp_ntrs_tls_stream_t *stream = calloc(1u, sizeof(*stream));
  BIO *read_bio = NULL;
  BIO *write_bio = NULL;

  if (stream == NULL) {
    return NULL;
  }
  stream->ssl = SSL_new(context);
  stream->plaintext = evbuffer_new();
  stream->transport = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
  if (stream->ssl == NULL || stream->plaintext == NULL ||
      stream->transport == NULL || (read_bio = BIO_new(BIO_s_mem())) == NULL ||
      (write_bio = BIO_new(BIO_s_mem())) == NULL) {
    BIO_free(read_bio);
    BIO_free(write_bio);
    utp_ntrs_tls_stream_free(stream);
    return NULL;
  }
  stream->callbacks = *callbacks;
  stream->user_data = user_data;
  stream->server = server;
  SSL_set_mode(stream->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_set_bio(stream->ssl, read_bio, write_bio);
  if (server) {
    stream->tcp_connected = true;
    SSL_set_accept_state(stream->ssl);
  } else {
    SSL_set_connect_state(stream->ssl);
  }
  bufferevent_setcb(stream->transport, utp_ntrs_tls_stream_on_read,
                    utp_ntrs_tls_stream_on_write, utp_ntrs_tls_stream_on_event,
                    stream);
  bufferevent_enable(stream->transport, EV_READ | EV_WRITE);
  if (server && !utp_ntrs_tls_stream_drive(stream)) {
    utp_ntrs_tls_stream_free(stream);
    return NULL;
  }
  return stream;
}

void utp_ntrs_tls_stream_free(utp_ntrs_tls_stream_t *stream) {
  if (stream != NULL) {
    bufferevent_free(stream->transport);
    SSL_free(stream->ssl);
    evbuffer_free(stream->plaintext);
    free(stream);
  }
}

bool utp_ntrs_tls_stream_connected(utp_ntrs_tls_stream_t *stream) {
  stream->tcp_connected = true;
  return utp_ntrs_tls_stream_drive(stream);
}

bool utp_ntrs_tls_stream_send(utp_ntrs_tls_stream_t *stream,
                              const uint8_t *data, size_t length) {
  if (stream->closed || length > (size_t)INT32_MAX ||
      evbuffer_add(stream->plaintext, data, length) != 0) {
    return false;
  }
  return utp_ntrs_tls_stream_drive(stream);
}

bool utp_ntrs_tls_stream_flush(utp_ntrs_tls_stream_t *stream) {
  return !stream->closed &&
         bufferevent_flush(stream->transport, EV_WRITE, BEV_FLUSH) == 0;
}

struct bufferevent *
utp_ntrs_tls_stream_transport(utp_ntrs_tls_stream_t *stream) {
  return stream->transport;
}
