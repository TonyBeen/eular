/*************************************************************************
    > File Name: receive_history.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 29 Jan 2026 03:01:02 PM CST
 ************************************************************************/

#include "util/receive_history.h"

#include <assert.h>

namespace eular {
namespace utp {
ReceiveHistory::ReceiveHistory(uint32_t maxRanges) :
    m_maxRanges(maxRanges)
{
    m_elements.reserve(maxRanges);
    m_free.reserve(maxRanges);
}

void ReceiveHistory::insert(utp_packno_t pn, utp_time_t now)
{
    if (m_cutoff && pn < m_cutoff) {
        return;
    }

    if (pn > m_largest) {
        m_largest = pn;
        m_largestAckedReceived = now;
    }

    insertIntoList(pn);

    if (m_usedCount > m_maxRanges) {
        dropOldestRange();
    }
}

void ReceiveHistory::stopWait(utp_packno_t cutoff)
{
    if (m_cutoff >= cutoff) {
        return;
    }

    m_cutoff = cutoff;
    if (m_head == npos) {
        return;
    }

    uint32_t prev = npos;
    uint32_t cur  = m_head;

    for (;;) {
        Element &e = m_elements[cur];
        Range r = ElementRange(e);

        if (cutoff > r.high) {
            // 整个区间都在 cutoff 左边, 删除整个区间
            if (prev == npos) { // 删除头部
                m_head = e.next;
            } else {
                m_elements[prev].next = e.next;
            }
            removeElement(cur);
        } else if (r.low < cutoff) {
            // 区间部分在 cutoff 左边, 向右调整 low
            uint32_t delta = static_cast<uint32_t>(cutoff - r.low);
            e.low += delta;
            e.count -= delta;
            break;
        } else {
            // 区间在 cutoff 右边, 无需处理
            break;
        }

        if (e.next == npos) {
            return;
        }
        prev = cur;
        cur = e.next;
    }
}

void ReceiveHistory::clear()
{
    m_elements.clear();
    m_free.clear();
    m_head                  = npos;
    m_usedCount             = 0;
    m_cutoff                = 0;
    m_largest               = 0;
    m_largestAckedReceived  = 0;
    m_iterNext              = npos;
}

ReceiveHistory::const_iterator ReceiveHistory::begin() const
{
    return const_iterator(this, m_head);
}

ReceiveHistory::const_iterator ReceiveHistory::end() const
{
    return const_iterator(this, npos);
}

uint32_t ReceiveHistory::allocElement()
{
    uint32_t idx;
    if (!m_free.empty()) {
        idx = m_free.back();
        m_free.pop_back();
    } else {
        idx = static_cast<uint32_t>(m_elements.size());
        m_elements.push_back(Element{0, 0, npos});
    }

    ++m_usedCount;
    return idx;
}

void ReceiveHistory::removeElement(uint32_t idx)
{
    m_elements[idx].next = npos;
    m_free.push_back(idx);
    assert(m_usedCount > 0);
    --m_usedCount;
}

void ReceiveHistory::insertIntoList(utp_packno_t pn)
{
    // 链表为空, 创建第一个区间 [pn, pn]
    if (m_head == npos) {
        uint32_t idx = allocElement();
        m_elements[idx].low   = pn;
        m_elements[idx].count = 1;
        m_elements[idx].next  = npos;
        m_head = idx;
        return;
    }

    uint32_t prev = npos;
    uint32_t cur  = m_head;

    while (cur != npos) {
        Element &e = m_elements[cur];
        utp_packno_t low  = e.low;
        utp_packno_t high = static_cast<utp_packno_t>(e.low + e.count - 1);

        // 已经在当前区间内, 无需处理
        if (pn >= low && pn <= high) {
            return;
        }

        // 向左扩展：pn 紧挨当前区间左边 => [pn, high]
        if (pn + 1 == low) {
            e.low = pn;
            e.count = static_cast<uint32_t>(high - pn + 1);

            // 与前一个区间是否可合并 (前一个区间的 high + 1 == e.low)
            if (prev != npos) {
                Element &pe = m_elements[prev];
                utp_packno_t plow  = pe.low;
                utp_packno_t phigh = static_cast<utp_packno_t>(pe.low + pe.count - 1);
                if (phigh + 1 == e.low) {
                    pe.count = static_cast<uint32_t>(e.low + e.count - plow);
                    pe.next  = e.next;
                    removeElement(cur);
                }
            }
            return;
        }

        // 向右扩展：pn 紧挨当前区间右边 => [low, pn]
        if (pn == high + 1) {
            e.count = static_cast<uint32_t>(pn - low + 1);

            // 与下一段是否可合并 (high + 1 == next.low)
            uint32_t next = e.next;
            if (next != npos) {
                Element &ne = m_elements[next];
                utp_packno_t nlow  = ne.low;
                utp_packno_t nhigh = static_cast<utp_packno_t>(ne.low + ne.count - 1);
                if (high + 1 == nlow) {
                    e.count = static_cast<uint32_t>(nhigh - low + 1);
                    e.next  = ne.next;
                    removeElement(next);
                }
            }
            return;
        }

        // 当前区间 high < pn, 说明 pn 比当前区间更大
        // 区间按 high 降序存放, 应插在当前区间之前
        if (high < pn) {
            break;
        }

        prev = cur;
        cur  = e.next;
    }

    // 需要新建一个 [pn, pn] 区间
    uint32_t idx = allocElement();
    m_elements[idx].low   = static_cast<utp_packno_t>(pn);
    m_elements[idx].count = 1;
    m_elements[idx].next  = cur;

    if (prev == npos) { // 插在链表头部
        m_head = idx;
    } else {
        m_elements[prev].next = idx;
    }
}

void ReceiveHistory::dropOldestRange()
{
    if (m_head == npos) {
        return;
    }

    uint32_t prev = npos;
    uint32_t cur  = m_head;

    // 遍历到链表尾, 最后一个元素的包号最小
    while (m_elements[cur].next != npos) {
        prev = cur;
        cur  = m_elements[cur].next;
    }

    Element &e = m_elements[cur];
    Range r = ElementRange(e);

    // cutoff: <= 该区间 high 的包号视为 "已经不需要精确信息"
    m_cutoff = r.high + 1;

    if (prev == npos) { // 只有一个元素
        m_head = npos;
    } else {
        m_elements[prev].next = npos;
    }
    removeElement(cur);
}

ReceiveHistory::const_iterator::const_iterator(const ReceiveHistory* hist, uint32_t index) :
    m_hist(hist),
    m_index(index)
{
    if (m_hist && m_index != npos) {
        updateRange();
    }
}

void ReceiveHistory::const_iterator::updateRange()
{
    if (!m_hist || m_index == npos) {
        m_range = Range{0, 0};
        return;
    }

    const ReceiveHistory::Element &e = m_hist->m_elements[m_index];
    m_range = ElementRange(e);
}

} // namespace utp
} // namespace eular
