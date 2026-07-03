/*************************************************************************
    > File Name: receive_history.h
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 03:00:59 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_RECEIVE_HISTORY_H__
#define __UTP_UTIL_RECEIVE_HISTORY_H__

#include <stdint.h>
#include <limits>
#include <vector>

#include "utp/types.h"
// ReceiveHistory 当前通过 m_maxRanges + dropOldestRange() 控制内存占用，
// 并结合 stopWait(cutoff) 裁剪已不需要出现在 ACK 中的历史包号。
namespace eular {
namespace utp {
class ReceiveHistory {
    ReceiveHistory(const ReceiveHistory&) = delete;
    ReceiveHistory& operator=(const ReceiveHistory&) = delete;
public:
    ReceiveHistory(uint32_t maxRanges = 256);
    ~ReceiveHistory() = default;

    /**
     * @brief 插入一个已接收包号
     *
     * @param pn 包号
     * @param now 当前时间, us
     */
    void insert(utp_packno_t pn, utp_time_t now);

    /**
     * @brief cutoff之前的包号不再记录在Ack帧中
     *
     * @param cutoff 包号
     */
    void stopWait(utp_packno_t cutoff);

    // 清空所有记录
    void clear();

    /**
     * @brief 返回当前最大包号
     *
     * @return utp_packno_t 包号, 无效返回0(包号不会为0)
     */
    utp_packno_t largest() const { return m_largest; }

    uint32_t rangeCount() const { return m_usedCount; }

    /**
     * @brief 返回当前最小的包号(低于这个包号的信息已丢弃)
     *
     * @return utp_packno_t 包号, 无效返回0(包号不会为0)
     */
    utp_packno_t cutoff() const { return m_cutoff; }

    /**
     * @brief 返回当前最大包号收到的时间
     *
     * @return utp_time_t 时间, 无效返回0
     */
    utp_time_t largestAckedReceived() const { return m_largestAckedReceived; }

    bool empty() const { return m_usedCount == 0; }

    class const_iterator {
    public:
        using value_type        = Range;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Range*;
        using reference         = const Range&;
        using iterator_category = std::forward_iterator_tag;

        const_iterator() = default;

        reference operator*() const  { return m_range; }
        pointer   operator->() const { return &m_range; }

        const_iterator& operator++() {
            if (!m_hist || m_index == npos) {
                return *this;
            }

            const Element &e = m_hist->m_elements[m_index];
            m_index = e.next;
            updateRange();

            return *this;
        }

        const_iterator operator++(int) {
            const_iterator tmp(*this);
            ++(*this);
            return tmp;
        }

        bool operator==(const const_iterator &other) const {
            return m_hist == other.m_hist && m_index == other.m_index;
        }

        bool operator!=(const const_iterator &other) const {
            return !(*this == other);
        }

    private:
        friend class ReceiveHistory;

        const ReceiveHistory* m_hist{nullptr};
        uint32_t              m_index{npos}; // 当前 element 索引或 npos = end
        Range                 m_range{};     // 当前区间 [low, high]

        const_iterator(const ReceiveHistory* hist, uint32_t index);
        void updateRange();
    };

    using iterator = const_iterator;

    const_iterator begin() const;
    const_iterator end() const;

private:
    struct Element {
        utp_packno_t    low;
        uint32_t        count; // 区间长度 = count, 代表 [low, low + count - 1]
        uint32_t        next;  // 下一个 Element 的索引, npos 表示无
    };
    static constexpr uint32_t npos = std::numeric_limits<uint32_t>::max();

    // 在 m_elements 中分配一个新元素索引
    uint32_t allocElement();

    // 将 idx 对应的元素从链表中移除并回收到 free list
    void removeElement(uint32_t idx);

    // 尝试与 head 链表中的区间合并/插入
    void insertIntoList(utp_packno_t pn);

    // 当区间数超过 m_maxRanges 时，丢弃最老区间
    void dropOldestRange();

    // 返回 elem 对应的 [low, high]
    static Range ElementRange(const Element &e) {
        Range r;
        r.low  = e.low;
        r.high = e.low + e.count - 1;
        return r;
    }

private:
    std::vector<Element>    m_elements;
    std::vector<uint32_t>   m_free;         // 空闲元素索引栈
    uint32_t                m_head{npos};   // 头元素索引
    uint32_t                m_usedCount{0}; // 当前使用的元素数量
    uint32_t                m_maxRanges;    // 最大区间数量

    // cutoff
    utp_packno_t            m_cutoff{0};

    // 记录当前收到的最大包号
    utp_packno_t            m_largest{0};
    utp_time_t              m_largestAckedReceived{0};

    // 用于迭代的游标
    uint32_t                m_iterNext{npos};
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_RECEIVE_HISTORY_H__
