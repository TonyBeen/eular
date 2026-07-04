/*************************************************************************
    > File Name: map_node.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 03:35:13 PM CST
 ************************************************************************/

#ifndef __UTILS_MAP_NODE_HPP__
#define __UTILS_MAP_NODE_HPP__

#include <cstdlib>
#include <functional>
#include <new>
#include <utility>

#include <utils/alloc.h>
#include <utils/exception.h>
#include <utils/rbtree_base.h>
#include <utils/refcount.h>
#include <utils/utils.h>

namespace detail {
struct MapNodeBase {
    struct rb_node __rb_node;
    static bool    isValidNode(rb_root* root, rb_node* node) noexcept;
};

inline size_t mapNodeAlignmentThreshold() noexcept { return 2 * sizeof(void*); }

inline void* mapNodeAllocate(size_t size, size_t alignment)
{
    void* node = alignment > mapNodeAlignmentThreshold() ? AlignedAlloc(size, alignment) : std::malloc(size);
    if (node == nullptr) {
        throw std::bad_alloc();
    }

    return node;
}

inline void mapNodeDeallocate(void* node, size_t alignment) noexcept
{
    if (alignment > mapNodeAlignmentThreshold()) {
        AlignedFree(node);
    } else {
        std::free(node);
    }
}

template <typename Key, typename Val>
struct MapNode : public MapNodeBase {
    DISALLOW_COPY_AND_ASSIGN(MapNode);
    Key key;
    Val value;

    const MapNode* nextNode() const noexcept
    {
        rb_node* currentNode = const_cast<rb_node*>(&__rb_node);
        return map_node_entry(rb_next(currentNode));
    }

    // NOTE this转化为const MapNode *表示明确调用const修饰的nextNode
    MapNode* nextNode() noexcept { return const_cast<MapNode*>(const_cast<const MapNode*>(this)->nextNode()); }

    const MapNode* previousNode() const noexcept
    {
        rb_node* currentNode = const_cast<rb_node*>(&__rb_node);
        return map_node_entry(rb_prev(currentNode));
    }

    MapNode* previousNode() noexcept { return const_cast<MapNode*>(const_cast<const MapNode*>(this)->previousNode()); }

    static MapNode* map_node_entry(rb_node* node) noexcept
    {
        if (node == NULL) {
            return NULL;
        }
        return static_cast<MapNode*>(rb_entry(node, MapNode, __rb_node));
    }

    template <typename T>
    static typename std::enable_if<!std::is_integral<T>::value && !std::is_enum<T>::value>::type callDestructor(T& t)
    {
        (void)t;
        t.~T();
    }

    template <typename T>
    static typename std::enable_if<std::is_integral<T>::value || std::is_enum<T>::value>::type callDestructor(
        T&) noexcept
    {
    }
};

template <typename Key, typename Val, typename Compare = std::less<Key> >
struct MapData {
    typedef MapNode<Key, Val> Node;

    eular::RefCount reference;
    struct rb_root  __rb_root;
    Node*           nullNode;  // for end()
    size_t          node_count;

    ~MapData() { clear(); }

    Node* begin() noexcept
    {
        // NOTE 当为空时rb_first返回nullptr, 故等于end()
        return MapNode<Key, Val>::map_node_entry(rb_first(&__rb_root));
    }

    const Node* begin() const noexcept
    {
        rb_root* root = &(const_cast<MapData*>(this)->__rb_root);
        return const_cast<const Node*>(Node::map_node_entry(rb_first(root)));
    }

    Node*       end() noexcept { return nullNode; }
    const Node* end() const noexcept { return nullNode; }

    Node*       rbegin() noexcept { return MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root)); }
    const Node* rbegin() const noexcept { return const_cast<const Node*>(Node::map_node_entry(rb_last(&__rb_root))); }
    Node*       rend() noexcept { return nullNode; }
    const Node* rend() const noexcept { return nullNode; }

    Node* nextNode(const Node*) noexcept;
    Node* prevNode(const Node*) noexcept;

    size_t size() const noexcept { return node_count; }

    /**
     * @brief 向红黑树插入节点
     * @param key 键
     * @param val 值
     * @return 创建内存失败时返回null，如果键不存在则返回新节点地址，如果存在，返回null
     */
    Node* insert(const Key& key, const Val& val);
    template <typename K, typename V>
    Node*       emplace(K&& key, V&& val);
    Node*       insertNode(Node* node);
    Node*       find(const Key& key);
    const Node* find(const Key& key) const;

    /**
     * @brief 删除节点
     * @param key 键
     * @return 删除键为key的节点，并返回下一节点位置
     */
    Node* erase(const Key& key);
    Node* erase(const Node*);
    Node* eraseRange(const Node* first, const Node* last);
    Node* extract(const Node*);

    void clear();

    template <typename K, typename V>
    Node* createNode(K&& k, V&& v)
    {
        Node* n = nullptr;
        try {
            n = static_cast<Node*>(mapNodeAllocate(sizeof(Node), alignof(Node)));
            new (&n->key) Key(std::forward<K>(k));  // placement new. already aligned
            try {
                new (&n->value) Val(std::forward<V>(v));
            } catch (...) {
                n->key.~Key();
                throw;
            }
        } catch (...) {
            if (n != nullptr) {
                mapNodeDeallocate(n, alignof(Node));
            }
            return nullptr;
        }

        return n;
    }

    void freeNode(Node* node)
    {
        MapNode<Key, Val>::callDestructor(node->key);
        MapNode<Key, Val>::callDestructor(node->value);
        mapNodeDeallocate(node, alignof(Node));
    }

    static MapData* create() { return new MapData(); }

    static void destroy(MapData* data) { delete data; }

private:
    static bool isLess(const Key& lhs, const Key& rhs) { return Compare()(lhs, rhs); }

    static bool isEqual(const Key& lhs, const Key& rhs) { return !isLess(lhs, rhs) && !isLess(rhs, lhs); }

    MapData() noexcept : nullNode(nullptr), node_count(0) { __rb_root.rb_node = nullptr; }
};

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::nextNode(const Node* curr) noexcept
{
    Node* lastNode = MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root));
    if (curr == lastNode || curr == end()) {
        return end();
    }

    return const_cast<Node*>(curr)->nextNode();
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::prevNode(const Node* curr) noexcept
{
    Node* lastNode = MapNode<Key, Val>::map_node_entry(rb_last(&__rb_root));
    if (begin() == curr) {
        return nullptr;
    }

    if (curr == end()) {
        return lastNode;
    }

    return const_cast<Node*>(curr)->previousNode();
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::insert(const Key& key, const Val& val)
{
    return emplace(key, val);
}

template <typename Key, typename Val, typename Compare>
template <typename K, typename V>
MapNode<Key, Val>* MapData<Key, Val, Compare>::emplace(K&& key, V&& val)
{
    rb_root*         root = &__rb_root;
    struct rb_node** node = &(root->rb_node);
    struct rb_node*  parent = nullptr;
    Key              mapKey(std::forward<K>(key));
    while (nullptr != (*node)) {
        parent = *node;
        MapNode<Key, Val>* p = MapNode<Key, Val>::map_node_entry(parent);
        if (isLess(mapKey, p->key)) {
            node = &(*node)->rb_left;
        } else if (isLess(p->key, mapKey)) {
            node = &(*node)->rb_right;
        } else {
            return nullptr;
        }
    }

    MapNode<Key, Val>* newMapNode = createNode(std::move(mapKey), std::forward<V>(val));
    if (newMapNode == nullptr) {
        return nullptr;
    }

    rb_node* newNode = &newMapNode->__rb_node;
    rb_link_node(newNode, parent, node);
    rb_insert_color(newNode, root);
    ++node_count;

    return newMapNode;
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::find(const Key& key)
{
    rb_root*           root = &__rb_root;
    struct rb_node*    curr = root->rb_node;
    MapNode<Key, Val>* currNode = nullptr;
    bool               exist = false;
    while (curr) {
        currNode = MapNode<Key, Val>::map_node_entry(curr);
        if (isLess(key, currNode->key)) {
            curr = curr->rb_left;
        } else if (isLess(currNode->key, key)) {
            curr = curr->rb_right;
        } else {
            exist = true;
            break;
        }
    }

    return exist ? currNode : nullptr;
}

template <typename Key, typename Val, typename Compare>
const MapNode<Key, Val>* MapData<Key, Val, Compare>::find(const Key& key) const
{
    rb_root*                 root = &(const_cast<MapData*>(this)->__rb_root);
    struct rb_node*          curr = root->rb_node;
    const MapNode<Key, Val>* currNode = nullptr;
    bool                     exist = false;
    while (curr) {
        currNode = MapNode<Key, Val>::map_node_entry(curr);
        if (isLess(key, currNode->key)) {
            curr = curr->rb_left;
        } else if (isLess(currNode->key, key)) {
            curr = curr->rb_right;
        } else {
            exist = true;
            break;
        }
    }

    return exist ? currNode : nullptr;
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::insertNode(Node* newMapNode)
{
    if (newMapNode == nullptr) {
        return nullptr;
    }

    rb_node*         newNode = &newMapNode->__rb_node;
    rb_root*         root = &__rb_root;
    struct rb_node** node = &(root->rb_node);
    struct rb_node*  parent = nullptr;
    while (*node != nullptr) {
        parent = *node;
        MapNode<Key, Val>* p = MapNode<Key, Val>::map_node_entry(parent);
        if (isLess(newMapNode->key, p->key)) {
            node = &(*node)->rb_left;
        } else if (isLess(p->key, newMapNode->key)) {
            node = &(*node)->rb_right;
        } else {
            return nullptr;
        }
    }

    rb_link_node(newNode, parent, node);
    rb_insert_color(newNode, root);
    ++node_count;
    return newMapNode;
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::erase(const Key& key)
{
    MapNode<Key, Val>* ret = find(key);
    if (ret == nullptr) {
        return nullptr;
    }

    return erase(ret);
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::erase(const Node* currNode)
{
    if (currNode == end()) {
        return end();
    }

    Node* curr = const_cast<Node*>(currNode);
#ifndef NDEBUG
    if (MapNodeBase::isValidNode(&__rb_root, &curr->__rb_node) == false) {
        throw eular::Exception("node is not in this rbtree");
    }
#endif

    MapNode<Key, Val>* next = this->nextNode(currNode);
    rb_erase(&curr->__rb_node, &__rb_root);
    freeNode(curr);
    --node_count;

    return next;
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::eraseRange(const Node* first, const Node* last)
{
    Node* curr = const_cast<Node*>(first);
    Node* stop = const_cast<Node*>(last);

#ifndef NDEBUG
    if (curr != nullptr && curr != end() && MapNodeBase::isValidNode(&__rb_root, &curr->__rb_node) == false) {
        throw eular::Exception("first node is not in this rbtree");
    }
    if (stop != nullptr && stop != end() && MapNodeBase::isValidNode(&__rb_root, &stop->__rb_node) == false) {
        throw eular::Exception("last node is not in this rbtree");
    }
#endif

    while (curr != nullptr && curr != stop && curr != end()) {
        rb_node* next = rb_next(&curr->__rb_node);
        rb_erase(&curr->__rb_node, &__rb_root);
        freeNode(curr);
        --node_count;
        curr = Node::map_node_entry(next);
    }

    return stop ? stop : end();
}

template <typename Key, typename Val, typename Compare>
MapNode<Key, Val>* MapData<Key, Val, Compare>::extract(const Node* currNode)
{
    if (currNode == end()) {
        return nullptr;
    }

    Node* curr = const_cast<Node*>(currNode);
#ifndef NDEBUG
    if (MapNodeBase::isValidNode(&__rb_root, &curr->__rb_node) == false) {
        throw eular::Exception("node is not in this rbtree");
    }
#endif

    rb_erase(&curr->__rb_node, &__rb_root);
    curr->__rb_node.rb_parent = nullptr;
    curr->__rb_node.rb_left = nullptr;
    curr->__rb_node.rb_right = nullptr;
    curr->__rb_node.rb_color = RB_RED;
    --node_count;
    return curr;
}

template <typename Key, typename Val, typename Compare>
void MapData<Key, Val, Compare>::clear()
{
    rb_root*           root = &__rb_root;
    rb_node*           node = nullptr;
    MapNode<Key, Val>* curr = nullptr;
    while ((node = root->rb_node) != nullptr) {
        rb_erase(node, root);
        curr = MapNode<Key, Val>::map_node_entry(node);
        freeNode(curr);
    }
    node_count = 0;
}

}  // namespace detail

#endif
