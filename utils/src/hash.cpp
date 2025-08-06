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

const uint32_t offset_basis = 2166136261U;
const uint32_t prime = 16777619U;

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
    0,              // fakeNode
    {},             // buckets
    { UINT32_MAX }, // ref
    0,              // size
    0,              // nodeSize
    minNumBits,     // numBits
    0               // numBuckets
};

void *HashData::allocateNode(int align)
{
    void *ptr = AlignedMalloc(nodeSize, align);
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

    d->fakeNode = nullptr;
    d->ref.ref();
    d->size = size;
    d->nodeSize = anodeSize;
    if (this == &shared_null) {
        d->numBits = minNumBits;
        d->numBuckets = primeForNumBits(minNumBits);
    } else {
        d->numBits = numBits;
        d->numBuckets = numBuckets;
    }

    try {
        d->buckets.resize(d->numBuckets, end);
    } catch (...) {
        d->numBuckets = 0;
        d->free_helper(node_delete);
        throw std::runtime_error(ERROR_MSG("std::vector resize error."));
    }

    if (numBuckets) {
        // NOTE QT的hash表采用单向链表来扩容冲突的值
        Node *this_node = reinterpret_cast<Node *>(this);
        for (int i = 0; i < numBuckets; ++i) {
            Node **newNode = &d->buckets[i];
            Node *oldNode = this->buckets[i];

            while (oldNode != this_node) {
                try {
                    Node *dup = static_cast<Node *>(allocateNode(nodeAlign));
                    try {
                        node_duplicate(oldNode, dup);
                    } catch (...) {
                        freeNode(dup);
                        throw std::runtime_error(ERROR_MSG("node_duplicate error"));
                    }
                    *newNode = dup;
                    newNode = &dup->next;
                    oldNode = oldNode->next;
                } catch (...) {
                    *newNode = end;
                    d->numBuckets = i + 1;
                    d->free_helper(node_delete);
                    throw std::runtime_error(ERROR_MSG("unknow error"));
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
        for (size_t i = 0; i < buckets.size(); ++i) {
            Node *curr = buckets[i];
            while (curr != this_node) {
                Node *next = curr->next;
                node_delete(curr);
                freeNode(curr);
                curr = next;
            }
        }
    }

    buckets.clear();
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
        while (primeForNumBits(hint) < (numBuckets >> 1))
            ++hint;
    } else if (hint < minNumBits) {
        hint = minNumBits;
    }

    if (numBits != hint) {
        Node *end = reinterpret_cast<Node *>(this);
        int oldNumBuckets = numBuckets;

        int nb = primeForNumBits(hint);
        numBits = hint;
        numBuckets = nb;
        std::vector<Node *> newBuckets;
        try {
            newBuckets.resize(nb, end);
        } catch (...) {
            throw std::runtime_error(ERROR_MSG("std::vector resize error."));
        }

        for (int i = 0; i < oldNumBuckets; ++i) {
            Node *firstNode = buckets[i];
            while (firstNode != end) {
                uint32_t hash = firstNode->hash;
                Node *lastNode = firstNode;
                while (lastNode != end && lastNode->next->hash == hash) { // 找出相同hash值的末尾节点
                    lastNode = lastNode->next;
                }

                Node *afterLastNode = lastNode->next; // maybe afterLastNode == end
                Node **beforeFirstNode = &newBuckets[hash % numBuckets];
                while (*beforeFirstNode != end) { // 找到新桶的最后一个节点
                    beforeFirstNode = &(*beforeFirstNode)->next;
                }

                // 链表尾插
                lastNode->next = *beforeFirstNode;
                *beforeFirstNode = firstNode;
                firstNode = afterLastNode;
            }
        }

        buckets.swap(newBuckets);
    }
}

bool HashData::grow()
{
    if (size >= numBuckets) {
        rehash(numBits + 1);
        return true;
    }

    return false;
}

HashData::Node *HashData::firstNode()
{
    Node *end = reinterpret_cast<Node *>(this);
    for (int i = 0; i < numBuckets; ++i) {
        Node *bucket = this->buckets[i];
        if (bucket != end) {
            return bucket;
        }
    }

    return end;
}

HashData::Node *HashData::nextNode(Node *node)
{
    assert(node);
    union {
        Node *next;
        Node *e;
        HashData *d;
    };

    next = node->next;
    assert(next);

    printf("node = %p, next = %p, next->next = %p\n", node, next, next->next);
    if (next->next) { // 如果next是最后节点HashData，则next->next == nullptr
        return next;
    }

    // 此时next为HashData的内存地址
    int start = (node->hash % d->numBuckets) + 1;
    printf("start = %d, buckets = %d\n", start, d->numBuckets);
    if (start < d->numBuckets) {
        Node *bucket = d->buckets[start]; // TODO 判断start是否会等于numBuckets
        int n = d->numBuckets - start;
        while (n--) { // 找到下一个存在数据的桶
            if (bucket != e) {
                printf("bucket = %p\n", bucket);
                return bucket;
            }
            ++bucket;
        }
    }

    return e; // 未找到就返回结尾
}

HashData::Node *HashData::previousNode(Node *node)
{
    union {
        Node *end;
        HashData *data;
    };

    end = node;
    while (end->next) { // 找出HashData的地址
        end = end->next;
    }

    int start = 0;
    if (node == end) { // 如果node是结尾，返回最后一个桶的索引
        start = data->numBuckets - 1;
    } else { // 否则就找出当前node所在的桶
        start = node->hash % data->numBuckets;
    }

    Node *sentinel = node;
    Node *bucket = nullptr;
    while (start >= 0) { // TODO 当node为第一个桶的第一个节点时
        bucket = data->buckets[start];
        if (bucket != sentinel) { // 参数node不为桶的第一个元素时
            Node *prev = bucket;
            while (prev->next != sentinel) {
                prev = prev->next;
            }
            return prev;
        }

        sentinel = end;
        --start;
    }

    return end;
}

uint32_t HashCmptBase::compute(const uint8_t *key, uint32_t size)
{
    uint32_t _Val = offset_basis;
    for (size_t _Idx = 0; _Idx < size; ++_Idx) {
        _Val ^= static_cast<uint32_t>(key[_Idx]);
        _Val *= prime;
    }

    return _Val;
}

static inline uint32_t ReadU32(const void *p)
{
    uint32_t tmp;
    memcpy(&tmp, p, sizeof tmp);
    return tmp;
}

uint32_t HashCmptBase::compute2(const void *key, uint32_t size)
{
    uint32_t hash = 0;
    uint32_t n = size;
    while (n >= 4) {
        hash ^= ReadU32(key);
        key = (uint8_t *)key + sizeof(uint32_t);
        hash = (hash << 13) | (hash >> 19);
        n -= 4;
    }
    while (n != 0) {
        hash ^= *(uint8_t *)key;
        key = (uint8_t *)key + sizeof(uint8_t);
        hash = (hash << 8) | (hash >> 24);
        n--;
    }
    return hash;
}

} // namespace eular
