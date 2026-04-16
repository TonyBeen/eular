/*************************************************************************
    > File Name: hash.cpp
    > Author: hsz
    > Brief:
    > Created Time: Fri 03 Feb 2023 10:35:35 AM CST
 ************************************************************************/

#include "utils/hash.h"

#include <string>
#include <exception>

#include "utils/alloc.h"
#include "utils/exception.h"

#define ERROR_MSG(msg) \
    (std::string(__func__) + "() " + (msg)).c_str()

// #if __WORDSIZE == 64
// const size_t offset_basis = 14695981039346656037ULL;
// const size_t prime = 1099511628211ULL;
// #elif __WORDSIZE == 32
// const size_t offset_basis = 2166136261U;
// const size_t prime = 16777619U;
// #else
// #error "undefined __WORDSIZE in hash.cpp"
// #endif

// const uint32_t offset_basis = 2166136261U;
// const uint32_t prime = 16777619U;

namespace eular {
/*
    The prime_deltas array contains the difference between a power
    of two and the next prime number:

    prime_deltas[i] = nextprime(2^i) - 2^i

    Basically, it's sequence A092131 from OEIS, assuming:
    - nextprime(1) = 1
    - nextprime(2) = 2
    and
    - left-extending it for the offset 0 (A092131 starts at i=1)
    - stopping the sequence at i = 28 (the table is big enough...)

    a(13)=17 because 2^13=8192 and the next prime is 8209=8192+17.
    http://oeis.org/A092131
*/

static const uint8_t prime_deltas[] = {
    0,  0,  1,  3,  1,  5,  3,  3,  1,  9,  7,  5,  3, 17, 27,  3,
    1, 29,  3, 21,  7, 17, 15,  9, 43, 35, 15, 29,  3, 11,  3, 11
};

/*
    The primeForNumBits() function returns the prime associated to a
    power of two. For example, primeForNumBits(8) returns 257.
*/
static inline int primeForNumBits(int numBits)
{
    return (1 << numBits) + prime_deltas[numBits];
}

/*
    Returns the smallest integer n such that
    primeForNumBits(n) >= hint.
*/
static int countBits(int hint)
{
    int numBits = 0;
    int bits = hint;

    while (bits > 1) {
        bits >>= 1;
        numBits++;
    }

    if (numBits >= (int)sizeof(prime_deltas)) {
        numBits = sizeof(prime_deltas) - 1;
    } else if (primeForNumBits(numBits) < hint) {
        ++numBits;
    }
    return numBits;
}

/**
 * hash模仿的QHash，下面是QHash的设计
 *
 * QHash采用二级指针, Node **, 是一个Node *数组, Node是单向链表节点,
 * 每个数组其内部存储的是链表的头节点，对于key不重复的hash表, 链表的头节点就是值
 * 当重复时会往链表插入, 相同的hash值会挨着
 *
 * 数组可以理解为n个桶, 每个桶放的是m个链表节点(当key不重复时m为2)
 *
 * QHash如何判断结尾呢？
 * 其设计很巧妙, 从源代码可以看到QHashData的第一个成员变量是fakeNext, 当将QHashData *转成Node *后
 * next变量的值就等于fakeNext的值，故此时node->next == nullptr
 * 这样可以让每个桶的链表结尾都指向QHashData *, 所以判断结尾就是判断是否和QHashData的地址是否相同
 *
 * rehash
 * rehash操作就是创建一个素数n大小的Node *数组, 然后遍历旧的数组，将链表根据hash值在插入到新的桶
 * 这样做就可以避免键值的拷贝
 */

static const uint8_t minNumBits = 4;
const HashData HashData::shared_null = {
    0,              // m_fakeNode
    {},             // m_buckets
    { UINT32_MAX }, // m_ref
    0,              // m_size
    0,              // m_nodeSize
    minNumBits,     // m_numBits
    0               // m_numBuckets
};

void *HashData::allocateNode(int align)
{
    void *ptr = AlignedAlloc(m_nodeSize, align);
    CHECK_PTR(ptr);
    return ptr;
}

void HashData::freeNode(void *node)
{
    AlignedFree(node);
}

HashData *HashData::detach_helper(void (*node_duplicate)(Node *, void *),
                                  void (*node_delete)(Node *),
                                  int anodeSize, int nodeAlign)
{
    union {
        HashData *d;
        Node *end;
    };
    d = new (std::nothrow)HashData();
    CHECK_PTR(d);

    d->m_fakeNode = nullptr;
    d->m_ref.ref();
    d->m_size = m_size;
    d->m_nodeSize = anodeSize;
    if (this == &shared_null) {
        d->m_numBits = minNumBits;
        d->m_numBuckets = primeForNumBits(minNumBits);
    } else {
        d->m_numBits = m_numBits;
        d->m_numBuckets = m_numBuckets;
    }

    try {
        d->m_buckets.resize(d->m_numBuckets, end);
    } catch (...) {
        d->m_numBuckets = 0;
        d->free_helper(node_delete);
        throw Exception(ERROR_MSG("std::vector resize error."));
    }

    if (m_numBuckets) {
        // NOTE QT的hash表采用单向链表来扩容冲突的值
        Node *this_node = reinterpret_cast<Node *>(this);
        for (int i = 0; i < m_numBuckets; ++i) {
            Node **newNode = &d->m_buckets[i];
            Node *oldNode = this->m_buckets[i];

            while (oldNode != this_node) {
                try {
                    Node *dup = static_cast<Node *>(allocateNode(nodeAlign));
                    try {
                        node_duplicate(oldNode, dup);
                    } catch (...) {
                        freeNode(dup);
                        throw Exception(ERROR_MSG("node_duplicate error"));
                    }
                    *newNode = dup;
                    newNode = &dup->m_next;
                    oldNode = oldNode->m_next;
                } catch (...) {
                    *newNode = end;
                    d->m_numBuckets = i + 1;
                    d->free_helper(node_delete);
                    throw Exception(ERROR_MSG("unknow error"));
                }
            }

            *newNode = end;
        }
    }

    return d;
}

/**
 * @brief 释放bucket的内容
 * @param node_delete 回调
 */
void HashData::free_helper(void (*node_delete)(Node *))
{
    if (node_delete) {
        Node *this_node = reinterpret_cast<Node *>(this);
        for (size_t i = 0; i < m_buckets.size(); ++i) {
            Node *curr = m_buckets[i];
            while (curr != this_node) {
                Node *next = curr->m_next;
                node_delete(curr);
                freeNode(curr);
                curr = next;
            }
        }
    }

    m_buckets.clear();
    delete this;
}

/**
 * @brief 
 * @param hint 大于0时表示bit数，小于0时表示容量
 */
void HashData::rehash(int hint)
{
    if (hint < 0) {
        hint = countBits(-hint);
        if (hint < minNumBits)
            hint = minNumBits;
        while (primeForNumBits(hint) < (m_numBuckets >> 1))
            ++hint;
    } else if (hint < minNumBits) {
        hint = minNumBits;
    }

    if (m_numBits != hint) {
        Node *end = reinterpret_cast<Node *>(this);
        int oldNumBuckets = m_numBuckets;

        int nb = primeForNumBits(hint);
        m_numBits = hint;
        m_numBuckets = nb;
        std::vector<Node *> newBuckets;
        try {
            newBuckets.resize(nb, end);
        } catch (...) {
            throw Exception(ERROR_MSG("std::vector resize error."));
        }

        for (int i = 0; i < oldNumBuckets; ++i) {
            Node *firstNode = m_buckets[i];
            while (firstNode != end) {
                uint32_t hash = firstNode->m_hash;
                Node *lastNode = firstNode;
                while (lastNode->m_next != end && lastNode->m_next->m_hash == hash) { // 找出相同hash值的末尾节点
                    lastNode = lastNode->m_next;
                }

                Node *afterLastNode = lastNode->m_next; // maybe afterLastNode == end
                Node **beforeFirstNode = &newBuckets[hash % m_numBuckets];
                while (*beforeFirstNode != end) { // 找到新桶的最后一个节点
                    beforeFirstNode = &(*beforeFirstNode)->m_next;
                }

                // 链表尾插
                lastNode->m_next = *beforeFirstNode;
                *beforeFirstNode = firstNode;
                firstNode = afterLastNode;
            }
        }

        m_buckets.swap(newBuckets);
    }
}

bool HashData::grow(float maxLoadFactor)
{
    if (maxLoadFactor <= 0.0f) {
        maxLoadFactor = 1.0f;
    }

    const int threshold = (std::max)(1, static_cast<int>(m_numBuckets * maxLoadFactor));
    if (m_size >= threshold) {
        rehash(m_numBits + 1);
        return true;
    }

    return false;
}

HashData::Node *HashData::firstNode()
{
    Node *end = reinterpret_cast<Node *>(this);
    for (int i = 0; i < m_numBuckets; ++i) {
        Node *bucket = this->m_buckets[i];
        if (bucket != end) {
            return bucket;
        }
    }

    return end;
}

HashData::Node *HashData::nextNode(Node *node)
{
    assert(node);
    Node *next = node->m_next;
    assert(next);

    if (next->m_next) { // 如果next是最后节点HashData，则next->next == nullptr
        return next;
    }

    HashData *data = reinterpret_cast<HashData *>(next);
    Node *end = next;
    int start = (node->m_hash % data->m_numBuckets) + 1;
    for (int i = start; i < data->m_numBuckets; ++i) {
        Node *bucket = data->m_buckets[i];
        if (bucket != end) {
            return bucket;
        }
    }

    return end; // 未找到就返回结尾
}

HashData::Node *HashData::previousNode(Node *node)
{
    union {
        Node *end;
        HashData *data;
    };

    end = node;
    while (end->m_next) { // 找出HashData的地址
        end = end->m_next;
    }

    int start = 0;
    if (node == end) { // 如果node是结尾，返回最后一个桶的索引
        start = data->m_numBuckets - 1;
    } else { // 否则就找出当前node所在的桶
        start = node->m_hash % data->m_numBuckets;
    }

    Node *sentinel = node;
    Node *bucket = nullptr;
    while (start >= 0) { // TODO 当node为第一个桶的第一个节点时
        bucket = data->m_buckets[start];
        if (bucket != sentinel) { // 参数node不为桶的第一个元素时
            Node *prev = bucket;
            while (prev->m_next != sentinel) {
                prev = prev->m_next;
            }
            return prev;
        }

        sentinel = end;
        --start;
    }

    return end;
}

} // namespace eular
