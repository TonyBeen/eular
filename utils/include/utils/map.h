/*************************************************************************
    > File Name: map.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 24 Nov 2022 04:29:18 PM CST
 ************************************************************************/

#include <assert.h>
#include <initializer_list>

#include <utils/map_node.h>
#include <utils/exception.h>

namespace eular {
template<typename Key, typename Val>
class Map {
    typedef Key KeyType;
    typedef Val ValType;
    using Data = detail::MapData<KeyType, ValType>;
    using Node = detail::MapNode<KeyType, ValType>;

public:
    Map() : mRBtree(Data::create()) {}
    Map(std::initializer_list<std::pair<KeyType, ValType>> initList) :
        mRBtree(Data::create())
    {
        typename std::initializer_list<std::pair<KeyType, ValType> >::const_iterator it;
        for (it = initList.begin(); it != initList.end(); ++it)
        {
            mRBtree->insert(it->first, it->second);
        }
    }

    Map(const Map<KeyType, ValType> &other) :
        mRBtree(nullptr)
    {
        mRBtree = other.mRBtree;
        assert(this != &other);
        if (this != &other) {
            mRBtree->reference.ref();
        }
    }

    Map(Map<KeyType, ValType> &&other) :
        mRBtree(nullptr)
    {
        mRBtree = other.mRBtree;
        other.mRBtree = Data::create();
    }

    ~Map()
    {
        destroy();
    }

    Map &operator=(const Map<KeyType, ValType> &other)
    {
        if (this == &other) {
            return *this;
        }

        // TODO: 当声明一个map时会先在堆上创建MapData，此时在赋值一个其他的map，会导致之前的MapData释放内存，存在性能问题
        destroy();
        mRBtree = other.mRBtree;
        mRBtree->reference.ref();
        return *this;
    }

    Map &operator=(Map<KeyType, ValType> &&other)
    {
        if (this == &other) {
            return *this;
        }
        std::swap(mRBtree, other.mRBtree);
        return *this;
    }

    class const_iterator;
    class iterator {
        friend class Map<Key, Val>;
        friend class const_iterator;
    public:
        iterator() : currentNode(nullptr) { }
        iterator(Node *n) : currentNode(n) { }

        inline const KeyType &key() const { return currentNode->key; }
        inline ValType &value() const { return currentNode->value; }
        inline ValType *operator->() const { return &currentNode->value; }
        inline bool operator==(const iterator &o) const { return currentNode == o.currentNode; }
        inline bool operator!=(const iterator &o) const { return currentNode != o.currentNode; }
        inline bool operator==(const const_iterator &o) const { return currentNode == o.currentNode; }
        inline bool operator!=(const const_iterator &o) const { return currentNode != o.currentNode; }

        inline iterator &operator++() { // 前置++
            currentNode = currentNode->nextNode();
            return *this;
        }

        inline iterator operator++(int) { // 后置++
            iterator it = *this;
            currentNode = currentNode->nextNode();
            return it;
        }

        inline iterator &operator--() {
            currentNode = currentNode->previousNode();
            return *this;
        }

        inline iterator operator--(int) {
            iterator it = *this;
            currentNode = currentNode->previousNode();
            return it;
        }
    
    private:
        Node *currentNode;
    };
    friend class iterator;

    class const_iterator {
        friend class Map<Key, Val>;
        friend class iterator;
    public:
        const_iterator() : currentNode(nullptr) { }
        const_iterator(const iterator &o) : currentNode(o.currentNode) { }
        const_iterator(const Node *n) : currentNode(n) { }

        inline const KeyType &key() const { return currentNode->key; }
        inline const ValType &value() const { return currentNode->value; }
        inline const ValType *operator->() const { return &currentNode->value; }
        inline bool operator==(const const_iterator &o) const { return currentNode == o.currentNode; }
        inline bool operator!=(const const_iterator &o) const { return currentNode != o.currentNode; }

        inline const_iterator &operator++() { // 前置++
            currentNode = currentNode->nextNode();
            return *this;
        }

        inline const_iterator operator++(int) { // 后置++
            const_iterator it = *this;
            currentNode = currentNode->nextNode();
            return it;
        }

        inline const_iterator &operator--() {
            currentNode = currentNode->previousNode();
            return *this;
        }

        inline const_iterator operator--(int) {
            const_iterator it = *this;
            currentNode = currentNode->previousNode();
            return it;
        }

    private:
        const Node *currentNode;
    };
    friend class const_iterator;

    /* 返回iterator对红黑树做detach操作是因为iterator可以修改变量，会导致拷贝的那份也修改 */
    inline iterator begin() { detach(); return iterator(mRBtree->begin()); }
    inline const_iterator begin() const { return const_iterator(mRBtree->begin()); }
    // inline iterator rbegin() { detach(); return iterator(mRBtree->rbegin()); }
    inline const_iterator rbegin() const { return const_iterator(mRBtree->rbegin()); }
    inline iterator end() { detach(); return iterator(mRBtree->end()); }
    inline const_iterator end() const { return const_iterator(mRBtree->end()); }
    // inline iterator rend() { detach(); return iterator(mRBtree->rend()); }
    inline const_iterator rend() const { return const_iterator(mRBtree->rend()); }

    iterator insert(const KeyType &k, const ValType &v);
    // FIXME: 对于rbegin反向循环中使用erase会出现问题, 目前做法是禁用反向循环中的擦除行为
    iterator erase(const KeyType &k);
    iterator erase(const iterator &it);
    iterator find(const KeyType &key);
    const_iterator find(const KeyType &key) const;
    const ValType &value(const KeyType &key, const ValType &defaultValue = ValType()) const;
    ValType &operator[](const KeyType &key);
    const ValType &operator[](const KeyType &key) const;
    void clear();
    size_t size() const { return mRBtree->size(); }

protected:
    void detach();
    bool isDetached() const { return mRBtree->reference.load() == 0; }
    void detach_helper();
    void destroy();

private:
    detail::MapData<KeyType, ValType> *mRBtree;
};

template<typename Key, typename Val>
void Map<Key, Val>::detach()
{
    if (mRBtree && mRBtree->reference.load() > 0) {
       detach_helper();
    } else if (mRBtree == nullptr) {
        mRBtree = Data::create();
    }
}

template<typename Key, typename Val>
void Map<Key, Val>::detach_helper()
{
    Data *data = Data::create();
    mRBtree->reference.deref();
    detail::MapNode<Key, Val> *it = nullptr;
    // QT源码采用自拷贝，但是当节点过多时可能会造成栈溢出，所以在此选择循环
    for (it = mRBtree->begin(); it != mRBtree->end(); it = mRBtree->nextNode(it)) {
        data->insert(it->key, it->value);
    }

    mRBtree = data;
}

template<typename Key, typename Val>
void Map<Key, Val>::destroy()
{
    if (mRBtree) {
        if (mRBtree->reference.load() > 0) {
            mRBtree->reference.deref();
        } else {
            mRBtree->clear();
            detail::MapDataBase::FreeData(mRBtree);
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
template<typename Key, typename Val>
typename Map<Key, Val>::iterator Map<Key, Val>::insert(const KeyType &k, const ValType &v)
{
    detach();
    Node *newNode = mRBtree->insert(k, v);
    return iterator(newNode);
}

template<typename Key, typename Val>
typename Map<Key, Val>::iterator Map<Key, Val>::erase(const KeyType &k)
{
    detach();
    Node *next = mRBtree->erase(k);
    return iterator(next);
}

/**
 * @brief 删除iterator
 * @param it iterator对象
 * @return 返回下一节点位置，如果是删除最后一个节点位置则会返回end，如果不存此iterator也会返回end
 */
template<typename Key, typename Val>
typename Map<Key, Val>::iterator Map<Key, Val>::erase(const iterator &it)
{
    detach();
    Node *next = mRBtree->erase(it.currentNode, false);
    return iterator(next);
}

template<typename Key, typename Val>
typename Map<Key, Val>::iterator Map<Key, Val>::find(const KeyType &key)
{
    detach();
    Node *node = mRBtree->find(key);
    return iterator(node);
}

template<typename Key, typename Val>
typename Map<Key, Val>::const_iterator Map<Key, Val>::find(const KeyType &key) const
{
    Node *node = mRBtree->find(key);
    return const_iterator(node);
}

template<typename Key, typename Val>
const Val &Map<Key, Val>::value(const KeyType &key, const ValType &defaultValue) const
{
    Node *n = mRBtree->find(key);
    return n ? n->value : defaultValue;
}

template<typename Key, typename Val>
Val &Map<Key, Val>::operator[](const KeyType &key)
{
    detach();
    Node *n = mRBtree->find(key);
    if (n == nullptr) {
        n = mRBtree->insert(key, Val());
        if (n == nullptr) {
            throw Exception("Map::insert error. maybe no memory");
        }
    }

    return n->value;
}

template<typename Key, typename Val>
const Val &Map<Key, Val>::operator[](const KeyType &key) const
{
    return value(key);
}

template<typename Key, typename Val>
void Map<Key, Val>::clear()
{
    if (mRBtree == nullptr) {
        mRBtree = detail::MapData<Key, Val>::create();
        return;
    }

    if (mRBtree->reference.load() > 0) {
        mRBtree->reference.deref();
        mRBtree = detail::MapData<Key, Val>::create();
    } else {
        mRBtree->clear();
    }
}

} // namespace eular
