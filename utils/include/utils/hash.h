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

#include <vector>
#include <initializer_list>

#include <utils/refcount.h>
#include <utils/utils.h>

namespace eular {

struct HashData
{
    struct Node {
        Node *next;
        uint32_t hash;
    };

    Node *fakeNode;     // 永为null
    std::vector<Node *> buckets;
    RefCount ref;
    int size;           // 当前有多少元素
    int nodeSize;       // 每个node节点的大小
    short numBits;      // prime_deltas数组元素的位置(小于素数的最大2的幂次方)
    int numBuckets;     // 容量(为素数)

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
    HashNode *next;
    const uint32_t hash;    // key计算后的hash值
    const Key key;
    Val value;

    HashNode(const Key &k, const Val &v, uint32_t h, HashNode *n) :
        next(n), hash(h), key(k), value(v) { }
    inline bool sameKeyWith(const Key &k, uint32_t h) { return (k == key && h == hash); }

private:
    HashNode(const HashNode &) = delete;
    HashNode &operator=(const HashNode &) = delete;
};

// HashComputeBase
class HashCmptBase
{
public:
    virtual ~HashCmptBase() { }
    virtual uint32_t hash() const { return 0; }

protected:
    static uint32_t compute(const uint8_t *key, uint32_t size);
    static uint32_t compute2(const void *key, uint32_t size);
};

template <typename Key, typename Val>
class HashMap
{
    typedef HashNode<Key, Val> Node;
    static_assert(std::is_base_of<eular::HashCmptBase, Key>::value, "must inherit from HashCmptBase!");

public:
    HashMap() noexcept : data(const_cast<HashData *>(&HashData::shared_null)) {}
    HashMap(std::initializer_list<std::pair<Key, Val>> list) :
        data(const_cast<HashData *>(&HashData::shared_null))
    {
        data->rehash(-std::max<int>(list.size(), 1));
        for (typename std::initializer_list<std::pair<Key, Val>>::const_iterator it = list.begin();
            it != list.end(); ++it) {
            insert(it->first, it->second);
        }
    }
    HashMap(const HashMap &other) noexcept :
        data(other.data)
    {
        data->ref.ref();
    }
    HashMap &operator=(const HashMap &other)
    {
        if (data != other.data) {
            HashData *o = other.data;
            o->ref.ref();
            if (!data->ref.deref())
                freeData(data);
            data = o;
        }
        return *this;
    }
    HashMap(HashMap &&other) noexcept :
        data(other.data)
    {
        other.data = const_cast<HashData *>(&HashData::shared_null);
    }
    HashMap &operator=(HashMap &&other) noexcept
    {
        if (this != &other) {
            swap(other);
        }

        return *this;
    }
    ~HashMap()
    {
        if (!data->ref.deref()) {
            freeData(data);
        }
    }

    void swap(HashMap &other) { std::swap(data, other.data); }

    inline int size() const { return data->size; }
    inline bool empty() const { return data->size == 0; }
    inline int capacity() const { return data->numBuckets; }
    void reserve(size_t size);
    void clear();

    Val &at(const Key &key);
    const Val &at(const Key &key, const Val &v = Val()) const;
    Val &operator[](const Key &key) { detach(); return at(key); }
    const Val operator[](const Key &key) const { return at(key); }

    class const_iterator;
    class iterator
    {
        friend class const_iterator;
        friend class HashMap<Key, Val>;
        HashData::Node *n;

    public:
        typedef std::bidirectional_iterator_tag iterator_category;
        typedef uint64_t difference_type;
        typedef Val value_type;
        typedef Val *pointer;
        typedef Val &reference;

        inline iterator() : n(nullptr) { }
        explicit inline iterator(void *node) : n(reinterpret_cast<HashData::Node *>(node)) { }
    
        inline const Key &key() const { return concrete(n)->key; }
        inline Val &value() const { return concrete(n)->value; }
        inline Val &operator*() const { return concrete(n)->value; }
        inline Val *operator->() const { return &concrete(n)->value; }
        inline bool operator==(const iterator &o) const { return n == o.n; }
        inline bool operator!=(const iterator &o) const { return n != o.n; }

        inline iterator &operator++() {     // 前置++
            n = HashData::nextNode(n);
            return *this;
        }
        inline iterator operator++(int) {   // 后置++
            iterator it = *this;
            n = HashData::nextNode(n);
            return it;
        }
        inline iterator &operator--() {
            n = HashData::previousNode(n);
            return *this;
        }
        inline iterator operator--(int) {
            iterator it = *this;
            n = HashData::previousNode(n);
            return it;
        }

        inline bool operator==(const const_iterator &o) const { return n == o.n; }
        inline bool operator!=(const const_iterator &o) const { return n != o.n; }
    };
    friend class iterator;

    class const_iterator
    {
        friend class iterator;
        friend class HashMap<Key, Val>;
        HashData::Node *n;
    
    public:
        typedef std::bidirectional_iterator_tag iterator_category;
        typedef uint64_t difference_type;
        typedef Val value_type;
        typedef const Val *pointer;
        typedef const Val &reference;

        inline const_iterator() : n(nullptr) { }
        explicit inline const_iterator(void *node) :
            n(reinterpret_cast<HashData::Node *>(node)) { }
        inline const_iterator(const iterator &o) :
            n(o.n) { }
        
        inline const Key &key() const { return concrete(n)->key; }
        inline const Val &value() const { return concrete(n)->value; }
        inline const Val &operator*() const { return concrete(n)->value; }
        inline const Val *operator->() const { return &concrete(n)->value; }
        inline bool operator==(const const_iterator &o) { return o.n == n; }
        inline bool operator!=(const const_iterator &o) { return o.n != n; }

        inline const_iterator &operator++() {
            n = HashData::nextNode(n);
            return *this;
        }
        inline const_iterator operator++(int) {
            const_iterator cit = *this;
            n = HashData::nextNode(n);
            return cit;
        }
        inline const_iterator &operator--() {
            n = HashData::previousNode(n);
            return *this;
        }
        inline const_iterator operator--(int) {
            const_iterator cit = *this;
            n = HashData::previousNode(n);
            return cit;
        }
    };

    inline iterator begin() { detach(); return iterator(data->firstNode()); }
    inline const_iterator begin() const { return const_iterator(data->firstNode()); }
    inline iterator end() { detach(); return iterator(nodeEnd); }
    inline const_iterator end() const { return const_iterator(nodeEnd); }

    iterator erase(iterator it) { return erase(const_iterator(it.n)); }
    iterator erase(const_iterator it);
    iterator find(const Key &key);
    const_iterator find(const Key &key) const;
    iterator insert(const Key &key, const Val &value);

protected:
    inline void detach() { if (data->ref.load() > 1) { detach_helper(); } }
    void detach_helper();
    void freeData(HashData *d);
    Node **findNode(const Key &k, uint32_t hash) const;
    Node *createNode(uint32_t h, const Key &key, const Val &value, Node **nextNode);
    void deleteNode(Node *node);
    static void deleteNode2(HashData::Node *node);
    static void duplicateNode(HashData::Node *originalNode, void *newNode);

    bool isValidIterator(const iterator &it) const noexcept
    {
        return isValidNode(it.n);
    }

    bool isVaildIterator(const const_iterator &it) const noexcept
    {
        return isValidNode(it.n);
    }

    inline bool isValidNode(const Node *node) const noexcept
    {
#if !defined(NDEBUG)
        while (node->next) {
            node = node->next;
        }
        return (static_cast<void *>(node) == data);
#else
        UNUSED(node);
        return true;
#endif
    }

    // 将HashData::Node转为HashNode类型
    static inline Node *concrete(HashData::Node *node) {
        return reinterpret_cast<Node *>(node);
    }

    static inline int alignOfNode() { return std::max<int>(sizeof(void*), alignof(Node)); }

private:
    union {
        HashData *data;
        HashNode<Key, Val> *nodeEnd;
    };
};

template <class Key, class Val>
void HashMap<Key, Val>::detach_helper()
{
    HashData *x = data->detach_helper(duplicateNode, deleteNode2, sizeof(Node), alignOfNode());
    if (data != &HashData::shared_null && !data->ref.deref())
        freeData(data);
    data = x;
}

template <class Key, class Val>
void HashMap<Key, Val>::freeData(HashData *d)
{
    d->free_helper(deleteNode2);
}

template <class Key, class Val>
typename HashMap<Key, Val>::Node **HashMap<Key, Val>::findNode(const Key &k, uint32_t hash) const
{
    Node **node = const_cast<Node **>(&nodeEnd);
    if (data->numBuckets) {
        node = reinterpret_cast<Node **>(&data->buckets[hash % data->numBuckets]);
        assert(*node == nodeEnd || (*node)->next); // node != nodeEnd 时 node->next必不为空
        while (*node != nodeEnd && !((*node)->sameKeyWith(k, hash))) {
            node = &(*node)->next;
        }
    }

    return node;
}

template <class Key, class Val>
typename HashMap<Key, Val>::Node *HashMap<Key, Val>::createNode(uint32_t h, const Key &key, const Val &value, Node **nextNode)
{
    Node *node = new (data->allocateNode(alignOfNode())) Node(key, value, h, *nextNode);
    *nextNode = node;
    ++data->size;
    return node;
}

template <class Key, class Val>
void HashMap<Key, Val>::deleteNode(Node *node)
{
    deleteNode2(reinterpret_cast<HashData::Node *>(node));
    data->freeNode(node);
}

template <class Key, class Val>
void HashMap<Key, Val>::deleteNode2(HashData::Node *node)
{
    concrete(node)->~Node();
}

template <class Key, class Val>
void HashMap<Key, Val>::duplicateNode(HashData::Node *originalNode, void *newNode)
{
    Node *concreteNode = concrete(originalNode);
    new (newNode) Node(concreteNode->key, concreteNode->value, concreteNode->hash, nullptr);
}

template <class Key, class Val>
void HashMap<Key, Val>::reserve(size_t size)
{
    if (size > 0) {
        detach();
        int n = static_cast<int>(size);
        data->rehash(-n);
    }
}

template <class Key, class Val>
void HashMap<Key, Val>::clear()
{
    *this = HashMap();
}

template <class Key, class Val>
Val &HashMap<Key, Val>::at(const Key &key)
{
    detach();
    Node **node = findNode(key, key.hash());
    if (*node == nodeEnd) {
        iterator it = insert(key, Val());
        if (it == end()) {
            throw std::exception();
        }
        return *it;
    }

    return (*node)->value;
}

template <class Key, class Val>
const Val &HashMap<Key, Val>::at(const Key &key, const Val &v) const
{
    Node **node = findNode(key, key.hash());
    if (*node == nodeEnd) {
        return v;
    }

    return (*node)->value;
}

template <class Key, class Val>
typename HashMap<Key, Val>::iterator HashMap<Key, Val>::erase(const_iterator it)
{
    assert(isValidIterator(it));
    if (it == const_iterator(nodeEnd)) {
        return iterator(it.n);
    }

    if (data->ref.load() > 1) { // 当存在共享时需分离并找到it在新HashMap的位置
        int bucketNum = (it.n->hash % data->numBuckets);
        const_iterator bucketIterator(data->buckets[bucketNum]);
        int stepsFromBucketStartToIte = 0;
        while (bucketIterator != it) {
            ++stepsFromBucketStartToIte;
            ++bucketIterator;
        }
        detach();
        it = const_iterator(data->buckets[bucketNum]);
        while (stepsFromBucketStartToIte > 0) {
            --stepsFromBucketStartToIte;
            ++it;
        }
    }

    iterator ret(it.n);
    ++ret;

    Node *node = concrete(it.n);
    Node **ptr = reinterpret_cast<Node *>(&data->buckets[node->hash % data->numBuckets]);
    while (*ptr != node) {
        *ptr = (*ptr)->next;
    }

    *ptr = node->next;
    deleteNode(node);
    --data->size;
    return ret;
}

template <class Key, class Val>
typename HashMap<Key, Val>::iterator HashMap<Key, Val>::find(const Key &key)
{
    detach();
    return iterator(*findNode(key, key.hash()));
}

template <class Key, class Val>
typename HashMap<Key, Val>::const_iterator HashMap<Key, Val>::find(const Key &key) const
{
    return const_iterator(*findNode(key, key.hash()));
}

template <class Key, class Val>
typename HashMap<Key, Val>::iterator HashMap<Key, Val>::insert(const Key &key, const Val &value)
{
    detach();

    uint32_t hash = key.hash();
    Node **node = findNode(key, hash);
    if (*node == nodeEnd) {
        if (data->grow()) {
            node = findNode(key, hash);
        }

        return iterator(createNode(hash, key, value, node));
    }

    return iterator(*node);
}


} // namespace eular

#endif // __EULAR_UTILS_HASH_H__
