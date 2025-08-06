/*************************************************************************
    > File Name: bitmap.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2022-05-23 09:42:46 Monday
 ************************************************************************/

#include "utils/bitmap.h"

#include <stdlib.h>
#include <string.h>

#include "utils/exception.h"
#include "utils/sysdef.h"

#define DEFAULT_SIZE    (256)

namespace eular {
static const uint16_t POS_SIZE = sizeof(uint8_t) * BITS_PEER_BYTE;
static uint8_t POS[POS_SIZE];
static uint8_t NPOS[POS_SIZE];

static bool gInit = [] () -> bool {
    // NOTE 从低位开始排列, 比如索引为1时, 取的是0x02
    for (uint32_t i = 0; i < POS_SIZE; ++i) {
        POS[i] = ((uint8_t)1) << i;
        NPOS[i] = ~POS[i];
    }

    return true;
}();

BitMap::BitMap() :
    mBitMap(nullptr),
    mSize(0),
    mCapacity(0)
{
    mBitMap = alloc(DEFAULT_SIZE);
}

BitMap::BitMap(uint32_t bitSize) :
    mBitMap(nullptr),
    mSize(0),
    mCapacity(0)
{
    if (bitSize == 0) {
        bitSize = DEFAULT_SIZE;
    }

    resize(bitSize);
}

BitMap::BitMap(const BitMap &other) :
    mBitMap(nullptr),
    mSize(0),
    mCapacity(0)
{
    if (other.mBitMap && other.mCapacity) {
        mBitMap = alloc(other.mCapacity);
        memcpy(mBitMap, other.mBitMap, mCapacity / BITS_PEER_BYTE);
        mSize = other.mSize;
    }
}

BitMap::~BitMap()
{
    release();
}

BitMap &BitMap::operator=(const BitMap &other)
{
    if (this != std::addressof(other)) {
        if (other.mBitMap && other.mCapacity) {
            mBitMap = alloc(other.mCapacity);
            memcpy(mBitMap, other.mBitMap, mCapacity / BITS_PEER_BYTE);
            mSize = other.mSize;
        }
    }

    return *this;
}

bool BitMap::set(uint32_t idx, bool v)
{
    if (idx >= mCapacity) {
        return false;
    }
    nullThrow();

    uint32_t index = idx / BITS_PEER_BYTE;
    uint32_t pos = idx % BITS_PEER_BYTE;

    uint8_t& value = mBitMap[index];
    if (v) {
        value |= POS[pos];
    } else {
        value &= NPOS[pos];
    }

    return true;
}

bool BitMap::at(uint32_t idx) const
{
    if (idx >= mCapacity) {
        return false;
    }
    nullThrow();

    uint32_t index = idx / BITS_PEER_BYTE;
    uint32_t pos = idx % BITS_PEER_BYTE;

    return mBitMap[index] & POS[pos];
}

void BitMap::clear()
{
    memset(mBitMap, 0, mCapacity / BITS_PEER_BYTE);
}

uint32_t popcount(uint32_t n)
{
#ifdef OS_WINDOWS
    // https://blog.csdn.net/github_38148039/article/details/109598368
    n = (n & 0x55555555) + ((n >> 1) & 0x55555555);
    n = (n & 0x33333333) + ((n >> 2) & 0x33333333);
    n = (n & 0x0F0F0F0F) + ((n >> 4) & 0x0F0F0F0F);
    n = (n & 0x00FF00FF) + ((n >> 8) & 0x00FF00FF);
    n = (n & 0x0000FFFF) + ((n >> 16) & 0x0000FFFF);
    return n;
#elif defined(OS_LINUX)
    return __builtin_popcount(n);
#endif
}

template<class T>
T count_bits(const T& v)
{
    return popcount(v);
}

template<>
uint64_t count_bits(const uint64_t& v)
{
    static const uint32_t uintMax = (uint32_t)(~0);
    return (popcount(v & uintMax) + popcount(v >> 32));
}

uint32_t BitMap::count() const
{
    uint32_t bytes =  mCapacity / BITS_PEER_BYTE;
    uint32_t len = bytes / sizeof(uint64_t);
    uint32_t reserveSize = bytes % sizeof(uint64_t);
    uint32_t count = 0;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < len; ++i) {
        uint64_t *pU64Vec = reinterpret_cast<uint64_t *>(mBitMap);
        uint64_t temp = pU64Vec[i];
        count += count_bits(temp);
        offset += sizeof(uint64_t);
    }

    for (uint32_t i = 0; i < reserveSize; ++i) {
        if (mBitMap[offset + i]) {
            count += count_bits(mBitMap[offset + i]);
        }
    }

    return count;
}

uint32_t BitMap::size() const
{
    return mSize;
}

uint32_t BitMap::capacity() const
{
    return mCapacity;
}

bool BitMap::resize(uint32_t bitSize)
{
    if (bitSize <= mCapacity) {
        mSize = bitSize;
        return true;
    }

    uint32_t oldCap = mCapacity;
    uint8_t* newBitMap = alloc(bitSize);
    if (newBitMap == nullptr) {
        return false;
    }

    mSize = bitSize;

    uint32_t bytes = (oldCap > mCapacity) ? (mCapacity / BITS_PEER_BYTE) : (oldCap / BITS_PEER_BYTE);
    memcpy(newBitMap, mBitMap, bytes);

    release();
    mBitMap = newBitMap;
    return true;
}

/**
 * @brief 申请一块内存
 *
 * @param size bitmap容量
 * @return 失败为nullptr
 */
uint8_t* BitMap::alloc(uint32_t bitSize)
{
    uint32_t bytes = (bitSize) / (sizeof(uint8_t) * BITS_PEER_BYTE);
    if (bitSize % (sizeof(uint8_t) * BITS_PEER_BYTE)) {
        ++bytes;
    }

    mCapacity = 0;
    uint8_t* bitMap = (uint8_t *)malloc(bytes);
    if (bitMap != nullptr) {
        memset(bitMap, 0x00, bytes);
        mCapacity = bytes * BITS_PEER_BYTE;
        mSize = bytes * BITS_PEER_BYTE;
    }

    return bitMap;
}

void BitMap::release()
{
    if (mBitMap) {
        free(mBitMap);
        mBitMap = nullptr;
    }
}

void BitMap::nullThrow() const
{
    if (mBitMap == nullptr) {
        throw Exception("using null pointers");
    }
}

} // namespace eular
