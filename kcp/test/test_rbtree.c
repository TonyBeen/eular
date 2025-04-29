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

struct mytype {
    struct rb_node rbnode;
    int data;
};

static int mytype_cmp(struct rb_node *node, struct rb_node *other)
{
    struct mytype *a = rb_entry(node, struct mytype, rbnode);
    struct mytype *b = rb_entry(other, struct mytype, rbnode);

    return a->data - b->data;
}

static void mytype_insert(struct rb_root *root, struct mytype *data)
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

int main(int argc, char **argv)
{
    struct rb_root mytree = RB_ROOT;
    struct rb_node *node;

    for (int i = 0; i < 10; i++) {
        struct mytype *data = (struct mytype *)malloc(sizeof(struct mytype));
        data->data = i;
        mytype_insert(&mytree, data);
    }

    for (node = rb_first(&mytree); node; node = rb_next(node)) {
        struct mytype *data = rb_entry(node, struct mytype, rbnode);
        printf("%d\n", data->data);
    }

    return 0;
}
