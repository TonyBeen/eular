#ifndef __KCP_INTERNAL_SOCKET_SET_H__
#define __KCP_INTERNAL_SOCKET_SET_H__

#include <stdbool.h>
#include <stdint.h>

#include "kcp_def.h"
#include "rbtree.h"

typedef struct rb_root connection_set_t;

EXTERN_C_BEGIN

void connection_set_init(connection_set_t *root);

struct KcpConnection *connection_set_search(connection_set_t *root, uint32_t conv);

bool connection_set_insert(connection_set_t *root, struct KcpConnection *node);

struct KcpConnection *connection_set_erase(connection_set_t *root, int32_t conv);

void connection_set_erase_node(connection_set_t *root, struct KcpConnection *node);

struct KcpConnection *connection_first(connection_set_t *root);
struct KcpConnection *connection_next(struct KcpConnection *node);
struct KcpConnection *connection_last(connection_set_t *node);

typedef void (*connection_set_destroy_cb_t)(struct KcpConnection *node);

void connection_set_clear(connection_set_t *root, connection_set_destroy_cb_t cb);

EXTERN_C_END

#endif // __KCP_INTERNAL_SOCKET_SET_H__
