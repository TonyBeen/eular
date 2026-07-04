/*************************************************************************
    > File Name: map.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 04:29:18 PM CST
 ************************************************************************/

#ifndef __EULAR_UTILS_MAP_H__
#define __EULAR_UTILS_MAP_H__

#include <assert.h>

#include <initializer_list>
#include <memory>
#include <utility>

#include <utils/exception.h>
#include <utils/map_node.h>

namespace eular {
template <typename Key, typename Val, typename Compare = std::less<Key>>
class Map
{
    typedef Key     KeyType;
    typedef Val     ValType;
    typedef Compare CompareType;
    using Data = detail::MapData<KeyType, ValType, CompareType>;
    using Node = detail::MapNode<KeyType, ValType>;

public:
    Map() noexcept : mRBtree(nullptr) {}
    Map(std::initializer_list<std::pair<KeyType, ValType>> initList) : mRBtree(nullptr)
    {
        typename std::initializer_list<std::pair<KeyType, ValType>>::const_iterator it;
        for (it = initList.begin(); it != initList.end(); ++it) {
            insert(it->first, it->second);
        }
    }

    Map(const Map<KeyType, ValType, CompareType>& other) : mRBtree(nullptr)
    {
        mRBtree = other.mRBtree;
        assert(this != &other);
        if (this != &other && mRBtree != nullptr) {
            mRBtree->reference.ref();
        }
    }

    Map(Map<KeyType, ValType, CompareType>&& other) noexcept : mRBtree(nullptr)
    {
        if (this != std::addressof(other)) {
            mRBtree = other.mRBtree;
            other.mRBtree = nullptr;
        }
    }

    ~Map() noexcept { destroy(); }

    Map& operator=(const Map<KeyType, ValType, CompareType>& other)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        destroy();
        mRBtree = other.mRBtree;
        if (mRBtree != nullptr) {
            mRBtree->reference.ref();
        }
        return *this;
    }

    Map& operator=(Map<KeyType, ValType, CompareType>&& other) noexcept
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        destroy();
        mRBtree = other.mRBtree;
        other.mRBtree = nullptr;
        return *this;
    }

    class const_iterator;
    class iterator
    {
        friend class Map<Key, Val, Compare>;
        friend class const_iterator;

    public:
        iterator() noexcept : currentNode(nullptr), owner(nullptr) {}
        iterator(Node* n, Data* data) noexcept : currentNode(n), owner(data) {}

        inline const KeyType& key() const noexcept { return currentNode->key; }
        inline ValType&       value() const noexcept { return currentNode->value; }
        inline ValType*       operator->() const noexcept { return &currentNode->value; }
        inline bool           operator==(const iterator& o) const noexcept { return currentNode == o.currentNode; }
        inline bool           operator!=(const iterator& o) const noexcept { return currentNode != o.currentNode; }
        inline bool operator==(const const_iterator& o) const noexcept { return currentNode == o.currentNode; }
        inline bool operator!=(const const_iterator& o) const noexcept { return currentNode != o.currentNode; }

        inline iterator& operator++() noexcept
        {  // 前置++
            currentNode = owner ? owner->nextNode(currentNode) : nullptr;
            return *this;
        }

        inline iterator operator++(int) noexcept
        {  // 后置++
            iterator it = *this;
            currentNode = owner ? owner->nextNode(currentNode) : nullptr;
            return it;
        }

        inline iterator& operator--() noexcept
        {
            currentNode = owner ? owner->prevNode(currentNode) : nullptr;
            return *this;
        }

        inline iterator operator--(int) noexcept
        {
            iterator it = *this;
            currentNode = owner ? owner->prevNode(currentNode) : nullptr;
            return it;
        }

    private:
        Node* currentNode;
        Data* owner;
    };
    friend class iterator;

    class const_iterator
    {
        friend class Map<Key, Val, Compare>;
        friend class iterator;

    public:
        const_iterator() noexcept : currentNode(nullptr), owner(nullptr) {}
        const_iterator(const iterator& o) noexcept : currentNode(o.currentNode), owner(o.owner) {}
        const_iterator(const Node* n, const Data* data) noexcept : currentNode(n), owner(const_cast<Data*>(data)) {}

        inline const KeyType& key() const noexcept { return currentNode->key; }
        inline const ValType& value() const noexcept { return currentNode->value; }
        inline const ValType* operator->() const noexcept { return &currentNode->value; }
        inline bool operator==(const const_iterator& o) const noexcept { return currentNode == o.currentNode; }
        inline bool operator!=(const const_iterator& o) const noexcept { return currentNode != o.currentNode; }

        inline const_iterator& operator++() noexcept
        {  // 前置++
            currentNode = owner ? owner->nextNode(currentNode) : nullptr;
            return *this;
        }

        inline const_iterator operator++(int) noexcept
        {  // 后置++
            const_iterator it = *this;
            currentNode = owner ? owner->nextNode(currentNode) : nullptr;
            return it;
        }

        inline const_iterator& operator--() noexcept
        {
            currentNode = owner ? owner->prevNode(currentNode) : nullptr;
            return *this;
        }

        inline const_iterator operator--(int) noexcept
        {
            const_iterator it = *this;
            currentNode = owner ? owner->prevNode(currentNode) : nullptr;
            return it;
        }

    private:
        const Node* currentNode;
        Data*       owner;
    };
    friend class const_iterator;

    /* 返回iterator对红黑树做detach操作是因为iterator可以修改变量，会导致拷贝的那份也修改 */
    inline iterator begin()
    {
        detach();
        return iterator(mRBtree->begin(), mRBtree);
    }
    inline const_iterator begin() const noexcept
    {
        return const_iterator(mRBtree ? mRBtree->begin() : nullptr, mRBtree);
    }
    inline const_iterator cbegin() const noexcept { return begin(); }
    // inline iterator rbegin() { detach(); return iterator(mRBtree->rbegin()); }
    inline const_iterator rbegin() const noexcept
    {
        return const_iterator(mRBtree ? mRBtree->rbegin() : nullptr, mRBtree);
    }
    inline const_iterator crbegin() const noexcept { return rbegin(); }
    inline iterator       end()
    {
        detach();
        return iterator(mRBtree->end(), mRBtree);
    }
    inline const_iterator end() const noexcept { return const_iterator(mRBtree ? mRBtree->end() : nullptr, mRBtree); }
    inline const_iterator cend() const noexcept { return end(); }
    // inline iterator rend() { detach(); return iterator(mRBtree->rend()); }
    inline const_iterator rend() const noexcept { return const_iterator(mRBtree ? mRBtree->rend() : nullptr, mRBtree); }
    inline const_iterator crend() const noexcept { return rend(); }

    // NOTE: 不提供可变 rbegin/rend, 避免反向遍历中 erase(iterator) 的返回方向语义不一致
    iterator insert(const KeyType& k, const ValType& v);
    template <typename K, typename V>
    iterator       emplace(K&& k, V&& v);
    iterator       erase(const KeyType& k);
    iterator       erase(const iterator& it);
    iterator       erase(const iterator& first, const iterator& last);
    iterator       find(const KeyType& key);
    const_iterator find(const KeyType& key) const;
    bool           contains(const KeyType& key) const;
    ValType        value(const KeyType& key, const ValType& defaultValue = ValType()) const;
    ValType&       operator[](const KeyType& key);
    ValType        operator[](const KeyType& key) const;
    void           clear();
    void           merge(Map<KeyType, ValType, CompareType>& other);
    void           swap(Map<KeyType, ValType, CompareType>& other) noexcept;
    bool           empty() const noexcept { return size() == 0; }
    size_t         size() const noexcept { return mRBtree ? mRBtree->size() : 0; }

protected:
    void detach();
    bool isDetached() const noexcept { return mRBtree == nullptr || mRBtree->reference.load() == 0; }
    void detach_helper();
    void destroy() noexcept;

private:
    detail::MapData<KeyType, ValType, CompareType>* mRBtree;
};

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::detach()
{
    if (mRBtree && mRBtree->reference.load() > 0) {
        detach_helper();
    } else if (mRBtree == nullptr) {
        mRBtree = Data::create();
    }
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::detach_helper()
{
    Data* data = Data::create();
    try {
        detail::MapNode<Key, Val>* it = nullptr;
        // QT源码采用自拷贝，但是当节点过多时可能会造成栈溢出，所以在此选择循环
        for (it = mRBtree->begin(); it != mRBtree->end(); it = mRBtree->nextNode(it)) {
            if (data->insert(it->key, it->value) == nullptr) {
                throw Exception("Map::detach error");
            }
        }
    } catch (...) {
        Data::destroy(data);
        throw;
    }

    Data* old = mRBtree;
    mRBtree = data;
    old->reference.deref();
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::destroy() noexcept
{
    if (mRBtree) {
        if (mRBtree->reference.load() > 0) {
            mRBtree->reference.deref();
        } else {
            Data::destroy(mRBtree);
        }
    }
    mRBtree = nullptr;
}

/**
 * @brief 向红黑树中插入数据
 * @param k 键
 * @param v 值
 * @return 成功返回新节点的iterator，失败返回end()
 */
template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::insert(const KeyType& k, const ValType& v)
{
    return emplace(k, v);
}

template <typename Key, typename Val, typename Compare>
template <typename K, typename V>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::emplace(K&& k, V&& v)
{
    detach();
    Node* newNode = mRBtree->emplace(std::forward<K>(k), std::forward<V>(v));
    return iterator(newNode, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::erase(const KeyType& k)
{
    detach();
    Node* next = mRBtree->erase(k);
    return iterator(next, mRBtree);
}

/**
 * @brief 删除iterator
 * @param it iterator对象
 * @return 返回下一节点位置，如果是删除最后一个节点位置则会返回end，如果不存此iterator也会返回end
 */
template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::erase(const iterator& it)
{
    if (mRBtree == nullptr || it.currentNode == mRBtree->end()) {
        return iterator(mRBtree ? mRBtree->end() : nullptr, mRBtree);
    }

    if (mRBtree->reference.load() > 0) {
        KeyType key = it.key();
        detach();
        Node* node = mRBtree->find(key);
        if (node == nullptr) {
            return iterator(mRBtree->end(), mRBtree);
        }
        Node* next = mRBtree->erase(node);
        return iterator(next, mRBtree);
    }

    Node* next = mRBtree->erase(it.currentNode);
    return iterator(next, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::erase(const iterator& first, const iterator& last)
{
    if (first == last) {
        return iterator(last.currentNode, last.owner);
    }

    if (mRBtree == nullptr) {
        return iterator(nullptr, nullptr);
    }

    if (mRBtree->reference.load() > 0) {
        KeyType firstKey = first.key();
        if (last.currentNode == mRBtree->end()) {
            detach();
            Node* node = mRBtree->find(firstKey);
            Node* next = mRBtree->eraseRange(node, mRBtree->end());
            return iterator(next, mRBtree);
        }

        KeyType lastKey = last.key();
        detach();
        Node* node = mRBtree->find(firstKey);
        Node* stop = mRBtree->find(lastKey);
        Node* next = mRBtree->eraseRange(node, stop ? stop : mRBtree->end());
        return iterator(next, mRBtree);
    }

    Node* node = first.currentNode;
    Node* stop = last.currentNode;
    Node* next = mRBtree->eraseRange(node, stop ? stop : mRBtree->end());
    return iterator(next, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::find(const KeyType& key)
{
    detach();

    Node* node = mRBtree->find(key);
    return iterator(node, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::const_iterator Map<Key, Val, Compare>::find(const KeyType& key) const
{
    if (mRBtree == nullptr) {
        return const_iterator(nullptr, nullptr);
    }

    const Data* data = mRBtree;
    const Node* node = data->find(key);
    return const_iterator(node, mRBtree);
}

template <typename Key, typename Val, typename Compare>
bool Map<Key, Val, Compare>::contains(const KeyType& key) const
{
    return mRBtree != nullptr && mRBtree->find(key) != nullptr;
}

template <typename Key, typename Val, typename Compare>
Val Map<Key, Val, Compare>::value(const KeyType& key, const ValType& defaultValue) const
{
    if (mRBtree == nullptr) {
        return defaultValue;
    }

    const Data* data = mRBtree;
    const Node* n = data->find(key);
    return n ? n->value : defaultValue;
}

template <typename Key, typename Val, typename Compare>
Val& Map<Key, Val, Compare>::operator[](const KeyType& key)
{
    detach();
    Node* n = mRBtree->find(key);
    if (n == nullptr) {
        n = mRBtree->insert(key, Val());
        if (n == nullptr) {
            throw Exception("Map::insert error. maybe no memory");
        }
    }

    return n->value;
}

template <typename Key, typename Val, typename Compare>
Val Map<Key, Val, Compare>::operator[](const KeyType& key) const
{
    return value(key);
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::clear()
{
    if (mRBtree == nullptr) {
        return;
    }

    if (mRBtree->reference.load() > 0) {
        mRBtree->reference.deref();
        mRBtree = nullptr;
    } else {
        mRBtree->clear();
    }
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::swap(Map<KeyType, ValType, CompareType>& other) noexcept
{
    std::swap(mRBtree, other.mRBtree);
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::merge(Map<KeyType, ValType, Compare>& other)
{
    if (this == std::addressof(other) || other.mRBtree == nullptr || other.mRBtree->size() == 0) {
        return;
    }

    other.detach();

    if (mRBtree == nullptr) {
        mRBtree = other.mRBtree;
        other.mRBtree = nullptr;
        return;
    }

    detach();

    if (mRBtree->size() == 0) {
        Data::destroy(mRBtree);
        mRBtree = other.mRBtree;
        other.mRBtree = nullptr;
        return;
    }

    for (Node* node = other.mRBtree->begin(); node != other.mRBtree->end();) {
        Node* next = other.mRBtree->nextNode(node);
        if (mRBtree->find(node->key) == nullptr) {
            Node* moved = other.mRBtree->extract(node);
            if (mRBtree->insertNode(moved) == nullptr) {
                other.mRBtree->insertNode(moved);
            }
        }
        node = next;
    }
}

}  // namespace eular

#endif  // __EULAR_UTILS_MAP_H__
