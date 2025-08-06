/*************************************************************************
    > File Name: test_rbtree.cc
    > Author: hsz
    > Brief:
    > Created Time: Fri 17 Dec 2021 10:56:23 AM CST
 ************************************************************************/

#include <string>
#include <utils/rbtree_base.h>
#include <iostream>

using namespace std;

struct rbtree_node {
    rb_node rbnode;
    int data;
};

#define rbtree_node_entry(ptr) rb_entry(ptr, rbtree_node, rbnode)

rb_node *insert(rb_root *root, int d)
{
    rbtree_node *new_rbtree_node = new rbtree_node;
    new_rbtree_node->data = d;
    rb_node *newNode = &new_rbtree_node->rbnode;
    struct rb_node **node = &(root->rb_node);
    struct rb_node *parent = nullptr;
    bool exists = false;
    int compareResult = 0;
    while (nullptr != (*node)) {
        parent = *node;
        rbtree_node *curr = rbtree_node_entry(parent);
        if (curr->data < d) {
            node = &(*node)->rb_right;
        } else if (curr->data > d) {
            node = &(*node)->rb_left;
        } else {
            exists = true;
            break;
        }
    }

    if (!exists) {
        rb_link_node(newNode, parent, node);
        rb_insert_color(newNode, root);
    }

    return exists ? nullptr : newNode;
}

void rbtree_foreach(rb_root *root)
{
    if (root == nullptr) {
        return;
    }

    struct rb_node *begin = rb_first(root);
    struct rb_node *next = begin;
    struct rb_node *end = rb_last(root);

    if (begin == nullptr) { // no nodes
        return;
    }

    rbtree_node *rbnode = nullptr;
    while (begin != end) {
        next = rb_next(next);
        rbnode = rbtree_node_entry(begin);
        printf("%s() data = %d\n", __func__, rbnode->data);
        begin = next;
    }

    rbnode = rbtree_node_entry(begin);
    printf("%s() data = %d\n", __func__, rbnode->data);
}

int main(int argc, char **argv)
{
    rb_root root;
    root.rb_node = nullptr;

    rb_node *ret = insert(&root, 10);
    printf("%p\n", ret);
    ret = rb_next(rb_last(&root));
    printf("%p\n", ret);

    ret = rb_prev(rb_first(&root));
    printf("%p\n", ret);

    rbtree_foreach(&root);

    printf("%p\n", root.rb_node);
    printf("%p\n", root.rb_node->rb_parent);

    return 0;
}
