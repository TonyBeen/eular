/*************************************************************************
    > File Name: hash.h
    > Author: hsz
    > Brief:
    > Created Time: Fri 03 Feb 2023 10:35:22 AM CST
 ************************************************************************/

#ifndef __EULAR_UTILS_HASH_H__
#define __EULAR_UTILS_HASH_H__

#include <stdint.h>
#include <assert.h>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <initializer_list>
#include <type_traits>

#include <utils/refcount.h>
#include <utils/utils.h>

namespace eular {

struct HashData
{
    struct Node {
        Node *m_next;
        uint32_t m_hash;
    };

    Node *m_fakeNode;     // 永为null
    std::vector<Node *> m_buckets;
    RefCount m_ref;
    int m_size;           // 当前有多少元素
    int m_nodeSize;       // 每个node节点的大小
    short m_numBits;      // prime_deltas数组元素的位置(小于素数的最大2的幂次方)
    int m_numBuckets;     // 容量(为素数)

    void *allocateNode(int align);
    void freeNode(void *node);
    HashData *detach_helper(void (*node_duplicate)(Node *, void *), void (*node_delete)(Node *),
                            int nodeSize, int nodeAlign);
    void free_helper(void (*node_delete)(Node *));
    void rehash(int hint);
    bool grow();

    Node *firstNode();
    static Node *nextNode(Node *node);
    static Node *previousNode(Node *node);

    static const HashData shared_null;
};

template <typename Key, typename Val>
struct HashNode
{
    HashNode *m_next;
    const uint32_t m_hash;    // key计算后的hash值
    const Key m_key;
    Val m_value;

    HashNode(const Key &k, const Val &v, uint32_t h, HashNode *n) :
        m_next(n), m_hash(h), m_key(k), m_value(v) { }
private:
    HashNode(const HashNode &) = delete;
    HashNode &operator=(const HashNode &) = delete;
};

template <typename Key, typename Val, typename Hash = std::hash<Key>, typename KeyEqual = std::equal_to<Key> >
class HashMap
{
    typedef HashNode<Key, Val> Node;
    typedef Hash Hasher;
    typedef KeyEqual KeyEqualer;

public:
    HashMap() noexcept : m_data(const_cast<HashData *>(&HashData::shared_null)), m_hasher(), m_keyEqual() {}
    HashMap(std::initializer_list<std::pair<Key, Val>> list) :
        m_data(const_cast<HashData *>(&HashData::shared_null)), m_hasher(), m_keyEqual()
    {
        m_data->rehash(-std::max<int>(list.size(), 1));
        for (typename std::initializer_list<std::pair<Key, Val>>::const_iterator it = list.begin();
            it != list.end(); ++it) {
            insert(it->first, it->second);
        }
    }
    HashMap(const HashMap &other) noexcept :
        m_data(other.m_data), m_hasher(other.m_hasher), m_keyEqual(other.m_keyEqual)
    {
        m_data->m_ref.ref();
    }
    HashMap &operator=(const HashMap &other)
    {
        if (m_data != other.m_data) {
            HashData *otherData = other.m_data;
            otherData->m_ref.ref();
            if (!m_data->m_ref.deref())
                freeData(m_data);
            m_data = otherData;
            m_hasher = other.m_hasher;
            m_keyEqual = other.m_keyEqual;
        }
        return *this;
    }
    HashMap(HashMap &&other) noexcept :
        m_data(other.m_data), m_hasher(std::move(other.m_hasher)), m_keyEqual(std::move(other.m_keyEqual))
    {
        other.m_data = const_cast<HashData *>(&HashData::shared_null);
    }
    HashMap &operator=(HashMap &&other) noexcept
    {
        if (this != &other) {
            swap(other);
        }

        return *this;
    }
    ~HashMap() noexcept
    {
        if (!m_data->m_ref.deref()) {
            freeData(m_data);
        }
    }

    void swap(HashMap &other) noexcept(noexcept(std::swap(m_data, other.m_data)) &&
                                       noexcept(std::swap(m_hasher, other.m_hasher)) &&
                                       noexcept(std::swap(m_keyEqual, other.m_keyEqual)))
    {
        std::swap(m_data, other.m_data);
        std::swap(m_hasher, other.m_hasher);
        std::swap(m_keyEqual, other.m_keyEqual);
    }

    inline int size() const noexcept { return m_data->m_size; }
    inline bool empty() const noexcept { return m_data->m_size == 0; }
    inline int capacity() const noexcept { return m_data->m_numBuckets; }
    void reserve(size_t size);
    void clear();

    Val *at(const Key &key) noexcept;
    const Val *at(const Key &key) const noexcept;
    Val &operator[](const Key &key);
    const Val &operator[](const Key &key) const;

    class const_iterator;
    class iterator
    {
        friend class const_iterator;
        friend class HashMap<Key, Val, Hash, KeyEqual>;
        HashData::Node *m_node;

    public:
        typedef std::bidirectional_iterator_tag iterator_category;
        typedef uint64_t difference_type;
        typedef Val value_type;
        typedef Val *pointer;
        typedef Val &reference;

        inline iterator() noexcept : m_node(nullptr) { }
        explicit inline iterator(void *node) noexcept : m_node(reinterpret_cast<HashData::Node *>(node)) { }

        inline const Key &key() const noexcept { return concrete(m_node)->m_key; }
        inline Val &value() const noexcept { return concrete(m_node)->m_value; }
        inline Val &operator*() const noexcept { return concrete(m_node)->m_value; }
        inline Val *operator->() const noexcept { return &concrete(m_node)->m_value; }
        inline bool operator==(const iterator &o) const noexcept { return m_node == o.m_node; }
        inline bool operator!=(const iterator &o) const noexcept { return m_node != o.m_node; }

        inline iterator &operator++() noexcept {     // 前置++
            m_node = HashData::nextNode(m_node);
            return *this;
        }
        inline iterator operator++(int) noexcept {   // 后置++
            iterator it = *this;
            m_node = HashData::nextNode(m_node);
            return it;
        }
        inline iterator &operator--() noexcept {
            m_node = HashData::previousNode(m_node);
            return *this;
        }
        inline iterator operator--(int) noexcept {
            iterator it = *this;
            m_node = HashData::previousNode(m_node);
            return it;
        }

        inline bool operator==(const const_iterator &o) const noexcept { return m_node == o.m_node; }
        inline bool operator!=(const const_iterator &o) const noexcept { return m_node != o.m_node; }
    };
    friend class iterator;

    class const_iterator
    {
        friend class iterator;
        friend class HashMap<Key, Val, Hash, KeyEqual>;
        HashData::Node *m_node;

    public:
        typedef std::bidirectional_iterator_tag iterator_category;
        typedef uint64_t difference_type;
        typedef Val value_type;
        typedef const Val *pointer;
        typedef const Val &reference;

        inline const_iterator() noexcept : m_node(nullptr) { }
        explicit inline const_iterator(void *node) :
            m_node(reinterpret_cast<HashData::Node *>(node)) { }
        inline const_iterator(const iterator &o) noexcept :
            m_node(o.m_node) { }

        inline const Key &key() const noexcept { return concrete(m_node)->m_key; }
        inline const Val &value() const noexcept { return concrete(m_node)->m_value; }
        inline const Val &operator*() const noexcept { return concrete(m_node)->m_value; }
        inline const Val *operator->() const noexcept { return &concrete(m_node)->m_value; }
        inline bool operator==(const const_iterator &o) const noexcept { return o.m_node == m_node; }
        inline bool operator!=(const const_iterator &o) const noexcept { return o.m_node != m_node; }

        inline const_iterator &operator++() noexcept {
            m_node = HashData::nextNode(m_node);
            return *this;
        }
        inline const_iterator operator++(int) noexcept {
            const_iterator cit = *this;
            m_node = HashData::nextNode(m_node);
            return cit;
        }
        inline const_iterator &operator--() noexcept {
            m_node = HashData::previousNode(m_node);
            return *this;
        }
        inline const_iterator operator--(int) noexcept {
            const_iterator cit = *this;
            m_node = HashData::previousNode(m_node);
            return cit;
        }
    };

    inline iterator begin() { detach(); return iterator(m_data->firstNode()); }
    inline const_iterator begin() const noexcept { return const_iterator(m_data->firstNode()); }
    inline iterator end() { detach(); return iterator(m_nodeEnd); }
    inline const_iterator end() const noexcept { return const_iterator(m_nodeEnd); }

    iterator erase(iterator it) { return erase(const_iterator(it.m_node)); }
    iterator erase(const_iterator it);
    iterator find(const Key &key);
    const_iterator find(const Key &key) const;
    iterator insert(const Key &key, const Val &value);

protected:
    inline void detach() { if (m_data->m_ref.load() > 1) { detach_helper(); } }
    void detach_helper();
    void freeData(HashData *d);
    Node **findNode(const Key &k, uint32_t hashValue) const;
    Node *createNode(uint32_t h, const Key &key, const Val &value, Node **nextNode);
    void deleteNode(Node *node);
    static void deleteNode2(HashData::Node *node);
    static void duplicateNode(HashData::Node *originalNode, void *newNode);
    uint32_t hashKey(const Key &key) const noexcept(noexcept(m_hasher(key)))
    {
        return static_cast<uint32_t>(m_hasher(key));
    }
    bool keysEqual(const Key &lhs, const Key &rhs) const noexcept(noexcept(m_keyEqual(lhs, rhs)))
    {
        return m_keyEqual(lhs, rhs);
    }

    bool isValidIterator(const iterator &it) const noexcept
    {
        return isValidNode(it.m_node);
    }

    bool isValidIterator(const const_iterator &it) const noexcept
    {
        return isValidNode(it.m_node);
    }

    inline bool isValidNode(const HashData::Node *node) const noexcept
    {
#if !defined(NDEBUG)
        if (node == nullptr) {
            return false;
        }
        while (node->m_next) {
            node = node->m_next;
        }
        return static_cast<const void *>(node) == static_cast<const void *>(m_data);
#else
        UNUSED(node);
        return true;
#endif
    }

    // 将HashData::Node转为HashNode类型
    static inline Node *concrete(HashData::Node *node) noexcept {
        return reinterpret_cast<Node *>(node);
    }

    static inline int alignOfNode() noexcept { return std::max<int>(sizeof(void*), alignof(Node)); }

private:
    union {
        HashData*           m_data;
        HashNode<Key, Val>* m_nodeEnd;
    };
    Hasher      m_hasher;
    KeyEqualer  m_keyEqual;
};

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::detach_helper()
{
    HashData *newData = m_data->detach_helper(duplicateNode, deleteNode2, sizeof(Node), alignOfNode());
    if (m_data != &HashData::shared_null && !m_data->m_ref.deref())
        freeData(m_data);
    m_data = newData;
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::freeData(HashData *d)
{
    d->free_helper(deleteNode2);
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::Node **HashMap<Key, Val, Hash, KeyEqual>::findNode(const Key &k, uint32_t hashValue) const
{
    Node **node = const_cast<Node **>(&m_nodeEnd);
    if (m_data->m_numBuckets) {
        node = reinterpret_cast<Node **>(&m_data->m_buckets[hashValue % m_data->m_numBuckets]);
        assert(*node == m_nodeEnd || (*node)->m_next); // node != nodeEnd 时 node->next必不为空
        while (*node != m_nodeEnd && !((*node)->m_hash == hashValue && keysEqual((*node)->m_key, k))) {
            node = &(*node)->m_next;
        }
    }

    return node;
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::Node *HashMap<Key, Val, Hash, KeyEqual>::createNode(uint32_t h, const Key &key, const Val &value, Node **nextNode)
{
    Node *node = new (m_data->allocateNode(alignOfNode())) Node(key, value, h, *nextNode);
    *nextNode = node;
    ++m_data->m_size;
    return node;
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::deleteNode(Node *node)
{
    deleteNode2(reinterpret_cast<HashData::Node *>(node));
    m_data->freeNode(node);
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::deleteNode2(HashData::Node *node)
{
    concrete(node)->~Node();
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::duplicateNode(HashData::Node *originalNode, void *newNode)
{
    Node *concreteNode = concrete(originalNode);
    new (newNode) Node(concreteNode->m_key, concreteNode->m_value, concreteNode->m_hash, nullptr);
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::reserve(size_t size)
{
    if (size > 0) {
        detach();
        int n = static_cast<int>(size);
        m_data->rehash(-n);
    }
}

template <class Key, class Val, class Hash, class KeyEqual>
void HashMap<Key, Val, Hash, KeyEqual>::clear()
{
    *this = HashMap();
}

template <class Key, class Val, class Hash, class KeyEqual>
Val *HashMap<Key, Val, Hash, KeyEqual>::at(const Key &key) noexcept
{
    try {
        detach();
        uint32_t hashValue = hashKey(key);
        Node **node = findNode(key, hashValue);
        return (*node == m_nodeEnd) ? nullptr : &(*node)->m_value;
    } catch (...) {
        return nullptr;
    }
}

template <class Key, class Val, class Hash, class KeyEqual>
const Val *HashMap<Key, Val, Hash, KeyEqual>::at(const Key &key) const noexcept
{
    try {
        Node **node = findNode(key, hashKey(key));
        return (*node == m_nodeEnd) ? nullptr : &(*node)->m_value;
    } catch (...) {
        return nullptr;
    }
}

template <class Key, class Val, class Hash, class KeyEqual>
Val &HashMap<Key, Val, Hash, KeyEqual>::operator[](const Key &key)
{
    detach();
    uint32_t hashValue = hashKey(key);
    Node **node = findNode(key, hashValue);
    if (*node == m_nodeEnd) {
        if (m_data->grow()) {
            node = findNode(key, hashValue);
        }

        Node *inserted = createNode(hashValue, key, Val(), node);
        return inserted->m_value;
    }

    return (*node)->m_value;
}

template <class Key, class Val, class Hash, class KeyEqual>
const Val &HashMap<Key, Val, Hash, KeyEqual>::operator[](const Key &key) const
{
    const Val *value = at(key);
    if (value) {
        return *value;
    }

    throw std::out_of_range("HashMap::operator[] const key not found");
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::iterator HashMap<Key, Val, Hash, KeyEqual>::erase(const_iterator it)
{
    assert(isValidIterator(it));
    if (it == const_iterator(m_nodeEnd)) {
        return iterator(it.m_node);
    }

    if (m_data->m_ref.load() > 1) { // 当存在共享时需分离并找到it在新HashMap的位置
        Key key = concrete(it.m_node)->m_key;
        detach();
        it = const_iterator(*findNode(key, hashKey(key)));
        if (it == const_iterator(m_nodeEnd)) {
            return end();
        }
    }

    iterator ret(it.m_node);
    ++ret;

    Node *node = concrete(it.m_node);
    Node **ptr = reinterpret_cast<Node **>(&m_data->m_buckets[node->m_hash % m_data->m_numBuckets]);
    while (*ptr != node) {
        ptr = &(*ptr)->m_next;
    }

    *ptr = node->m_next;
    deleteNode(node);
    --m_data->m_size;
    return ret;
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::iterator HashMap<Key, Val, Hash, KeyEqual>::find(const Key &key)
{
    detach();
    return iterator(*findNode(key, hashKey(key)));
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::const_iterator HashMap<Key, Val, Hash, KeyEqual>::find(const Key &key) const
{
    return const_iterator(*findNode(key, hashKey(key)));
}

template <class Key, class Val, class Hash, class KeyEqual>
typename HashMap<Key, Val, Hash, KeyEqual>::iterator HashMap<Key, Val, Hash, KeyEqual>::insert(const Key &key, const Val &value)
{
    detach();

    uint32_t hashValue = hashKey(key);
    Node **node = findNode(key, hashValue);
    if (*node == m_nodeEnd) {
        if (m_data->grow()) {
            node = findNode(key, hashValue);
        }

        return iterator(createNode(hashValue, key, value, node));
    }

    return iterator(*node);
}


} // namespace eular

#endif // __EULAR_UTILS_HASH_H__
