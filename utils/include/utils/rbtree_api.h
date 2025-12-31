/*************************************************************************
    > File Name: rbtree_api.h
    > Author: hsz
    > Brief:
    > Created Time: Wed 23 Nov 2022 11:10:12 AM CST
 ************************************************************************/

#ifndef __UTILS_RBTREE_API_H__
#define __UTILS_RBTREE_API_H__

#include "utils/rbtree_base.h"
#include "utils/utils.h"

EXTERN_C_BEGIN

void rbtree_init(rb_root *root);
void rbtree_clear(rb_root *root, void (*free_node_cb)(rb_node *));

/**
 * @brief 将node插入到红黑树root
 * 
 * @param root 红黑树
 * @param node 红黑树节点
 * @param compare_callback 比较回调。返回0表示相等，返回1往右子树放，返回-1往左子树放。
 * @return rb_node* 成功返回插入的节点地址，失败返回nullptr
 */
rb_node *rbtree_insert(rb_root *root, rb_node *newNode, int (*compare_callback)(rb_node *, rb_node *));

/**
 * @brief 从红黑树中删除等于seed的节点，但不释放此节点内存
 * 
 * @param root 红黑树根节点
 * @param seed 种子
 * @param equal_callback 判断相等回调
 * @return rb_node* 成功返回删除的节点，失败返回nullptr
 */
rb_node *rbtree_erase(rb_root *root, const void *seed, int (*equal_callback)(rb_node *, const void *));

rb_node *rbtree_erase_node(rb_root *root, rb_node *node);

/**
 * @brief 对红黑树进行有序遍历(在遍历中删除是不安全的?)
 * 
 * @param root 红黑树根节点
 * @param data_callback 数据处理回调. 从循环中退出返回true，不退出返回false
 * @return true 遍历成功
 * @return false 遍历失败
 */
bool rbtree_foreach(rb_root *root, bool (*data_callback)(rb_node *node));

/**
 * @brief 从红黑树中查找与seed相等的节点
 * 
 * @param root 红黑树根节点
 * @param seed 种子数据
 * @param equal_callback 同rbtree_erase的compare_callback
 * @return rb_node* 返回查找到的节点，失败返回nullptr
 */
rb_node *rbtree_search(rb_root *root, const void *seed, int (*equal_callback)(rb_node *, const void *));

size_t rbtree_size(rb_root *root);

EXTERN_C_END

#endif // __UTILS_RBTREE_API_H__
