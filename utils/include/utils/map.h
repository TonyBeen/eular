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
    Map() : mRBtree(Data::create()) {}
    Map(std::initializer_list<std::pair<KeyType, ValType>> initList) : mRBtree(Data::create())
    {
        typename std::initializer_list<std::pair<KeyType, ValType>>::const_iterator it;
        for (it = initList.begin(); it != initList.end(); ++it) {
            mRBtree->insert(it->first, it->second);
        }
    }

    Map(const Map<KeyType, ValType, CompareType>& other) : mRBtree(nullptr)
    {
        mRBtree = other.mRBtree;
        assert(this != &other);
        if (this != &other) {
            mRBtree->reference.ref();
        }
    }

    Map(Map<KeyType, ValType, CompareType>&& other) : mRBtree(nullptr)
    {
        if (this != std::addressof(other)) {
            Data* empty = Data::create();
            mRBtree = other.mRBtree;
            other.mRBtree = empty;
        }
    }

    ~Map() { destroy(); }

    Map& operator=(const Map<KeyType, ValType, CompareType>& other)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        // TODO:
        // 当声明一个map时会先在堆上创建MapData，此时在赋值一个其他的map，会导致之前的MapData释放内存，存在性能问题
        destroy();
        mRBtree = other.mRBtree;
        mRBtree->reference.ref();
        return *this;
    }

    Map& operator=(Map<KeyType, ValType, CompareType>&& other)
    {
        if (this == std::addressof(other)) {
            return *this;
        }

        Data* empty = Data::create();
        destroy();
        mRBtree = other.mRBtree;
        other.mRBtree = empty;
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
    inline const_iterator begin() const noexcept { return const_iterator(mRBtree->begin(), mRBtree); }
    // inline iterator rbegin() { detach(); return iterator(mRBtree->rbegin()); }
    inline const_iterator rbegin() const noexcept { return const_iterator(mRBtree->rbegin(), mRBtree); }
    inline iterator       end()
    {
        detach();
        return iterator(mRBtree->end(), mRBtree);
    }
    inline const_iterator end() const noexcept { return const_iterator(mRBtree->end(), mRBtree); }
    // inline iterator rend() { detach(); return iterator(mRBtree->rend()); }
    inline const_iterator rend() const noexcept { return const_iterator(mRBtree->rend(), mRBtree); }

    iterator insert(const KeyType& k, const ValType& v);
    // FIXME: 对于rbegin反向循环中使用erase会出现问题, 目前做法是禁用反向循环中的擦除行为
    iterator       erase(const KeyType& k);
    iterator       erase(const iterator& it);
    iterator       find(const KeyType& key);
    const_iterator find(const KeyType& key) const;
    ValType        value(const KeyType& key, const ValType& defaultValue = ValType()) const;
    ValType&       operator[](const KeyType& key);
    ValType        operator[](const KeyType& key) const;
    void           clear();
    void           merge(Map<KeyType, ValType, CompareType>& other);
    size_t         size() const noexcept { return mRBtree->size(); }

protected:
    void detach();
    bool isDetached() const noexcept { return mRBtree->reference.load() == 0; }
    void detach_helper();
    void destroy();

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
void Map<Key, Val, Compare>::destroy()
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
    detach();
    Node* newNode = mRBtree->insert(k, v);
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
        Node* next = mRBtree->erase(node, true);
        return iterator(next, mRBtree);
    }

    Node* next = mRBtree->erase(it.currentNode, true);
    return iterator(next, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::iterator Map<Key, Val, Compare>::find(const KeyType& key)
{
    Node* node = mRBtree->find(key);
    return iterator(node, mRBtree);
}

template <typename Key, typename Val, typename Compare>
typename Map<Key, Val, Compare>::const_iterator Map<Key, Val, Compare>::find(const KeyType& key) const
{
    Node* node = mRBtree->find(key);
    return const_iterator(node, mRBtree);
}

template <typename Key, typename Val, typename Compare>
Val Map<Key, Val, Compare>::value(const KeyType& key, const ValType& defaultValue) const
{
    Node* n = mRBtree->find(key);
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
        mRBtree = detail::MapData<Key, Val, Compare>::create();
        return;
    }

    if (mRBtree->reference.load() > 0) {
        Data* data = detail::MapData<Key, Val, Compare>::create();
        mRBtree->reference.deref();
        mRBtree = data;
    } else {
        mRBtree->clear();
    }
}

template <typename Key, typename Val, typename Compare>
void Map<Key, Val, Compare>::merge(Map<KeyType, ValType, Compare>& other)
{
    if (this == std::addressof(other) || other.mRBtree == nullptr || other.mRBtree->size() == 0) {
        return;
    }

    if (mRBtree == nullptr) {
        mRBtree = Data::create();
    }

    detach();
    other.detach();

    if (mRBtree->size() == 0) {
        Data* empty = Data::create();
        Data::destroy(mRBtree);
        mRBtree = other.mRBtree;
        other.mRBtree = empty;
        return;
    }

    for (Node* node = other.mRBtree->begin(); node != other.mRBtree->end();) {
        Node* next = other.mRBtree->nextNode(node);
        if (mRBtree->find(node->key) == nullptr) {
            Node* moved = other.mRBtree->extract(node, false);
            if (mRBtree->insertNode(moved) == nullptr) {
                other.mRBtree->insertNode(moved);
            }
        }
        node = next;
    }
}

}  // namespace eular

#endif  // __EULAR_UTILS_MAP_H__
