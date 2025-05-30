#include "connection_set.h"
#include <assert.h>

#include "kcp_protocol.h"

void connection_set_init(connection_set_t *root)
{
    if (root != NULL) {
        root->rb_node = NULL;
    }
}

struct KcpConnection *connection_set_search(connection_set_t *root, uint32_t conv)
{
    struct rb_node *node = root->rb_node;
  	while (node) {
  		struct KcpConnection *pthis = container_of(node, struct KcpConnection, node_rbtree);

		if (conv < pthis->conv) {
            node = node->rb_left;
        } else if (conv > pthis->conv) {
  			node = node->rb_right;
        } else {
  			return pthis;
        }
	}

	return NULL;
}

bool connection_set_insert(connection_set_t *root, struct KcpConnection *data)
{
    if (root == NULL || data == NULL) {
        return false;
    }

    struct rb_node **new = &(root->rb_node), *parent = NULL;
    while (*new) {
        struct KcpConnection *pthis = container_of(*new, struct KcpConnection, node_rbtree);

        parent = *new;
        if (data->conv < pthis->conv) {
            new = &((*new)->rb_left);
        } else if (data->conv > pthis->conv) {
            new = &((*new)->rb_right);
        } else {
            return false;
        }
    }

    rb_link_node(&data->node_rbtree, parent, new);
    rb_insert_color(&data->node_rbtree, root);
    return true;
}

struct KcpConnection *connection_set_erase(connection_set_t *root, int32_t conv)
{
    if (root == NULL) {
        return NULL;
    }

    struct KcpConnection *pthis = connection_set_search(root, conv);
    if (pthis) {
        rb_erase(&pthis->node_rbtree, root);
    }

    return pthis;
}

void connection_set_erase_node(connection_set_t *root, struct KcpConnection *node)
{
    if (root != NULL && node != NULL) {
        rb_erase(&node->node_rbtree, root);
    }
}

struct KcpConnection *connection_first(connection_set_t *root)
{
    if (root == NULL) {
        return NULL;
    }

    struct rb_node *node = rb_first(root);
    if (node) {
        return rb_entry(node, struct KcpConnection, node_rbtree);
    }

    return NULL;
}

struct KcpConnection *connection_next(struct KcpConnection *node)
{
    if (node == NULL) {
        return NULL;
    }

    struct rb_node *next = rb_next(&node->node_rbtree);
    return next ? rb_entry(next, struct KcpConnection, node_rbtree) : NULL;
}

struct KcpConnection *connection_last(connection_set_t *node)
{
    if (node == NULL) {
        return NULL;
    }

    struct rb_node *last = rb_last(node);
    if (last) {
        return rb_entry(last, struct KcpConnection, node_rbtree);
    }

    return NULL;
}

void connection_set_clear(connection_set_t *root, connection_set_destroy_cb_t cb)
{
    if (root == NULL) {
        return;
    }

    struct rb_node* node = NULL;
    while((node = rb_first(root))) {
        rb_erase(node, root);
        if (cb) {
            struct KcpConnection *pthis = rb_entry(node, struct KcpConnection, node_rbtree);
            cb(pthis);
        }
    }
}