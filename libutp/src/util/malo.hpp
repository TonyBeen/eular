/*************************************************************************
    > File Name: malo.hpp
    > Author: eular
    > Brief: Fast allocator for fixed-sized objects.
    > Created Time: Tue 09 Dec 2025 04:31:30 PM CST
 ************************************************************************/

#ifndef __MALO_HPP__
#define __MALO_HPP__

#include <stdint.h>
#include <errno.h>
#include <assert.h>

#include <vector>
#include <unordered_map>
#include <stack>
#include <memory>

namespace eular {
namespace utp {
template<typename T>
class Malo {
public:
    /**
     * 构造函数
     * @param block_size 每轮分配块的对象数量
     */
    explicit Malo(size_t block_size = 128)
        : m_blockSize(block_size)
    {
        allocateBlock(); // 先分配一块
    }

    Malo(const Malo&) = delete;
    Malo& operator=(const Malo&) = delete;

    ~Malo()
    {
        for ( ; m_free.size(); ) {
            m_free.pop();
        }
#ifndef NDEBUG
        for (const auto &pair : m_objsMap) {
            assert(pair.second == true && "All objects must be returned to Malo pool before destruction");
        }
        m_objsMap.clear();
#endif

        for (auto block : m_blocks) {
            delete[] block;
        }
        m_blocks.clear();
    }

    T* get()
    {
        if (m_free.empty()) {
            allocateBlock();
        }

        if (m_free.empty()) {
            errno = ENOMEM;
            return nullptr;
        }

        T *ptr = m_free.top();
#ifndef NDEBUG
        auto it = m_objsMap.find(ptr);
        assert(it != m_objsMap.end() && "Get object must be from this Malo pool");
        assert(it->second == true && "Get object must be free in Malo pool");
        it->second = false;
#endif
        m_free.pop();
        return ptr;
    }

    void put(T* obj)
    {
#ifndef NDEBUG
        auto it = m_objsMap.find(obj);
        assert(it != m_objsMap.end() && "Put object must be from this Malo pool");
        assert(it->second == false && "Put object must be allocated in Malo pool");
        it->second = true;
#endif
        m_free.push(obj);
    }

    size_t available() const {
        return m_free.size();
    }

    size_t totalAllocated() const {
        return m_blocks.size() * m_blockSize;
    }

    size_t blockSize() const {
        return m_blockSize;
    }

    size_t blocks() const {
        return m_blocks.size();
    }

private:
    void allocateBlock()
    {
        T* block = new (std::nothrow) T[m_blockSize];
        if (!block) {
            return;
        }

        m_blocks.push_back(block);
        for (size_t i = 0; i < m_blockSize; ++i) {
            T* ptr = block + i;
            m_free.push(ptr);
#ifndef NDEBUG
            m_objsMap.emplace(ptr, true);
#endif
        }
    }

    std::vector<T*>     m_blocks; // 持有所有分配过的块用于delete[]
    std::stack<T*>      m_free; // 空闲对象栈, 保持cache高命中
    size_t              m_blockSize;

#ifndef NDEBUG
    std::unordered_map<T*, bool> m_objsMap; // 用于调试时验证put的对象合法性 T* <--> is_free
#endif
};

} // namespace utp
} // namespace eular

#endif // __MALO_HPP__
