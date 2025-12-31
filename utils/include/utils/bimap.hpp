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
    m_indexMap.reserve(m_storage.size());
    for (auto it = m_storage.begin(); it != m_storage.end(); ++it)
    {
        auto hashV = HashFunctionV(it->second);
        m_indexMap[hashV] = const_cast<K *>(&(it->first));
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
        for (auto it = m_storage.begin(); it != m_storage.end(); ++it)
        {
            auto hashV = HashFunctionV(it->second);
            m_indexMap[hashV] = const_cast<K *>(&(it->first));
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
    auto hashV = HashFunctionV(value);
    auto found = m_indexMap.find(hashV);
    if (found != m_indexMap.end()) {
        // value已存在
        return false;
    }

    auto retPair = m_storage.insert(std::make_pair(key, value));
    if (!retPair.second) {
        // key已存在
        return false;
    }

    K *pKey = const_cast<K *>(&(retPair.first->first));
    m_indexMap[hashV] = pKey;
    return true;
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::replaceValue(const K &key, const V &value)
{
    auto newHashV = HashFunctionV(value);
    auto foundIt = m_indexMap.find(newHashV);
    if (foundIt != m_indexMap.end())
    {
        throw std::logic_error("The same value already exists!");
    }

    auto keyIt = m_storage.find(key);
    if (keyIt == m_storage.end())
    {
        // 不存在时则按插入处理
        this->insert(key, value);
        return;
    }

    auto oldHashV = HashFunctionV(keyIt->second);
    m_indexMap.erase(oldHashV);
    m_indexMap[newHashV] = const_cast<K *>(&(keyIt->first));
}

template <typename K, typename V, typename CompareK, typename HashV>
void BiMap<K, V, CompareK, HashV>::replaceKey(const V &value, const K &key)
{
    auto foundIt = m_storage.find(key);
    if (foundIt != m_storage.end())
    {
        throw std::logic_error("The same key already exists!");
    }

    auto hashV = HashFunctionV(value);
    auto indexIt = m_indexMap.find(hashV);
    if (indexIt == m_indexMap.end())
    {
        // 不存在时则按插入处理
        this->insert(key, value);
        return;
    }

    m_storage.erase(*(indexIt->second));
    auto resultPair = m_storage.insert(std::make_pair(key, value));
    if (!resultPair.second)
    {
        // 未知的情况会进入此处
        throw std::logic_error("The same key already exists!");
    }

    K *pKey = const_cast<K *>(&(resultPair.first->first));
    m_indexMap[hashV] = pKey;
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(const K &key)
{
    auto keyIt = m_storage.find(key);
    if (keyIt == m_storage.end())
    {
        return iterator(this);
    }

    auto hashV = HashFunctionV(keyIt->second);
    m_indexMap.erase(hashV);

    auto nextIt = m_storage.erase(keyIt);
    return iterator(nextIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(const V &value)
{
    auto hashV = HashFunctionV(value);
    auto indexIt = m_indexMap.find(hashV);
    if (indexIt == m_indexMap.end())
    {
        return iterator(this);
    }

    K *pKey = indexIt->second;
    m_indexMap.erase(indexIt);
    auto eraseIt = m_storage.find(*pKey);
    auto nextIt = m_storage.erase(eraseIt);
    return iterator(nextIt, this);
}

template <typename K, typename V, typename CompareK, typename HashV>
typename BiMap<K, V, CompareK, HashV>::iterator BiMap<K, V, CompareK, HashV>::erase(iterator it)
{
    it.checkIterator();
    auto hashV = HashFunctionV(it.m_it->second);
    m_indexMap.erase(hashV);

    auto eraseIt = m_storage.find(it.m_it->first);
    auto nextIt = m_storage.erase(eraseIt);
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
    auto it = m_indexMap.find(HashFunctionV(value));
    if (it == m_indexMap.end())
    {
        return iterator(this);
    }

    auto keyIt = m_storage.find(*(it->second));
    return iterator(keyIt, this);
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
    auto it = m_indexMap.find(HashFunctionV(value));
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

    auto hashV = HashFunctionV(it->Second);
    m_storage.erase(it);
    m_indexMap.erase(hashV);
}

} // namespace eular

#endif // __EULAR_UTILS_BIMAP_HPP__
