/*************************************************************************
    > File Name: map_node.cpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 04:10:21 PM CST
 ************************************************************************/

#include "utils/map_node.h"

#include <assert.h>

namespace detail {

bool MapNodeBase::isValidNode(rb_root* root, rb_node* node) noexcept
{
    assert(root && node);
    rb_node* parent = node;
    bool     valid = false;
    while (parent) {
        if (parent == root->rb_node) {
            valid = true;
            break;
        }
        // if (parent == parent->rb_parent) {
        //     break;
        // }
        parent = parent->rb_parent;  // 根节点的父节点是空值
    }

    return valid;
}

}  // namespace detail
