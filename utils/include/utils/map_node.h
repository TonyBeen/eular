/*************************************************************************
    > File Name: map_node.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 03:35:13 PM CST
 ************************************************************************/

#ifndef __UTILS_MAP_NODE_HPP__
#define __UTILS_MAP_NODE_HPP__

#include <functional>

#include <utils/utils.h>
#include <utils/rbtree_base.h>
#include <utils/refcount.h>

namespace detail {
struct MapNodeBase {
    struct rb_node __rb_node;
    static bool isValidNode(rb_root *root, rb_node *node);
};

template <typename Key, typename Val>
struct MapNode : public MapNodeBase
{
    DISALLOW_COPY_AND_ASSIGN(MapNode);
    Key key;
    Val value;

    const MapNode *nextNode() const
    {
        rb_node *currentNode = const_cast<rb_node *>(&__rb_node);
        return map_node_entry(rb_next(currentNode));
    }

    // NOTE this转化为const MapNode *表示明确调用const修饰的nextNode
    MapNode *nextNode()
    {
        return const_cast<MapNode *>(const_cast<const MapNode *>(this)->nextNode());
    }

    const MapNode *previousNode() const
    {
        rb_node *currentNode = const_cast<rb_node *>(&__rb_node);
        return map_node_entry(rb_prev(currentNode));
    }

    MapNode *previousNode()
    {
        return const_cast<MapNode *>(const_cast<const MapNode *>(this)->previousNode());
    }

    static MapNode *map_node_entry(rb_node *node)
    {
        if (node == NULL) {
            return NULL;
        }
        return static_cast<MapNode *>(rb_entry(node, MapNode, __rb_node));
    }

    template <typename T>
    static typename std::enable_if<!std::is_integral<T>::value && !std::is_enum<T>::value>::type
    callDestructor(T &t) { (void)t; t.~T(); }

    template <typename T>
    static typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value>::type
    callDestructor(T &) {}
};

struct MapDataBase {
    eular::RefCount reference;
    struct rb_root __rb_root;
    struct MapNodeBase *nullNode; // for end()
    size_t node_count;

    MapNodeBase *createNode(int size, int alignment);
    void freeNode(MapNodeBase *ptr, int alignment);

    static MapDataBase *CreateData();
    static void FreeData(MapDataBase *d);
};

template <typename Key, typename Val>
struct MapData : public MapDataBase {
    typedef MapNode<Key, Val> Node;

    Node *begin()
    {
        // NOTE 当为空时rb_first返回nullptr, 故等于end()
        return MapNode<Key, Val>::map_node_entry(rb_first(&__rb_root));
    }

    const Node *begin() const
    {
        rb_root *root = &(const_cast<MapData *>(this)->__rb_root);
        return const_cast<const Node *>(Node::map_node_entry(rb_first(root)));
    }

    Node *end() { return static_cast<Node *>(nullNode); }
    const Node *end() const { return static_cast<Node *>(nullNode); }

    Node *rbegin()
    {
        return MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root));
    }
    const Node *rbegin() const
    {
        return const_cast<const Node *>(Node::map_node_entry(rb_last(&__rb_root)));
    }
    Node *rend() { return static_cast<Node *>(nullNode); }
    const Node *rend() const { return static_cast<Node *>(nullNode); }

    Node *nextNode(const Node *);
    Node *prevNode(const Node *);

    size_t size() const { return node_count; }

    /**
     * @brief 向红黑树插入节点
     * @param key 键
     * @param val 值
     * @return 创建内存失败时返回null，如果键不存在则返回新节点地址，如果存在，返回存在的节点地址
     */
    Node *insert(const Key &key, const Val &val);
    Node *find(const Key &key);

    /**
     * @brief 删除节点
     * @param key 键
     * @return 删除键为key的节点，并返回下一节点位置
     */
    Node *erase(const Key &key);
    Node *erase(const Node *, bool check = true);

    void clear();

    Node *createNode(const Key &k, const Val &v)
    {
        Node *n = static_cast<Node *>(MapDataBase::createNode(sizeof(Node), alignof(Node)));
        try {
            new (&n->key) Key(k); // placement new. already aligned
            try {
                new (&n->value) Val(v);
            } catch (...) {
                n->key.~Key();
                MapDataBase::freeNode(n, alignof(Node));
                n = NULL;
            }
        } catch (...) {
            MapDataBase::freeNode(n, alignof(Node));
            n = NULL;
        }

        return n;
    }

    void freeNode(Node *node)
    {
        MapNode<Key, Val>::callDestructor(node->key);
        MapNode<Key, Val>::callDestructor(node->value);
        MapDataBase::freeNode(node, alignof(Node));
    }

    static MapData *create()
    {
        return static_cast<MapData *>(CreateData());
    }

private:
    MapData() {}
};

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::nextNode(const Node *curr)
{
    Node *lastNode = MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root));
    if (curr == lastNode || curr == end()) {
        return end();
    }

    return const_cast<Node *>(curr)->nextNode();
}

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::prevNode(const Node *curr)
{
    Node *lastNode = MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root));
    if (begin() == curr) {
        return nullptr;
    }

    if (curr == end()) {
        return lastNode;
    }

    return const_cast<Node *>(curr)->previousNode();
}

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::insert(const Key &key, const Val &val)
{
    MapNode<Key, Val> *newMapNode = createNode(key, val);
    if (newMapNode == nullptr) {
        return nullptr;
    }

    rb_node *newNode = &newMapNode->__rb_node;
    rb_root *root = &__rb_root;
    struct rb_node **node = &(root->rb_node);
    struct rb_node *parent = nullptr;
    bool exists = false;
    while (nullptr != (*node)) {
        parent = *node;
        MapNode<Key, Val> *p = MapNode<Key, Val>::map_node_entry(parent);
        if (std::less<const Key &>()(key, p->key)) { // key < p->key
            node = &(*node)->rb_left;
        } else if (std::greater<const Key &>()(key, p->key)) { // key > p->key
            node = &(*node)->rb_right;
        } else { // key == p->key
            exists = true;
            freeNode(newMapNode);
            newMapNode = nullptr;
            break;
        }
    }

    if (!exists) {
        rb_link_node(newNode, parent, node);
        rb_insert_color(newNode, root);
        ++node_count;
    }

    return newMapNode;
}

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::find(const Key &key)
{
    rb_root *root = &__rb_root;
    struct rb_node *curr = root->rb_node;
    MapNode<Key, Val> *currNode = nullptr;
    bool exist = false;
    while (curr) {
        currNode = MapNode<Key, Val>::map_node_entry(curr);
        if (std::less<const Key &>()(key, currNode->key)) { // key < currNode->key
            curr = curr->rb_left;
        } else if (std::greater<const Key &>()(key, currNode->key)) { // key > currNode->key
            curr = curr->rb_right;
        } else { // key == currNode->key
            exist = true;
            break;
        }
    }

    return exist ? currNode : nullptr;
}

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::erase(const Key &key)
{
    MapNode<Key, Val> *ret = find(key);
    if (ret == nullptr) {
        return nullptr;
    }

    return erase(ret, false);
}

template <typename Key, typename Val>
MapNode<Key, Val> *MapData<Key, Val>::erase(const Node *currNode, bool check)
{
    if (currNode == end()) {
        return end();
    }

    Node *curr = const_cast<Node *>(currNode);
    if (check && MapNodeBase::isValidNode(&__rb_root, &curr->__rb_node) == false) {
        throw std::logic_error("node is not in this rbtree");
    }

    MapNode<Key, Val> *next = this->nextNode(currNode);
    rb_erase(&curr->__rb_node, &__rb_root);
    freeNode(curr);
    --node_count;

    return next;
}

template <typename Key, typename Val>
void MapData<Key, Val>::clear()
{
    rb_root *root = &__rb_root;
    rb_node *node = nullptr;
    MapNode<Key, Val> *curr = nullptr;
    while ((node = root->rb_node) != nullptr) {
        rb_erase(node, root);
        curr = MapNode<Key, Val>::map_node_entry(node);
        freeNode(curr);
    }
    node_count = 0;
}

} // namespace detail

#endif