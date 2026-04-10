/*************************************************************************
    > File Name: bimap.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年03月25日 星期一 09时35分49秒
 ************************************************************************/

#ifndef __EULAR_UTILS_BIMAP_HPP__
#define __EULAR_UTILS_BIMAP_HPP__

#include <utils/bimap.h>

namespace eular {
template <typename K, typename V, typename CompareK, typename HashV>
BiMap<K, V, CompareK, HashV>::BiMap(const std::initializer_list<std::pair<K, V>> &list)
{
    for (const auto &pair : list)
    {
        if (!this->insert(pair.first, pair.second))
        {
            throw std::logic_error("The same key value pair already exists!");
        }
    }
}

template <typename K, typename V, typename CompareK, typename HashV>
inline BiMap<K, V, CompareK, HashV>::BiMap(const BiMap &other) :
    m_storage(other.m_storage)
{
    m_indexMap.clear();
    m_indexMap.reserve(m_storage.size());
    for (auto it = m_storage.begin(); it != m_storage.end(); ++it)
    {
        m_indexMap.emplace(&(it->second), it);
    }
}

template<typename K, typename V, typename CompareK, typename HashV>
inline BiMap<K, V, CompareK, HashV>::BiMap(BiMap &&other)
{
    m_storage.swap(other.m_storage);
    m_indexMap.swap(other.m_indexMap);
}

template <typename K, typename V, typename CompareK, typename HashV>
BiMap<K, V, CompareK, HashV>::~BiMap()
{
    m_indexMap.clear();
    m_storage.clear();
}

template <typename K, typename V, typename CompareK, typename HashV>
BiMap<K, V, CompareK, HashV> &BiMap<K, V, CompareK, HashV>::operator=(const BiMap &other)
{
    if (this != std::addressof(other)) {
        m_storage = other.m_storage;
        m_indexMap.clear();
        m_indexMap.reserve(m_storage.size());
        for (auto it = m_storage.begin(); it != m_storage.end(); ++it)
        {
            m_indexMap.emplace(&(it->second), it);
        }
    }

    return *this;
}

template <typename K, typename V, typename CompareK, typename HashV>
BiMap<K, V, CompareK, HashV> &BiMap<K, V, CompareK, HashV>::operator=(BiMap &&other)
{
    if (this != std::addressof(other)) {
        m_storage.swap(other.m_storage);
        m_indexMap.swap(other.m_indexMap);
    }

    return *this;
}

template <typename K, typename V, typename CompareK, typename HashV>
bool BiMap<K, V, CompareK, HashV>::insert(const K &key, const V &value)
{
    auto found = m_indexMap.find(&value);
    if (found != m_indexMap.end()) {
        // value已存在
        return false;
    }

    auto retPair = m_storage.insert(std::make_pair(key, value));
    if (!retPair.second) {
        // key已存在
        return false;
    }

    m_indexMap.emplace(&(retPair.first->second), retPair.first);
    return true;
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::replaceValue(const K &key, const V &value)
{
    auto keyIt = m_storage.find(key);
    if (keyIt == m_storage.end())
    {
        // 不存在时则按插入处理
        this->insert(key, value);
        return;
    }

    if (keyIt->second == value) {
        return;
    }

    auto foundIt = m_indexMap.find(&value);
    if (foundIt != m_indexMap.end())
    {
        throw std::logic_error("The same value already exists!");
    }

    const V *oldValuePtr = &(keyIt->second);
    m_indexMap.erase(oldValuePtr);
    keyIt->second = value;
    m_indexMap.emplace(&(keyIt->second), keyIt);
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::replaceKey(const V &value, const K &key)
{
    auto foundIt = m_storage.find(key);
    if (foundIt != m_storage.end())
    {
        throw std::logic_error("The same key already exists!");
    }

    auto indexIt = m_indexMap.find(&value);
    if (indexIt == m_indexMap.end())
    {
        // 不存在时则按插入处理
        this->insert(key, value);
        return;
    }

    V oldValue = indexIt->second->second;
    m_storage.erase(indexIt->second);
    auto resultPair = m_storage.insert(std::make_pair(key, oldValue));
    if (!resultPair.second)
    {
        // 未知的情况会进入此处
        throw std::logic_error("The same key already exists!");
    }

    m_indexMap.erase(indexIt);
    m_indexMap.emplace(&(resultPair.first->second), resultPair.first);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(const K &key)
{
    auto keyIt = m_storage.find(key);
    if (keyIt == m_storage.end())
    {
        return iterator(this);
    }

    m_indexMap.erase(&(keyIt->second));

    auto nextIt = m_storage.erase(keyIt);
    return iterator(nextIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(const V &value)
{
    auto indexIt = m_indexMap.find(&value);
    if (indexIt == m_indexMap.end())
    {
        return iterator(this);
    }

    auto eraseIt = indexIt->second;
    m_indexMap.erase(indexIt);
    auto nextIt = m_storage.erase(eraseIt);
    return iterator(nextIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(iterator it)
{
    it.checkIterator();
    m_indexMap.erase(&(it.m_it->second));

    auto nextIt = m_storage.erase(it.m_it);
    return iterator(nextIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::find(const K& key)
{
    auto foundIt = m_storage.find(key);
    return iterator(foundIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::find(const V& value)
{
    auto it = m_indexMap.find(&value);
    if (it == m_indexMap.end())
    {
        return iterator(this);
    }

    return iterator(it->second, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::begin()
{
    return iterator(m_storage.begin(), this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::end()
{
    return iterator(this);
}

template <typename K, typename V, typename CompareK, typename HashV>
bool BiMap<K, V, CompareK, HashV>::contains(const K &key) const
{
    auto it = m_storage.find(key);
    return it != m_storage.end();
}

template <typename K, typename V, typename CompareK, typename HashV>
bool BiMap<K, V, CompareK, HashV>::contains(const V &value) const
{
    auto it = m_indexMap.find(&value);
    return it != m_indexMap.end();
}

template <typename K, typename V, typename CompareK, typename HashV>
size_t BiMap<K, V, CompareK, HashV>::size() const
{
    return m_storage.size();
}

template <typename K, typename V, typename CompareK, typename HashV>
bool BiMap<K, V, CompareK, HashV>::empty() const
{
    return m_storage.empty();
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::clear()
{
    m_indexMap.clear();
    m_storage.clear();
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::erase(MapIterator it)
{
    if (it == m_storage.end())
    {
        return;
    }

    auto valuePtr = &(it->second);
    m_storage.erase(it);
    m_indexMap.erase(valuePtr);
}

} // namespace eular

#endif // __EULAR_UTILS_BIMAP_HPP__
