/*************************************************************************
    > File Name: test_rbtree_api.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 11:08:34 AM CST
 ************************************************************************/

#include <iostream>
#include <stdio.h>
#include <stdlib.h>

#include "catch/catch.hpp"

#include <utils/rbtree_api.h>

struct RBData {
    rb_node rbNode;
    int data;
};

rb_root root;

#define rbdata_entry(node) rb_entry(node, RBData, rbNode)

void free_node(rb_node *node)
{
    RBData *data = rbdata_entry(node);
    printf("%s() free(%p) data = %d\n", __func__, data, data->data);
    delete data;
}

bool processData(rb_node *node)
{
    RBData *data = rbdata_entry(node);
    printf("\t%s() data = %d\n", __func__, data->data);
    if (data->data == 4) {
        free_node(rbtree_erase_node(&root, node)); // 测试在遍历时删除节点是否产生异常
    }
    return false;
}

int compare_key(rb_node *node, const void *seed)
{
    int key = *static_cast<const int *>(seed);
    RBData *data = rbdata_entry(node);
    if (data->data < key) {
        return -1;
    }
    if (data->data > key) {
        return 1;
    }

    return 0;
}

int compare_node(rb_node *left, rb_node *right)
{
    RBData *l = rbdata_entry(left);
    RBData *r = rbdata_entry(right);

    if (l->data < r->data) {
        return -1;
    }
    if (l->data > r->data) {
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    rbtree_init(&root);
    rbtree_foreach(&root, processData);

    printf("\n--->rbtree size = %zu\n", rbtree_size(&root));

    printf("insert to(%p) 1, 3, 5\n", &root);
    RBData *newNode = new RBData;
    newNode->data = 1;
    rbtree_insert(&root, &newNode->rbNode, compare_node);

    newNode = new RBData;
    newNode->data = 3;
    rbtree_insert(&root, &newNode->rbNode, compare_node);

    newNode = new RBData;
    newNode->data = 5;
    rbtree_insert(&root, &newNode->rbNode, compare_node);

    printf("\nforeach rbtree(%p)\n", &root);
    rbtree_foreach(&root, processData);

    printf("\ninsert to(%p) 2, 4\n", &root);
    newNode = new RBData;
    newNode->data = 2;
    rbtree_insert(&root, &newNode->rbNode, compare_node);

    newNode = new RBData;
    newNode->data = 4;
    rbtree_insert(&root, &newNode->rbNode, compare_node);

    printf("\n--->rbtree size = %zu\n", rbtree_size(&root));

    printf("\nforeach rbtree(%p) and will erase data 4\n", &root);
    rbtree_foreach(&root, processData);

    printf("\n--->rbtree size = %zu\n", rbtree_size(&root));

    int num = 3;
    printf("\nerase %d from rbtree(%p)\n", num, &root);
    rb_node *nodeErase = rbtree_erase(&root, &num, compare_key);
    free_node(nodeErase);

    printf("\nforeach rbtree(%p)\n", &root);
    rbtree_foreach(&root, processData);

    printf("\nclear rbtree\n");
    rbtree_clear(&root, free_node);
    printf("\n--->rbtree size = %zu\n", rbtree_size(&root));
    return 0;
}
