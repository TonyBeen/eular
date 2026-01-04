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
#include <unordered_map>
#include <map>
#include <exception>
#include <stdexcept>

namespace eular {
template <typename K, typename V, typename CompareK = std::less<K>, typename HashV = std::hash<V>>
class BiMap
{
public:
    using MapStorage = typename std::map<K, V, CompareK>;
    using MapIterator = typename MapStorage::iterator;

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
            return m_it->first;
        }

        inline const V &value() const
        {
            checkIterator();
            return m_it->second;
        }

        // NOTE 循环过程中更新键会产生未定义行为
        bool update(const K &key)
        {
            checkIterator();

            auto newKeyIt = m_parent->find(key);
            if (newKeyIt != m_parent->end())
            {
                return false;
            }

            V value = m_it->second;
            m_parent->erase(m_it);
            bool result = m_parent->insert(key, value);
            if (!result)
            {
                m_it = m_parent->end();
                return result;
            }

            *this = m_parent->find(key);
            return result;
        }

        bool update(const V &value)
        {
            checkIterator();

            auto newHashV = m_parent->HashFunctionV(value);
            auto foundIt = m_parent->m_indexMap.find(newHashV);
            if (foundIt != m_parent->m_indexMap.end())
            {
                // 新值已存在
                return false;
            }

            // 更新indexMap的数据
            auto oldHashV = m_parent->HashFunctionV(m_it->second);
            m_parent->m_indexMap.erase(oldHashV);
            m_parent->m_indexMap[oldHashV] = const_cast<K *>(&(m_it->first));
            return true;
        }

    private:
        inline void checkIterator() const
        {
            if (m_it == m_parent->m_storage.end())
            {
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
    void erase(MapIterator it);

private:
    HashV                               HashFunctionV;
    // NOTE 当m_storage扩容时会导致m_indexMap记录的值全部失效, 故存储采用红黑树
    MapStorage                          m_storage;
    std::unordered_map<uint64_t, K *>   m_indexMap; // std::hash<V> -> K *
};

} // namespace eular

#include <utils/bimap.hpp>

#endif // __EULAR_UTILS_BIMAP_H__
