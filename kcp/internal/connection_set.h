#ifndef __KCP_INTERNAL_SOCKET_SET_H__
#define __KCP_INTERNAL_SOCKET_SET_H__

#include <stdbool.h>

#include "kcp_def.h"
#include "rbtree.h"
#include "kcp_protocol.h"

typedef struct rb_root connection_set_t;

EXTERN_C_BEGIN

void connection_set_init(connection_set_t *root);

kcp_connection_t *connection_set_search(connection_set_t *root, int32_t conv);

bool connection_set_insert(connection_set_t *root, kcp_connection_t *node);

kcp_connection_t *connection_set_erase(connection_set_t *root, int32_t conv);

void connection_set_erase_node(connection_set_t *root, kcp_connection_t *node);

kcp_connection_t *connection_first(connection_set_t *root);

typedef void (*connection_set_destroy_cb_t)(kcp_connection_t *node);

void connection_set_clear(connection_set_t *root, connection_set_destroy_cb_t cb);

EXTERN_C_END

#endif // __KCP_INTERNAL_SOCKET_SET_H__
