/*************************************************************************
    > File Name: bimap.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年03月25日 星期一 09时35分36秒
 ************************************************************************/

#ifndef __EULAR_UTILS_BIMAP_H__
#define __EULAR_UTILS_BIMAP_H__

#include <string>
#include <initializer_list>
#include <type_traits>
#include <exception>
#include <stdexcept>

#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
#include <map>
#include <unordered_map>
#else
#include <utils/map.h>
#include <utils/hash.h>
#endif

namespace eular {
template <typename K, typename V, typename CompareK = std::less<K>, typename HashV = std::hash<V>>
class BiMap
{
private:
    struct ValuePtrHash {
        size_t operator()(const V *valuePtr) const {
            return HashV()(*valuePtr);
        }
    };

    struct ValuePtrEqual {
        bool operator()(const V *lhs, const V *rhs) const {
            return std::equal_to<V>()(*lhs, *rhs);
        }
    };

public:
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
    using MapStorage     = std::map<K, V, CompareK>;
    using MapIterator    = typename MapStorage::iterator;
    using HashMapStorage = std::unordered_map<const V *, MapIterator, ValuePtrHash, ValuePtrEqual>;
#else
    using MapStorage     = Map<K, V, CompareK>;
    using MapIterator    = typename MapStorage::iterator;
    using HashMapStorage = HashMap<const V *, MapIterator, ValuePtrHash, ValuePtrEqual>;
#endif

    static_assert(std::is_same<typename std::result_of<HashV(V)>::type, std::size_t>::value,
                  "HashV must be a function object type that takes a V and returns a size_t");

    static_assert(!std::is_same<K, void>::value, "K cannot be void");
    static_assert(!std::is_same<V, void>::value, "V cannot be void");
    static_assert(!std::is_same<K, V>::value, "Key and value types cannot be the same");

    BiMap() = default;
    BiMap(const std::initializer_list<std::pair<K, V>> &list);
    BiMap(const BiMap &other);
    BiMap(BiMap &&other);
    ~BiMap();

    BiMap &operator=(const BiMap &other);
    BiMap &operator=(BiMap &&other);

    friend class iterator;
    class iterator {
        iterator(MapIterator other, BiMap<K, V, CompareK, HashV> *parent) :
            m_it(other),
            m_parent(parent)
        {
        }

        iterator(BiMap<K, V, CompareK, HashV> *parent) :
            m_parent(parent)
        {
            m_it = m_parent->m_storage.end();
        }

    public:
        iterator() = default;

        iterator(const iterator &other) :
            m_it(other.m_it),
            m_parent(other.m_parent)
        {
        }

        inline bool operator==(const iterator &o) const { return m_it == o.m_it; }
        inline bool operator!=(const iterator &o) const { return m_it != o.m_it; }

        inline iterator &operator++() {
            ++m_it;
            return *this;
        }

        inline iterator operator++(int) {
            iterator it = *this;
            ++m_it;
            return it;
        }

        inline iterator &operator--() {
            --m_it;
            return *this;
        }

        inline iterator operator--(int) {
            iterator it = *this;
            --m_it;
            return it;
        }

        inline const K &key() const
        {
            checkIterator();
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
            return m_it->first;
#else
            return m_it.key();
#endif
        }

        inline const V &value() const
        {
            checkIterator();
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
            return m_it->second;
#else
            return m_it.value();
#endif
        }

        // NOTE 遍历过程中更新 key 会改变迭代顺序, 可能导致遍历结果不可预期 (跳过或重复访问)
        bool update(const K &key)
        {
            checkIterator();

            auto newKeyIt = m_parent->find(key);
            if (newKeyIt != m_parent->end()) {
                return false;
            }

#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
            V value = m_it->second;
#else
            V value = m_it.value();
#endif
            m_parent->erase(m_it);
            bool result = m_parent->insert(key, value);
            if (!result) {
                m_it = m_parent->end();
                return result;
            }

            *this = m_parent->find(key);
            return result;
        }

        bool update(const V &value)
        {
            checkIterator();

#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
            if (m_it->second == value) {
                return true;
            }
#else
            if (m_it.value() == value) {
                return true;
            }
#endif

            auto foundIt = m_parent->m_indexMap.find(&value);
            if (foundIt != m_parent->m_indexMap.end()) { // 新值已存在
                return false;
            }

            // 更新indexMap的数据
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
            const V *oldValuePtr = &(m_it->second);
            m_parent->eraseValueIndex(oldValuePtr);
            m_it->second = value;
            m_parent->insertValueIndex(&(m_it->second), m_it);
#else
            const V *oldValuePtr = &(m_it.value());
            m_parent->eraseValueIndex(oldValuePtr);
            m_it.value() = value;
            m_parent->insertValueIndex(&(m_it.value()), m_it);
#endif
            return true;
        }

    private:
        inline void checkIterator() const {
            if (m_it == m_parent->m_storage.end()) {
                throw std::logic_error("The iterator reached the end");
            }
        }

    private:
        friend class BiMap<K, V, CompareK, HashV>;
        MapIterator m_it;
        BiMap<K, V, CompareK, HashV> *m_parent;
    };

    /**
     * @brief 插入键为key, 值为value的数据
     * 
     * @param key 
     * @param value 
     * @return true 成功插入
     * @return false 容器中已存在相同的key或value
     */
    bool insert(const K &key, const V &value);

    /**
     * @brief 将键为key的值替换为value, 不存在key将视作插入, 已存在相同value时将抛出异常
     * @note 循环过程中更新会产生未定义行为
     * 
     * @param key 键
     * @param value 新值
     */
    void replaceValue(const K &key, const V &value);

    /**
     * @brief 将值为value的键替换为key, 不存在value将视作插入, 已存在相同key时将抛出异常
     * @note 循环过程中更新会产生未定义行为
     * 
     * @param value 值
     * @param key 新的键
     */
    void replaceKey(const V &value, const K &key);

    iterator erase(const K &key);
    iterator erase(const V &value);
    iterator erase(iterator it);

    iterator find(const K &key);
    iterator find(const V &value);

    iterator begin();
    iterator end();

    bool contains(const K &key) const;
    bool contains(const V &value) const;

    size_t size() const;

    bool empty() const;
    void clear();

private:
    inline void insertValueIndex(const V *valuePtr, const MapIterator &it)
    {
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
        m_indexMap.emplace(valuePtr, it);
#else
        m_indexMap.insert(valuePtr, it);
#endif
    }

    inline void eraseValueIndex(const V *valuePtr)
    {
#if defined(UTILS_BIMAP_USE_STD_CONTAINER) && (UTILS_BIMAP_USE_STD_CONTAINER)
        m_indexMap.erase(valuePtr);
#else
        auto it = m_indexMap.find(valuePtr);
        if (it != m_indexMap.end()) {
            m_indexMap.erase(it);
        }
#endif
    }

private:
    void erase(MapIterator it);

private:
    HashV           HashFunctionV;
    // NOTE 反向索引：value pointer -> 主存储迭代器
    MapStorage      m_storage;
    HashMapStorage  m_indexMap;
};

} // namespace eular

#include <utils/bimap.hpp>

#endif // __EULAR_UTILS_BIMAP_H__
