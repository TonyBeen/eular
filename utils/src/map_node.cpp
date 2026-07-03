/*************************************************************************
    > File Name: map_node.cpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 04:10:21 PM CST
 ************************************************************************/

#include "utils/map_node.h"

#include <assert.h>

#include "utils/alloc.h"
#include "utils/sysdef.h"

namespace detail {

static inline int AlignmentThreshold() { return 2 * sizeof(void*); }

static inline void* map_allocate(int size, int alignment)
{
    return alignment > AlignmentThreshold() ? AlignedAlloc(size, alignment) : ::malloc(size);
}

static inline void map_deallocate(void* node, int alignment)
{
    if (alignment > AlignmentThreshold()) {
        AlignedFree(node);
    } else {
        ::free(node);
    }
}

bool MapNodeBase::isValidNode(rb_root* root, rb_node* node)
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

void* mapNodeAllocate(int size, int alignment)
{
    void* node = map_allocate(size, alignment);
    if (node == NULL) {
        throw std::bad_alloc();
    }

    memset(node, 0, size);
    return node;
}

void mapNodeDeallocate(void* node, int alignment) { map_deallocate(node, alignment); }

}  // namespace detail
