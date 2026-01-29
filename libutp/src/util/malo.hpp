/*************************************************************************
    > File Name: malo.hpp
    > Author: eular
    > Brief: Fast allocator for fixed-sized objects
    > Created Time: Tue 09 Dec 2025 04:31:30 PM CST
 ************************************************************************/

#ifndef __MALO_HPP__
#define __MALO_HPP__

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cassert>
#include <new>
#include <vector>
#include <map>
#include <utils/alloc.h>

namespace eular {
namespace utp {
// 统计信息
struct MaloStats {
    size_t totalGets;       // 总获取次数
    size_t totalPuts;       // 总归还次数
    size_t peakUsage;       // 峰值使用量
    size_t currentUsage;    // 当前使用量
    size_t totalSlots;      // 总槽位数
    size_t freeSlots;       // 空闲槽位数
    size_t blockCount;      // 块数量
    size_t totalMemory;     // 总内存占用（字节）
    size_t allocFailures;   // 分配失败次数

    MaloStats()
        : totalGets(0), totalPuts(0), peakUsage(0), currentUsage(0),
          totalSlots(0), freeSlots(0), blockCount(0), totalMemory(0),
          allocFailures(0) {}

    void reset() {
        totalGets = 0;
        totalPuts = 0;
        peakUsage = 0;
        currentUsage = 0;
        totalSlots = 0;
        freeSlots = 0;
        blockCount = 0;
        totalMemory = 0;
        allocFailures = 0;
    }
};

struct MaloStatsEmpty {
    void reset() {}
};

template<typename T, size_t Alignment = 0, bool EnableStats = false>
class Malo;

// 辅助类型用于选择统计成员
template <bool Enable>
struct MaloStatsSelector {
    typedef MaloStats type;
};

template <>
struct MaloStatsSelector<false> {
    typedef MaloStatsEmpty type;
};

/**
 * @brief 高性能固定大小对象内存池
 *
 * @tparam T 对象类型
 * @tparam Alignment 内存对齐字节数 (0 表示使用默认对齐)
 * @tparam EnableStats 是否启用统计
 */
template<typename T, size_t Alignment, bool EnableStats>
class Malo {
public:
    static size_t actualAlign() {
        return Alignment > 0 ? Alignment : alignof(T);
    }

    static size_t minSlotSize() {
        return sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
    }

    static size_t slotSize() {
        size_t a = actualAlign();
        return (minSlotSize() + a - 1) & ~(a - 1);
    }

private:
    // 内存块头部
    struct Block {
        Block*  next;
        size_t  slotCount;
        size_t  memorySize;
    };

    struct FreeNode {
        FreeNode* next;
        Block*    owner; // 指向所属 Block
    };

    typedef typename MaloStatsSelector<EnableStats>::type StatsT;

public:
    /**
     * @brief 构造函数
     * @param blockSize 每个块中的对象数量
     */
    explicit Malo(size_t blockSize = 128)
        : m_blockSize(blockSize > 0 ? blockSize : 128)
        , m_blocks(nullptr)
        , m_freeList(nullptr)
        , m_freeCount(0)
        , m_totalSlots(0)
        , m_blockCount(0)
        , m_totalMemory(0) {
        m_stats.reset();
        allocateBlock();
    }

    ~Malo() {
#ifndef NDEBUG
        if (m_freeCount != m_totalSlots) {
            assert(false && "Memory leak: not all objects returned to pool");
        }
#endif
        freeAllBlocks();
    }

    Malo(const Malo&) = delete;
    Malo& operator=(const Malo&) = delete;

    Malo(Malo&& other) {
        moveFrom(other);
    }
    Malo& operator=(Malo&& other) {
        if (this != &other) {
            freeAllBlocks();
            moveFrom(other);
        }
        return *this;
    }

    /**
     * @brief 获取一个对象
     * @return 对象指针, 失败返回 nullptr
     */
    T* get() {
        if (!m_freeList) {
            if (!allocateBlock()) {
                record_alloc_fail();
                errno = ENOMEM;
                return nullptr;
            }
        }

        // 从空闲链表头部取出
        FreeNode* node = m_freeList;
        m_freeList = node->next;
        --m_freeCount;
        record_get();
        return reinterpret_cast<T*>(node);
    }

    /**
     * @brief 归还一个对象
     * @param obj 之前从 get() 获取的指针
     */
    void put(T* obj) {
        if (!obj) return;
#ifndef NDEBUG
        assert(owns(obj) && "Object does not belong to this pool");
        assert(!isFree(obj) && "Double free detected");
#endif
        record_put();
        FreeNode* node = reinterpret_cast<FreeNode*>(obj);
        node->next = m_freeList;
        m_freeList = node;
        ++m_freeCount;
    }

    /**
     * @brief 批量获取对象
     * @param out 输出数组
     * @param count 请求数量
     * @return 实际获取的数量
     */
    size_t getBatch(std::vector<T *> &out, size_t count) {
        size_t got = 0;
        for (; got < count; ++got) {
            T* obj = get();
            if (!obj) {
                break;
            }
            out.push_back(obj);
        }
        return got;
    }

    /**
     * @brief 批量归还对象
     * @param objs 对象数组
     * @param count 数量
     */
    void putBatch(const std::vector<T *> &objs) {
        for (size_t i = 0; i < objs.size(); ++i) {
            put(objs[i]);
        }
    }

    /**
     * @brief 预分配块
     * @param numBlocks 块数量
     * @return 成功分配的块数
     */
    size_t reserve(size_t numBlocks) {
        size_t allocated = 0;
        for (size_t i = 0; i < numBlocks; ++i) {
            if (allocateBlock()) {
                ++allocated;
            } else {
                break;
            }
        }
        return allocated;
    }

    /**
     * @brief 收缩内存, 释放完全空闲的块（保留至少一个块）
     * @return 释放的块数量
     */
    size_t shrink() {
        if (m_blockCount <= 1) return 0;

        // 统计每个 block 的空闲节点数
        std::map<Block*, size_t> freePerBlock;
        for (FreeNode* node = m_freeList; node; node = node->next) {
            freePerBlock[node->owner]++;
        }

        size_t freed = 0;
        Block** pprev = &m_blocks;
        while (*pprev) {
            Block* block = *pprev;
            size_t blockFree = freePerBlock[block];
            if (blockFree == block->slotCount && m_blockCount - freed > 1) {
                // 从 freeList 移除属于该 block 的节点
                FreeNode** pNode = &m_freeList;
                while (*pNode) {
                    if ((*pNode)->owner == block) {
                        *pNode = (*pNode)->next;
                    } else {
                        pNode = &((*pNode)->next);
                    }
                }
                // 移除block
                *pprev = block->next;
                size_t a = actualAlign();
                if (a > alignof(std::max_align_t)) {
                    AlignedFree(block);
                } else {
                    std::free(block);
                }
                m_freeCount -= block->slotCount;
                m_totalSlots -= block->slotCount;
                m_totalMemory -= block->memorySize;
                --m_blockCount;
                ++freed;
            } else {
                pprev = &(*pprev)->next;
            }
        }
        return freed;
    }

    /**
     * @brief 重置池
     */
    void reset() {
        freeAllBlocks();
        m_stats.reset();
        allocateBlock();
    }

    size_t available() const { return m_freeCount; }
    size_t capacity() const { return m_totalSlots; }
    size_t inUse() const { return m_totalSlots - m_freeCount; }
    size_t blockSize() const { return m_blockSize; }
    size_t blockCount() const { return m_blockCount; }
    size_t totalMemory() const { return m_totalMemory; }
    static size_t static_slotSize() { return slotSize(); }
    static size_t static_alignment() { return actualAlign(); }

    /**
     * @brief 检查指针是否属于此池
     */
    bool owns(const T* ptr) const {
        if (!ptr) return false;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(ptr);
        for (Block* block = m_blocks; block; block = block->next) {
            const unsigned char* slotsStart = getSlotsStart(block);
            const unsigned char* slotsEnd = slotsStart + block->slotCount * slotSize();
            if (p >= slotsStart && p < slotsEnd) {
                size_t offset = p - slotsStart;
                return (offset % slotSize()) == 0;
            }
        }
        return false;
    }

    MaloStats getStats() const {
        MaloStats stats;
        copy_stats(stats);
        stats.currentUsage = m_totalSlots - m_freeCount;
        stats.totalSlots = m_totalSlots;
        stats.freeSlots  = m_freeCount;
        stats.blockCount = m_blockCount;
        stats.totalMemory = m_totalMemory;
        return stats;
    }

    void resetStats() { m_stats.reset(); }

    /**
     * @brief 打印统计信息到缓冲区
     * @param buf 输出缓冲区
     * @param size 缓冲区大小
     * @return 写入的字符数
     */
    int formatStats(char* buf, size_t size) const {
        MaloStats s = getStats();
        return std::snprintf(buf, size,
            "Malo<%zu>: slots=%zu/%zu, blocks=%zu, mem=%zuKB",
            sizeof(T), s.currentUsage, s.totalSlots, 
            s.blockCount, s.totalMemory / 1024);
    }

private:
    bool allocateBlock() {
        size_t a = actualAlign();
        size_t headerSize = (sizeof(Block) + a - 1) & ~(a - 1);
        size_t slotsSize = m_blockSize * slotSize();
        size_t totalSize = headerSize + slotsSize;
        void* mem;
        if (a > alignof(std::max_align_t)) {
            mem = AlignedAlloc(totalSize, a);
        } else {
            mem = std::malloc(totalSize);
        }
        if (!mem) return false;
        Block* block = static_cast<Block*>(mem);
        block->slotCount = m_blockSize;
        block->memorySize = totalSize;
        block->next = m_blocks;
        m_blocks = block;
        unsigned char* slotsStart = reinterpret_cast<unsigned char*>(block) + headerSize;

        for (size_t i = m_blockSize; i > 0; --i) {
            FreeNode* node = reinterpret_cast<FreeNode*>(slotsStart + (i - 1) * slotSize());
            node->next = m_freeList;
            node->owner = block;
            m_freeList = node;
        }
        m_freeCount += m_blockSize;
        m_totalSlots += m_blockSize;
        ++m_blockCount;
        m_totalMemory += totalSize;
        return true;
    }

    void freeAllBlocks() {
        Block* block = m_blocks;
        while (block) {
            Block* next = block->next;
            size_t a = actualAlign();
            if (a > alignof(std::max_align_t)) {
                AlignedFree(block);
            } else {
                std::free(block);
            }
            block = next;
        }
        m_blocks = NULL;
        m_freeList = NULL;
        m_freeCount = 0;
        m_totalSlots = 0;
        m_blockCount = 0;
        m_totalMemory = 0;
    }

    void moveFrom(Malo& other) {
        m_blockSize = other.m_blockSize;
        m_blocks = other.m_blocks;
        m_freeList = other.m_freeList;
        m_freeCount = other.m_freeCount;
        m_totalSlots = other.m_totalSlots;
        m_blockCount = other.m_blockCount;
        m_totalMemory = other.m_totalMemory;
        move_stats(other.m_stats);
        other.m_blocks = NULL;
        other.m_freeList = NULL;
        other.m_freeCount = 0;
        other.m_totalSlots = 0;
        other.m_blockCount = 0;
        other.m_totalMemory = 0;
        other.m_stats.reset();
    }

    static const unsigned char* getSlotsStart(const Block* block) {
        size_t a = actualAlign();
        size_t headerSize = (sizeof(Block) + a - 1) & ~(a - 1);
        return reinterpret_cast<const unsigned char*>(block) + headerSize;
    }

#ifndef NDEBUG
    bool isFree(const T* obj) const {
        const FreeNode* target = reinterpret_cast<const FreeNode*>(obj);
        for (FreeNode* node = m_freeList; node; node = node->next) {
            if (node == target) return true;
        }
        return false;
    }
#endif

    // 统计相关分发
    template <bool B = EnableStats>
    typename std::enable_if<B, void>::type record_get()  {
        ++m_stats.totalGets;
        size_t usage = m_totalSlots - m_freeCount;
        if (usage > m_stats.peakUsage) m_stats.peakUsage = usage;
    }
    template <bool B = EnableStats>
    typename std::enable_if<!B, void>::type record_get() {}

    template <bool B = EnableStats>
    typename std::enable_if<B, void>::type record_put() { ++m_stats.totalPuts; }
    template <bool B = EnableStats>
    typename std::enable_if<!B, void>::type record_put() {}

    template <bool B = EnableStats>
    typename std::enable_if<B, void>::type record_alloc_fail() { ++m_stats.allocFailures; }
    template <bool B = EnableStats>
    typename std::enable_if<!B, void>::type record_alloc_fail() {}

    template <bool B = EnableStats>
    typename std::enable_if<B, void>::type copy_stats(MaloStats& dst) const {
        dst = m_stats;
    }
    template <bool B = EnableStats>
    typename std::enable_if<!B, void>::type copy_stats(MaloStats& dst) const {
        dst.reset();
    }

    template <bool B = EnableStats>
    typename std::enable_if<B, void>::type move_stats(StatsT& other_stats) {
        m_stats = other_stats;
    }
    template <bool B = EnableStats>
    typename std::enable_if<!B, void>::type move_stats(StatsT&) {}

private:
    size_t      m_blockSize;
    Block*      m_blocks;
    FreeNode*   m_freeList;
    size_t      m_freeCount;
    size_t      m_totalSlots;
    size_t      m_blockCount;
    size_t      m_totalMemory;
    StatsT      m_stats;
};

// 基础版本
#ifndef NDEBUG
template<typename T>
using MaloBasic = Malo<T, 0, true>;

// 16字节对齐（SIMD友好）
template<typename T, bool EnableStats = true>
using MaloSimd = Malo<T, 16, EnableStats>;

// 缓存行对齐版本（64字节）
template<typename T, bool EnableStats = true>
using MaloCacheLine = Malo<T, 64, EnableStats>;
// 页对齐版本
template<typename T, bool EnableStats = true>
using MaloPageAligned = Malo<T, 4096, EnableStats>;

#else
template<typename T>
using MaloBasic = Malo<T, 0, false>;

// 16字节对齐（SIMD友好）
template<typename T, bool EnableStats = false>
using MaloSimd = Malo<T, 16, EnableStats>;

// 缓存行对齐版本（64字节）
template<typename T, bool EnableStats = false>
using MaloCacheLine = Malo<T, 64, EnableStats>;

// 页对齐版本
template<typename T, bool EnableStats = false>
using MaloPageAligned = Malo<T, 4096, EnableStats>;

#endif

} // namespace utp
} // namespace eular

#endif // __MALO_HPP__
