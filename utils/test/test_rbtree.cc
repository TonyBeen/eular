/*************************************************************************
    > File Name: test_rbtree.cc
    > Author: hsz
    > Brief:
    > Created Time: Fri 17 Dec 2021 10:56:23 AM CST
 ************************************************************************/

#include <vector>

#include "catch/catch.hpp"

#include <utils/rbtree_base.h>

namespace {

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
    rb_node **node = &(root->rb_node);
    rb_node *parent = nullptr;
    bool exists = false;
    while (*node != nullptr) {
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

    if (exists) {
        delete new_rbtree_node;
        return nullptr;
    }
    return newNode;
}

std::vector<int> inorder_values(rb_root *root)
{
    std::vector<int> values;
    for (rb_node *node = rb_first(root); node != nullptr; node = rb_next(node)) {
        values.push_back(rbtree_node_entry(node)->data);
    }
    return values;
}

void clear_tree(rb_root *root)
{
    while (root->rb_node != nullptr) {
        rb_node *node = rb_first(root);
        rb_erase(node, root);
        delete rbtree_node_entry(node);
    }
}

} // namespace

TEST_CASE("rbtree_insert_and_navigation", "[rbtree]")
{
    rb_root root{};

    rb_node *inserted = insert(&root, 10);
    REQUIRE(inserted != nullptr);
    CHECK(rb_next(rb_last(&root)) == nullptr);
    CHECK(rb_prev(rb_first(&root)) == nullptr);
    CHECK(insert(&root, 10) == nullptr);

    clear_tree(&root);
}

TEST_CASE("rbtree_inorder_traversal_is_sorted", "[rbtree]")
{
    rb_root root{};
    const int values[] = {10, 4, 15, 12, 1, 7};
    for (int value : values) {
        REQUIRE(insert(&root, value) != nullptr);
    }

    const std::vector<int> ordered = inorder_values(&root);
    const std::vector<int> expected{1, 4, 7, 10, 12, 15};
    CHECK(ordered == expected);
    REQUIRE(root.rb_node != nullptr);
    CHECK(root.rb_node->rb_parent == nullptr);

    clear_tree(&root);
}
