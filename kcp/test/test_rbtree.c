/*************************************************************************
    > File Name: test_rbtree.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年03月28日 星期五 10时22分54秒
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbtree.h"

struct RBTreeType {
    struct rb_node rbnode;
    int data;
};

static int mytype_cmp(struct rb_node *node, struct rb_node *other)
{
    struct RBTreeType *a = rb_entry(node, struct RBTreeType, rbnode);
    struct RBTreeType *b = rb_entry(other, struct RBTreeType, rbnode);

    return a->data - b->data;
}

static void mytype_insert(struct rb_root *root, struct RBTreeType *data)
{
    struct rb_node **new_place = &(root->rb_node);
    struct rb_node *parent = NULL;
    while (*new_place) {
        int ret = mytype_cmp(*new_place, &data->rbnode);
        parent = *new_place;
        if (ret < 0) {
            new_place = &((*new_place)->rb_left);
        } else if (ret > 0) {
            new_place = &((*new_place)->rb_right);
        } else {
            return;
        }
    }

    rb_link_node(&data->rbnode, parent, new_place);
    rb_insert_color(&data->rbnode, root);
}

void test_foreach()
{
    struct rb_root mytree = RB_ROOT;
    struct rb_node *node;

    for (int i = 0; i < 10; i++) {
        struct RBTreeType *data = (struct RBTreeType *)malloc(sizeof(struct RBTreeType));
        data->data = i;
        mytype_insert(&mytree, data);
    }

    for (node = rb_first(&mytree); node; node = rb_next(node)) {
        struct RBTreeType *data = rb_entry(node, struct RBTreeType, rbnode);
        printf("%d\n", data->data);
    }
}

void test_foreach_erase()
{
    struct rb_root mytree = RB_ROOT;
    struct rb_node *node;

    for (int i = 0; i < 1; i++) {
        struct RBTreeType *data = (struct RBTreeType *)malloc(sizeof(struct RBTreeType));
        data->data = rand() % 299;
        mytype_insert(&mytree, data);
    }

    struct rb_node *next = NULL;
    for (node = rb_first(&mytree); node;) {
        struct RBTreeType *data = rb_entry(node, struct RBTreeType, rbnode);
        next = rb_next(node);
        printf("%d\n", data->data);
        rb_erase(node, &mytree);
        free(data);
        node = next;
    }

    printf("After erase:\n");
    for (node = rb_first(&mytree); node; node = rb_next(node)) {
        struct RBTreeType *data = rb_entry(node, struct RBTreeType, rbnode);
        printf("%d\n", data->data);
    }
}

int main(int argc, char **argv)
{
    srand(time(NULL));
    test_foreach_erase();
    return 0;
}
