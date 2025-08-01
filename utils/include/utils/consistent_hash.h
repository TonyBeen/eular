#ifndef __HTTPD_CONSISTENT_HASH_H__
#define __HTTPD_CONSISTENT_HASH_H__

#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <functional>
#include <algorithm>

#define USE_FNV_HASH
#ifdef USE_FNV_HASH
using HashFunction = uint32_t (*)(const std::string &);
// https://en.wikipedia.org/wiki/Fowler–Noll–Vo_hash_function
static inline uint32_t FNVHash(const std::string &key)
{
    const uint32_t FNV_prime = 0x01000193; // FNV prime number
    uint32_t hash = 0x811c9dc5;            // FNV offset basis

    for (unsigned char c : key) {
        hash ^= c;         // XOR the byte into the hash
        hash *= FNV_prime; // Multiply by the FNV prime
    }

    return hash;
}
#endif

namespace eular {
template <typename T>
class ConsistentHash
{
public:
    ConsistentHash(uint32_t m_virtualNodeCount = 17) : m_virtualNodeCount(m_virtualNodeCount) {}

    /**
     * @brief 添加节点
     * 
     * @param key 真实节点的键
     * @param value 节点的值
     */
    void insertNode(std::string key, T *value)
    {
        for (uint32_t i = 0; i < m_virtualNodeCount; ++i) {
            std::string virtualNode = key + "#" + std::to_string(i);
            uint32_t hash = m_hashFunction(virtualNode);
            m_nodeMap.emplace(hash, value);
        }
    }

    /**
     * @brief 获取真实节点
     * 
     * @param key 关键值, 非真实节点的键, 如文件名, IP
     * @return T* 返回真实节点的值
     */
    T *getNode(const std::string &key)
    {
        if (m_nodeMap.empty()) {
            throw std::runtime_error("No node in the hash ring");
        }

        uint32_t hash = m_hashFunction(key);
        // 在 hash ring 中找到第一个大于等于该值的节点
        auto it = m_nodeMap.lower_bound(hash);
        // 如果沒有找到，返回第一个节点
        if (it == m_nodeMap.end()) {
            it = m_nodeMap.begin();
        }

        return it->second;
    }

    /**
     * @brief 移除节点
     * 
     * @param key 真实节点的键
     */
    void removeNode(std::string key)
    {
        for (uint32_t i = 0; i < m_virtualNodeCount; ++i) {
            std::string virtualNode = key + "#" + std::to_string(i);
            uint32_t hash = m_hashFunction(virtualNode);
            m_nodeMap.erase(hash);
        }
    }

private:
    std::map<uint32_t, T *> m_nodeMap;          // 真实节点与虚拟节点混合Map hahs <--> node
    uint32_t                m_virtualNodeCount; // 每个真实节点的虚拟节点

#ifdef USE_FNV_HASH
    HashFunction            m_hashFunction = FNVHash;
#else
    std::hash<std::string>  m_hashFunction;
#endif
};

} // namespace eular

#endif /*__HTTPD_CONSISTENT_HASH_H__*/
